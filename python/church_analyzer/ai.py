from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path


_MODELS = {"tiny", "base", "small", "medium", "large", "turbo"}


def transcribe(path: str, model: str = "small", progress=None, cancelled=None) -> dict:
    if model not in _MODELS:
        raise ValueError("modelo Whisper no permitido")
    try:
        import whisper
    except ImportError as exc:
        return {"available": False, "error": "Instala el extra ai para usar Whisper"}
    if cancelled and cancelled.is_set():
        return {"available": False, "cancelled": True}
    if progress:
        progress(.1, "Cargando Whisper")
    # Downloads are controlled by the package/model cache, never by the audio
    # engine.  Operators may pre-seed this cache in the Windows installer.
    result = whisper.load_model(model).transcribe(path, fp16=False, language="es", verbose=False)
    if progress:
        progress(.95, "Preparando transcripción")
    return {"available": True, "language": result.get("language"),
            "text": result.get("text", ""), "model": model,
            "segments": [{"start": s.get("start"), "end": s.get("end"), "text": s.get("text", "").strip()}
                         for s in result.get("segments", [])]}


def separate(path: str, output_dir: str, two_stems: bool = False, progress=None, cancelled=None) -> dict:
    """Runs Demucs only when explicitly requested; no audio is uploaded."""
    try:
        import demucs  # noqa: F401
    except ImportError:
        return {"available": False, "error": "Instala el extra ai para usar Demucs"}
    destination = Path(output_dir).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, "-m", "demucs.separate", "-o", str(destination), str(Path(path).resolve())]
    if two_stems:
        command.extend(["--two-stems", "vocals"])
    if progress:
        progress(.05, "Iniciando Demucs")
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + 3600
    try:
        while process.poll() is None:
            if cancelled and cancelled.is_set():
                process.terminate()
                process.wait(timeout=10)
                return {"available": False, "cancelled": True, "output_dir": str(destination)}
            if progress:
                progress(.5, "Separando fuentes con Demucs")
            if time.monotonic() >= deadline:
                process.terminate()
                process.wait(timeout=10)
                return {"available": False, "timed_out": True, "output_dir": str(destination),
                        "error": "Demucs superó el límite de una hora"}
            # poll at a bounded interval without blocking service shutdown.
            try:
                process.wait(timeout=.25)
            except subprocess.TimeoutExpired:
                continue
    except Exception:
        process.kill()
        raise
    stdout, stderr = process.communicate(timeout=30)
    if progress:
        progress(.95, "Finalizando stems")
    return {"available": process.returncode == 0, "return_code": process.returncode,
            "output_dir": str(destination), "stderr": stderr[-2000:], "stdout": stdout[-1000:]}
