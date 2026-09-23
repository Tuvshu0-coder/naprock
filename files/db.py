"""
BandFlow — database helper functions

This is the ONLY file that should contain raw SQL. Everything else
(Flask routes, BLE callbacks) should call these functions instead of
writing SQL directly — keeps things easy to change later.

Add more functions here as you need them.
"""

import sqlite3
import os
from contextlib import contextmanager
from pathlib import Path

DB_PATH = os.environ.get(
    "BAND_FLOW_DB_PATH",
    str(Path(__file__).resolve().parents[1] / "bandflow.db"),
)


@contextmanager
def get_connection():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA foreign_keys = ON")
    conn.row_factory = sqlite3.Row  # lets us access columns by name, e.g. row["description"]
    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def create_task(description):
    """Create a new task. Returns the new task's id."""
    with get_connection() as conn:
        cursor = conn.execute(
            "INSERT INTO tasks (description) VALUES (?)", (description,)
        )
        return cursor.lastrowid


def add_subtask(task_id, description, order_index, depends_on=None):
    """Add a subtask to a task. depends_on is another subtask's id, or None."""
    with get_connection() as conn:
        cursor = conn.execute(
            """
            INSERT INTO subtasks (task_id, description, order_index, depends_on)
            VALUES (?, ?, ?, ?)
            """,
            (task_id, description, order_index, depends_on),
        )
        return cursor.lastrowid


def get_subtasks_for_task(task_id):
    """Return all subtasks for a task, in order."""
    with get_connection() as conn:
        rows = conn.execute(
            "SELECT * FROM subtasks WHERE task_id = ? ORDER BY order_index",
            (task_id,),
        ).fetchall()
    return [dict(row) for row in rows]


def get_next_pending_subtask(task_id):
    """
    Return the next subtask that's ready to be shown on the band:
    status is 'pending' AND (no dependency, OR its dependency is 'done').
    Returns None if nothing is ready yet.
    """
    with get_connection() as conn:
        row = conn.execute(
            """
            SELECT subtasks.* FROM subtasks
            LEFT JOIN subtasks AS dependency ON subtasks.depends_on = dependency.id
            WHERE subtasks.task_id = ?
              AND subtasks.status = 'pending'
              AND (subtasks.depends_on IS NULL OR dependency.status = 'done')
            ORDER BY subtasks.order_index
            LIMIT 1
            """,
            (task_id,),
        ).fetchone()
    return dict(row) if row else None


def mark_subtask_done(subtask_id):
    """Mark a subtask as done."""
    with get_connection() as conn:
        conn.execute(
            "UPDATE subtasks SET status = 'done' WHERE id = ?",
            (subtask_id,),
        )


def mark_subtask_active(subtask_id):
    """Mark a subtask as active (currently being shown on the band)."""
    with get_connection() as conn:
        conn.execute(
            """
            UPDATE subtasks
            SET status = 'active', started_at = CURRENT_TIMESTAMP
            WHERE id = ?
            """,
            (subtask_id,),
        )


def list_tasks():
    """Return all tasks with a status derived from their subtasks."""
    with get_connection() as conn:
        rows = conn.execute(
            """
            SELECT
                tasks.id,
                tasks.description,
                tasks.created_at,
                CASE
                    WHEN COUNT(subtasks.id) = 0 THEN 'pending'
                    WHEN SUM(CASE WHEN subtasks.status != 'done' THEN 1 ELSE 0 END) = 0
                        THEN 'complete'
                    ELSE 'in_progress'
                END AS status
            FROM tasks
            LEFT JOIN subtasks ON subtasks.task_id = tasks.id
            GROUP BY tasks.id
            ORDER BY tasks.created_at, tasks.id
            """
        ).fetchall()
    return [dict(row) for row in rows]
