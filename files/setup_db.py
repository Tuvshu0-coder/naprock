"""
BandFlow — database setup

Run this ONCE to create bandflow.db with the tasks/subtasks tables:
    python3 setup_db.py

If you ever need to start over, just delete bandflow.db and run this again.
"""

import sqlite3
import os
from pathlib import Path

DB_PATH = os.environ.get(
    "BAND_FLOW_DB_PATH",
    str(Path(__file__).resolve().parents[1] / "bandflow.db"),
)


def setup():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA foreign_keys = ON")
    cursor = conn.cursor()

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS tasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            description TEXT NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    """)

    cursor.execute("""
        CREATE TABLE IF NOT EXISTS subtasks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            task_id INTEGER NOT NULL,
            description TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending', 'active', 'done')),
            depends_on INTEGER,
            order_index INTEGER NOT NULL,
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
            started_at TIMESTAMP,
            FOREIGN KEY (task_id) REFERENCES tasks(id),
            FOREIGN KEY (depends_on) REFERENCES subtasks(id)
        )
    """)

    columns = {row[1] for row in cursor.execute("PRAGMA table_info(subtasks)")}
    if "started_at" not in columns:
        cursor.execute("ALTER TABLE subtasks ADD COLUMN started_at TIMESTAMP")

    conn.commit()
    conn.close()
    print(f"Database ready at {DB_PATH}")


if __name__ == "__main__":
    setup()
