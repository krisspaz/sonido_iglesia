from __future__ import annotations

import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path


class LocalLearning:
    """Stores metrics and operator feedback only; audio is never copied."""
    def __init__(self, database: str | Path):
        database = Path(database).expanduser()
        database.parent.mkdir(parents=True, exist_ok=True)
        self.database = str(database)
        with sqlite3.connect(self.database) as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
            db.execute("CREATE TABLE IF NOT EXISTS sessions (id INTEGER PRIMARY KEY, church TEXT NOT NULL, rating TEXT NOT NULL, metrics TEXT NOT NULL, accepted TEXT NOT NULL, created TEXT NOT NULL)")
            db.execute("CREATE INDEX IF NOT EXISTS sessions_church_created ON sessions(church, created DESC)")
            db.execute("INSERT OR IGNORE INTO metadata(key,value) VALUES('schema_version','1')")

    def add(self, church: str, rating: str, metrics: dict, accepted: list[dict] | None = None) -> None:
        if rating not in {"excellent", "acceptable", "disliked"}:
            raise ValueError("rating inválido")
        if not church or len(church) > 128:
            raise ValueError("iglesia inválida")
        with sqlite3.connect(self.database) as db:
            db.execute("INSERT INTO sessions(church,rating,metrics,accepted,created) VALUES(?,?,?,?,?)",
                       (church, rating, json.dumps(metrics, ensure_ascii=False), json.dumps(accepted or [], ensure_ascii=False),
                        datetime.now(timezone.utc).isoformat()))

    def summary(self, church: str) -> dict:
        with sqlite3.connect(self.database) as db:
            rows = db.execute("SELECT rating, COUNT(*) FROM sessions WHERE church=? GROUP BY rating", (church,)).fetchall()
        return {rating: count for rating, count in rows}

    def preference(self, church: str) -> dict:
        """Returns bounded local tendencies learned only from excellent sessions."""
        with sqlite3.connect(self.database) as db:
            rows = db.execute("SELECT metrics, accepted, created FROM sessions WHERE church=? AND rating='excellent' ORDER BY created DESC LIMIT 100", (church,)).fetchall()
        if not rows:
            return {"sessions": 0, "loudness_target": None, "accepted_modules": {}}
        loudness, count, modules = 0.0, 0, {}
        for index, (metrics_json, accepted_json, _created) in enumerate(rows):
            # Recent services carry a little more influence, while still being
            # bounded and fully explainable.
            weight = max(.35, 1.0 - index * .01)
            metrics = json.loads(metrics_json or "{}")
            value = metrics.get("lufs_integrated")
            if isinstance(value, (int, float)):
                loudness += float(value) * weight
                count += weight
            for item in json.loads(accepted_json or "[]"):
                if isinstance(item, dict) and isinstance(item.get("kind"), str):
                    modules[item["kind"]] = modules.get(item["kind"], 0) + weight
        return {"sessions": len(rows), "loudness_target": round(max(-18.0, min(-10.0, loudness / count)), 2) if count else None,
                "accepted_modules": {key: round(value, 2) for key, value in modules.items()},
                "ready": len(rows) >= 3, "schema_version": 1}

    def reset(self) -> None:
        with sqlite3.connect(self.database) as db:
            db.execute("DELETE FROM sessions")
