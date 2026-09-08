"""Bounded, local-only background work for expensive offline analysis.

The service deliberately keeps this separate from the C++ audio process.  A
job is best-effort: cancelling or losing the Python service can never affect
the DSP.
"""
from __future__ import annotations

from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass, field
from threading import Event, Lock
from time import time
from typing import Any, Callable
from uuid import uuid4


@dataclass
class Job:
    id: str
    kind: str
    created_at: float = field(default_factory=time)
    status: str = "queued"
    progress: float = 0.0
    message: str = "En cola"
    result: dict[str, Any] | None = None
    error: str | None = None
    cancelled: Event = field(default_factory=Event, repr=False)
    future: Future | None = field(default=None, repr=False)

    def public(self) -> dict[str, Any]:
        return {"id": self.id, "kind": self.kind, "status": self.status,
                "progress": round(self.progress, 3), "message": self.message,
                "result": self.result, "error": self.error,
                "created_at": self.created_at}


class LocalJobManager:
    """Small bounded executor; jobs are intentionally not persisted."""
    def __init__(self, max_workers: int = 1, max_jobs: int = 16):
        self._executor = ThreadPoolExecutor(max_workers=max(1, max_workers), thread_name_prefix="church-analysis")
        self._jobs: dict[str, Job] = {}
        self._lock = Lock()
        self._max_jobs = max_jobs

    def submit(self, kind: str, work: Callable[[Callable[[float, str], None], Event], dict[str, Any]]) -> Job:
        with self._lock:
            active = sum(job.status in {"queued", "running"} for job in self._jobs.values())
            if active >= self._max_jobs:
                raise RuntimeError("Demasiados análisis pendientes")
            job = Job(uuid4().hex, kind)
            self._jobs[job.id] = job

        def update(progress: float, message: str) -> None:
            with self._lock:
                job.progress = max(0.0, min(1.0, float(progress)))
                job.message = str(message)[:300]

        def run() -> None:
            with self._lock:
                if job.cancelled.is_set():
                    job.status, job.message = "cancelled", "Cancelado"
                    return
                job.status, job.message = "running", "Analizando"
            try:
                result = work(update, job.cancelled)
                with self._lock:
                    if job.cancelled.is_set():
                        job.status, job.message = "cancelled", "Cancelado"
                    else:
                        job.status, job.progress, job.message, job.result = "complete", 1.0, "Listo", result
            except Exception as exc:  # Always turn model failures into a local job error.
                with self._lock:
                    job.status, job.error, job.message = "failed", str(exc)[:1000], "El análisis falló"

        job.future = self._executor.submit(run)
        return job

    def get(self, job_id: str) -> Job | None:
        with self._lock:
            return self._jobs.get(job_id)

    def cancel(self, job_id: str) -> Job | None:
        with self._lock:
            job = self._jobs.get(job_id)
            if job is None:
                return None
            job.cancelled.set()
            if job.status == "queued":
                job.future.cancel() if job.future else None
                job.status, job.message = "cancelled", "Cancelado"
            return job

    def shutdown(self) -> None:
        self._executor.shutdown(wait=False, cancel_futures=True)
