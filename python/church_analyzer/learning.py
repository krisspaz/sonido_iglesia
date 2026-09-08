from __future__ import annotations

import json
import sqlite3
from pathlib import Path


class LocalLearning:
    """Stores metrics and operator feedback only; audio is never copied."""
    def __init__(self, database: str | Path):
        self.database = str(database)
        with sqlite3.connect(self.database) as db:
            db.execute("CREATE TABLE IF NOT EXISTS sessions (id INTEGER PRIMARY KEY, church TEXT, rating TEXT, metrics TEXT, accepted TEXT, created TEXT DEFAULT CURRENT_TIMESTAMP)")

    def add(self, church: str, rating: str, metrics: dict, accepted: list[dict] | None = None) -> None:
        if rating not in {"excellent", "acceptable", "disliked"}:
            raise ValueError("rating inválido")
        with sqlite3.connect(self.database) as db:
            db.execute("INSERT INTO sessions(church,rating,metrics,accepted) VALUES(?,?,?,?)",
                       (church, rating, json.dumps(metrics), json.dumps(accepted or [])))

    def summary(self, church: str) -> dict:
        with sqlite3.connect(self.database) as db:
            rows = db.execute("SELECT rating, COUNT(*) FROM sessions WHERE church=? GROUP BY rating", (church,)).fetchall()
        return {rating: count for rating, count in rows}

    def reset(self) -> None:
        with sqlite3.connect(self.database) as db:
            db.execute("DELETE FROM sessions")
