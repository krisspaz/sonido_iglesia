from __future__ import annotations

import os
import tempfile
from pathlib import Path
from .analyzer import analyze_file
from .learning import LocalLearning
from .reference import reference_match
from .advanced import analyze_full, refine_with_transcript
from .ai import separate, transcribe
from .jobs import LocalJobManager


def create_app():
    try:
        from fastapi import FastAPI, File, Header, HTTPException, UploadFile
    except ImportError as exc:
        raise RuntimeError("Instala el extra del servidor: pip install -e '.[server]'") from exc
    app = FastAPI(title="Church Sound Analyst", version="0.2.0")
    token = os.getenv("CHURCH_ANALYZER_TOKEN")
    token_file = os.getenv("CHURCH_ANALYZER_TOKEN_FILE")
    if not token and token_file:
        try:
            token = Path(token_file).read_text(encoding="utf-8").strip()
        except OSError:
            token = None
    data_root = Path(os.getenv("CHURCH_ANALYZER_DATA_DIR", Path.home() / ".church-stream-processor")).resolve()
    data_root.mkdir(parents=True, exist_ok=True)
    learning = LocalLearning(os.getenv("CHURCH_ANALYZER_DB", str(data_root / "church-sound-learning.sqlite3")))
    jobs = LocalJobManager(max_workers=int(os.getenv("CHURCH_ANALYZER_WORKERS", "1")))
    allowed_roots = [Path(item).expanduser().resolve() for item in os.getenv("CHURCH_ANALYZER_ALLOWED_ROOTS", "").split(os.pathsep) if item]

    def require_token(value: str | None) -> None:
        if not token:
            raise HTTPException(status_code=503, detail="Token local no configurado")
        if value != token:
            raise HTTPException(status_code=401, detail="Token inválido")

    def checked_path(raw: object) -> Path:
        if not isinstance(raw, str) or not raw or len(raw) > 4096:
            raise HTTPException(status_code=400, detail="path inválido")
        path = Path(raw).expanduser().resolve()
        if path.suffix.lower() not in {".wav", ".flac", ".aif", ".aiff"} or not path.is_file():
            raise HTTPException(status_code=400, detail="Archivo de audio no encontrado")
        if allowed_roots and not any(path.is_relative_to(root) for root in allowed_roots):
            raise HTTPException(status_code=403, detail="Archivo fuera de las rutas locales autorizadas")
        if path.stat().st_size > 500 * 1024 * 1024:
            raise HTTPException(status_code=413, detail="Archivo demasiado grande")
        return path

    def authorized(headers) -> bool:
        return bool(token) and headers.get("x-api-token") == token

    @app.get("/health")
    def health():
        return {"ok": True, "service": "church-analyzer", "version": "0.2.0", "authenticated": bool(token),
                "data_dir": str(data_root), "jobs": True}

    @app.post("/analyze")
    async def analyze(upload: UploadFile = File(...), x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        suffix = Path(upload.filename or "").suffix.lower()
        if suffix not in {".wav", ".flac", ".aif", ".aiff"}:
            raise HTTPException(status_code=415, detail="Formato de audio no permitido")
        data = await upload.read()
        if len(data) > 500 * 1024 * 1024:
            raise HTTPException(status_code=413, detail="Archivo demasiado grande")
        with tempfile.NamedTemporaryFile(prefix="church-analyzer-", suffix=suffix, dir=data_root, delete=False) as handle:
            handle.write(data)
            temp = Path(handle.name)
        try:
            return analyze_full(str(temp))
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

    def submit(kind: str, work):
        try:
            return {"job": jobs.submit(kind, work).public()}
        except RuntimeError as exc:
            raise HTTPException(status_code=429, detail=str(exc)) from exc

    @app.post("/jobs/analyze")
    def analyze_job(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        path = str(checked_path(request.get("path")))
        processed = str(checked_path(request["processed_path"])) if request.get("processed_path") else None
        return submit("analyze", lambda update, cancelled: analyze_full(path, processed) if not cancelled.is_set() else {"cancelled": True})

    @app.post("/jobs/ai-analysis")
    def ai_analysis_job(request: dict, x_api_token: str | None = Header(default=None)):
        """Optional local composite job: report + Whisper (+ Demucs if selected)."""
        require_token(x_api_token)
        path, model = str(checked_path(request.get("path"))), str(request.get("model", "small"))
        use_demucs = bool(request.get("separate_sources", False))

        def work(update, cancelled):
            update(.05, "Midiendo grabación")
            report = analyze_full(path)
            if cancelled.is_set():
                return {"cancelled": True}
            update(.2, "Transcribiendo prédica")
            transcript = transcribe(path, model, lambda value, message: update(.2 + value * .35, message), cancelled)
            if transcript.get("available"):
                report = refine_with_transcript(report, transcript)
                report["transcript"] = transcript
            if use_demucs and not cancelled.is_set():
                destination = data_root / "stems" / Path(path).stem
                report["source_separation"] = separate(path, str(destination), True,
                                                        lambda value, message: update(.55 + value * .4, message), cancelled)
            return report
        return submit("ai-analysis", work)

    @app.get("/jobs/{job_id}")
    def job_status(job_id: str, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        job = jobs.get(job_id)
        if not job:
            raise HTTPException(status_code=404, detail="Trabajo no encontrado")
        return {"job": job.public()}

    @app.delete("/jobs/{job_id}")
    def cancel_job(job_id: str, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        job = jobs.cancel(job_id)
        if not job:
            raise HTTPException(status_code=404, detail="Trabajo no encontrado")
        return {"job": job.public()}

    @app.post("/transcribe")
    def transcribe_endpoint(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        try:
            path, model = str(checked_path(request.get("path"))), str(request.get("model", "small"))
            if request.get("background", False):
                return submit("transcribe", lambda update, cancelled: transcribe(path, model, update, cancelled))
            return transcribe(path, model)
        except (OSError, ValueError) as exc:
            raise HTTPException(status_code=422, detail=str(exc)) from exc

    @app.post("/separate")
    def separate_endpoint(request: dict, x_api_token: str | None = Header(default=None)):
        require_token(x_api_token)
        try:
            path = str(checked_path(request.get("path")))
            # Demucs output is always in service-owned storage, never an arbitrary path supplied over HTTP.
            destination = data_root / "stems" / Path(path).stem
            work = lambda update, cancelled: separate(path, str(destination), bool(request.get("two_stems", False)), update, cancelled)
            if request.get("background", False):
                return submit("separate", work)
            return work(lambda *_: None, __import__("threading").Event())
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
            source = analyze_full(str(checked_path(request.get("source_path"))))
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

    @app.on_event("shutdown")
    def shutdown_jobs():
        jobs.shutdown()

    return app
