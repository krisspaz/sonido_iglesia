from __future__ import annotations

import os
import tempfile
from pathlib import Path
from .analyzer import analyze_file
from .learning import LocalLearning
from .reference import reference_match
from .advanced import analyze_full
from .ai import separate, transcribe


def create_app():
    try:
        from fastapi import FastAPI, File, Header, HTTPException, UploadFile
    except ImportError as exc:
        raise RuntimeError("Instala el extra del servidor: pip install -e '.[server]'") from exc
    app = FastAPI(title="Church Stream Analyzer", version="0.1.0")
    token = os.getenv("CHURCH_ANALYZER_TOKEN")
    token_file = os.getenv("CHURCH_ANALYZER_TOKEN_FILE")
    if not token and token_file:
        try:
            token = Path(token_file).read_text(encoding="utf-8").strip()
        except OSError:
            token = None
    learning = LocalLearning(os.getenv("CHURCH_ANALYZER_DB", "church-sound-learning.sqlite3"))

    def require_token(value: str | None) -> None:
        if not token:
            raise HTTPException(status_code=503, detail="Token local no configurado")
        if value != token:
            raise HTTPException(status_code=401, detail="Token inválido")

    def checked_path(raw: object) -> Path:
        if not isinstance(raw, str) or not raw or len(raw) > 4096:
            raise HTTPException(status_code=400, detail="path inválido")
        path = Path(raw).expanduser()
        if path.suffix.lower() not in {".wav", ".flac", ".aif", ".aiff"} or not path.is_file():
            raise HTTPException(status_code=400, detail="Archivo de audio no encontrado")
        if path.stat().st_size > 500 * 1024 * 1024:
            raise HTTPException(status_code=413, detail="Archivo demasiado grande")
        return path

    def authorized(headers) -> bool:
        return bool(token) and headers.get("x-api-token") == token

    @app.get("/health")
    def health():
        return {"ok": True, "service": "church-analyzer"}

    @app.post("/analyze")
    async def analyze(upload: UploadFile = File(...)):
        if not token:
            raise HTTPException(status_code=503, detail="Token local no configurado")
        if not authorized(upload.headers):
            raise HTTPException(status_code=401, detail="Token inválido")
        if upload.content_type not in ("audio/wav", "audio/x-wav", "application/octet-stream"):
            raise HTTPException(status_code=415, detail="Sólo WAV PCM por ahora")
        data = await upload.read()
        if len(data) > 500 * 1024 * 1024:
            raise HTTPException(status_code=413, detail="Archivo demasiado grande")
        with tempfile.NamedTemporaryFile(prefix="church-analyzer-", suffix=".wav", delete=False) as handle:
            handle.write(data)
            temp = Path(handle.name)
        try:
            return analyze_file(temp).to_dict()
        finally:
            temp.unlink(missing_ok=True)

    @app.post("/analyze-path")
    def analyze_path(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        path = checked_path(request.get("path") if isinstance(request, dict) else None)
        try:
            return analyze_file(path).to_dict()
        except (OSError, ValueError) as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc

    @app.post("/analyze-full")
    def analyze_full_endpoint(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        try:
            return analyze_full(str(checked_path(request.get("path"))),
                                str(checked_path(request["processed_path"])) if request.get("processed_path") else None)
        except (KeyError, OSError, ValueError) as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc

    @app.post("/transcribe")
    def transcribe_endpoint(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        try:
            return transcribe(str(checked_path(request.get("path"))), str(request.get("model", "small")))
        except (OSError, ValueError) as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc

    @app.post("/separate")
    def separate_endpoint(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        try:
            return separate(str(checked_path(request.get("path"))), str(request["output_dir"]), bool(request.get("two_stems", False)))
        except (KeyError, OSError, ValueError) as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc

    @app.post("/recommendations/preview")
    def recommendation_preview(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        report = analyze_full(str(checked_path(request.get("path"))))
        preference = learning.preference(str(request.get("church", "default")))
        recommendations = []
        for item in report.get("recommendations", []):
            if item.get("gain_db") is not None:
                item["gain_db"] = max(-3.0, min(3.0, item["gain_db"]))
            if item.get("kind") in preference["accepted_modules"]:
                item["confidence"] = min(.95, float(item.get("confidence", 0)) + .05)
            if item.get("kind") == "loudness_target" and preference["loudness_target"] is not None:
                item["gain_db"] = max(-18.0, min(-10.0, preference["loudness_target"]))
            recommendations.append(item)
        return {"requires_confirmation": True, "recommendations": recommendations,
                "local_preference": preference, "message": "Vista previa solamente; no cambia el DSP."}

    @app.post("/recommendations/apply")
    def recommendation_apply(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        if request.get("confirm") is not True:
            raise HTTPException(status_code=400, detail="Se requiere confirm=true")
        church = str(request.get("church", "default"))
        accepted = list(request.get("recommendations", []))
        learning.add(church, "acceptable", {}, accepted)
        return {"accepted": len(accepted), "applied_to_dsp": False,
                "message": "Guardado como ajuste aceptado; la aplicación al DSP requiere confirmación en C++."}

    @app.post("/reference-match")
    def match(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        try:
            source = analyze_file(checked_path(request.get("source_path")))
            return reference_match(source, str(checked_path(request.get("reference_path"))))
        except (OSError, ValueError) as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc

    @app.post("/feedback")
    def feedback(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        try:
            church = str(request.get("church", "default"))
            learning.add(church, str(request["rating"]), dict(request.get("metrics", {})),
                         list(request.get("accepted", [])))
            return {"ok": True, "summary": learning.summary(church)}
        except (KeyError, TypeError, ValueError) as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

    @app.get("/learning")
    def learning_summary(church: str = "default", x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        return {"church": church, "summary": learning.summary(church), "preference": learning.preference(church)}

    @app.delete("/learning")
    def learning_reset(x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        learning.reset()
        return {"ok": True, "message": "Historial local borrado"}

    @app.post("/learning/reset")
    def learning_reset_post(x_api_token: str | None = Header(default=None)):
        return learning_reset(x_api_token)

    return app
