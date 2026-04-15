"""
SQLite persistence layer for the matching-engine-challenge CI server.

Uses plain sqlite3 -- no ORM.  All writes are serialised through a
module-level threading lock so that concurrent background pipeline
threads do not corrupt the database.
"""

import json
import sqlite3
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from . import config

_db_lock = threading.Lock()


def _connect() -> sqlite3.Connection:
    path = config.DATABASE_PATH
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path), timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    return conn


# ------------------------------------------------------------------
#  Schema bootstrap
# ------------------------------------------------------------------

_SCHEMA = """
CREATE TABLE IF NOT EXISTS submissions (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    team_name    TEXT    NOT NULL,
    commit_hash  TEXT    NOT NULL,
    submitted_at TEXT    NOT NULL,
    status       TEXT    NOT NULL DEFAULT 'queued'
);

CREATE TABLE IF NOT EXISTS build_results (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    submission_id INTEGER NOT NULL REFERENCES submissions(id),
    success       INTEGER NOT NULL,
    log           TEXT    NOT NULL DEFAULT '',
    duration_s    REAL    NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS test_results (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    submission_id INTEGER NOT NULL REFERENCES submissions(id),
    passed        INTEGER NOT NULL DEFAULT 0,
    total         INTEGER NOT NULL DEFAULT 0,
    details       TEXT    NOT NULL DEFAULT '{}'
);

CREATE TABLE IF NOT EXISTS bench_results (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    submission_id   INTEGER NOT NULL REFERENCES submissions(id),
    scenario        TEXT    NOT NULL,
    mean_ns         REAL    NOT NULL DEFAULT 0,
    p50_ns          REAL    NOT NULL DEFAULT 0,
    p90_ns          REAL    NOT NULL DEFAULT 0,
    p99_ns          REAL    NOT NULL DEFAULT 0,
    min_ns          REAL    NOT NULL DEFAULT 0,
    max_ns          REAL    NOT NULL DEFAULT 0,
    stddev_ns       REAL    NOT NULL DEFAULT 0,
    throughput_ops  REAL    NOT NULL DEFAULT 0
);

CREATE INDEX IF NOT EXISTS idx_submissions_team
    ON submissions(team_name, submitted_at);
CREATE INDEX IF NOT EXISTS idx_build_results_sub
    ON build_results(submission_id);
CREATE INDEX IF NOT EXISTS idx_test_results_sub
    ON test_results(submission_id);
CREATE INDEX IF NOT EXISTS idx_bench_results_sub
    ON bench_results(submission_id);
"""


def init_db() -> None:
    """Create tables if they do not already exist."""
    with _db_lock:
        conn = _connect()
        try:
            conn.executescript(_SCHEMA)
            conn.commit()
        finally:
            conn.close()


# ------------------------------------------------------------------
#  Submissions
# ------------------------------------------------------------------

def insert_submission(team_name: str, commit_hash: str) -> int:
    """Insert a new queued submission and return its id."""
    now = datetime.now(timezone.utc).isoformat()
    with _db_lock:
        conn = _connect()
        try:
            cur = conn.execute(
                "INSERT INTO submissions (team_name, commit_hash, submitted_at, status) "
                "VALUES (?, ?, ?, 'queued')",
                (team_name, commit_hash, now),
            )
            conn.commit()
            return cur.lastrowid  # type: ignore[return-value]
        finally:
            conn.close()


def update_submission_status(submission_id: int, status: str) -> None:
    """Update the status column of an existing submission."""
    with _db_lock:
        conn = _connect()
        try:
            conn.execute(
                "UPDATE submissions SET status = ? WHERE id = ?",
                (status, submission_id),
            )
            conn.commit()
        finally:
            conn.close()


# ------------------------------------------------------------------
#  Build results
# ------------------------------------------------------------------

def insert_build_result(
    submission_id: int,
    success: bool,
    log: str,
    duration_s: float,
) -> None:
    with _db_lock:
        conn = _connect()
        try:
            conn.execute(
                "INSERT INTO build_results (submission_id, success, log, duration_s) "
                "VALUES (?, ?, ?, ?)",
                (submission_id, int(success), log, duration_s),
            )
            conn.commit()
        finally:
            conn.close()


# ------------------------------------------------------------------
#  Test results
# ------------------------------------------------------------------

def insert_test_result(
    submission_id: int,
    passed: int,
    total: int,
    details_json: str,
) -> None:
    with _db_lock:
        conn = _connect()
        try:
            conn.execute(
                "INSERT INTO test_results (submission_id, passed, total, details) "
                "VALUES (?, ?, ?, ?)",
                (submission_id, passed, total, details_json),
            )
            conn.commit()
        finally:
            conn.close()


# ------------------------------------------------------------------
#  Benchmark results
# ------------------------------------------------------------------

def insert_bench_result(
    submission_id: int,
    scenario: str,
    stats: dict[str, Any],
) -> None:
    with _db_lock:
        conn = _connect()
        try:
            conn.execute(
                "INSERT INTO bench_results "
                "(submission_id, scenario, mean_ns, p50_ns, p90_ns, p99_ns, "
                " min_ns, max_ns, stddev_ns, throughput_ops) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    submission_id,
                    scenario,
                    stats.get("mean_ns", 0),
                    stats.get("p50_ns", 0),
                    stats.get("p90_ns", 0),
                    stats.get("p99_ns", 0),
                    stats.get("min_ns", 0),
                    stats.get("max_ns", 0),
                    stats.get("stddev_ns", 0),
                    stats.get("throughput_ops", 0),
                ),
            )
            conn.commit()
        finally:
            conn.close()


# ------------------------------------------------------------------
#  Leaderboard
# ------------------------------------------------------------------

def get_leaderboard() -> list[dict[str, Any]]:
    """
    Return the leaderboard: one row per team, best qualifying submission
    (passed ALL tests), ranked by composite weighted_p50_ns ascending.

    Weights: uniform=0.30, realistic=0.40, adversarial=0.30
    """
    sql = """
    WITH qualified AS (
        -- Submissions where every test passed
        SELECT s.id AS submission_id,
               s.team_name,
               s.commit_hash,
               s.submitted_at
          FROM submissions s
          JOIN test_results t ON t.submission_id = s.id
         WHERE s.status = 'complete'
           AND t.passed = t.total
           AND t.total > 0
    ),
    scored AS (
        SELECT q.submission_id,
               q.team_name,
               q.commit_hash,
               q.submitted_at,
               SUM(CASE b.scenario
                   WHEN 'uniform'     THEN 0.30 * b.p50_ns
                   WHEN 'realistic'   THEN 0.40 * b.p50_ns
                   WHEN 'adversarial' THEN 0.30 * b.p50_ns
                   ELSE 0 END) AS weighted_p50_ns,
               SUM(CASE b.scenario
                   WHEN 'uniform'     THEN 0.30 * b.throughput_ops
                   WHEN 'realistic'   THEN 0.40 * b.throughput_ops
                   WHEN 'adversarial' THEN 0.30 * b.throughput_ops
                   ELSE 0 END) AS weighted_throughput_ops
          FROM qualified q
          JOIN bench_results b ON b.submission_id = q.submission_id
         GROUP BY q.submission_id
    ),
    best AS (
        SELECT *,
               ROW_NUMBER() OVER (
                   PARTITION BY team_name
                   ORDER BY weighted_throughput_ops DESC
               ) AS rn
          FROM scored
    )
    SELECT team_name,
           commit_hash,
           submitted_at,
           weighted_p50_ns,
           weighted_throughput_ops,
           submission_id
      FROM best
     WHERE rn = 1
     ORDER BY weighted_throughput_ops DESC
    """
    with _db_lock:
        conn = _connect()
        try:
            rows = conn.execute(sql).fetchall()
            result: list[dict[str, Any]] = []
            for rank, row in enumerate(rows, start=1):
                entry: dict[str, Any] = {
                    "rank": rank,
                    "team_name": row["team_name"],
                    "commit_hash": row["commit_hash"],
                    "submitted_at": row["submitted_at"],
                    "weighted_p50_ns": row["weighted_p50_ns"],
                    "weighted_throughput_ops": row["weighted_throughput_ops"],
                }
                # Attach per-scenario breakdown
                scenarios = conn.execute(
                    "SELECT scenario, mean_ns, p50_ns, p90_ns, p99_ns, "
                    "       min_ns, max_ns, stddev_ns, throughput_ops "
                    "  FROM bench_results WHERE submission_id = ?",
                    (row["submission_id"],),
                ).fetchall()
                entry["scenarios"] = {
                    s["scenario"]: dict(s) for s in scenarios
                }
                result.append(entry)
            return result
        finally:
            conn.close()


# ------------------------------------------------------------------
#  Team status
# ------------------------------------------------------------------

def get_team_status(team_name: str) -> dict[str, Any] | None:
    """Return the latest submission for a team with all attached results."""
    with _db_lock:
        conn = _connect()
        try:
            row = conn.execute(
                "SELECT * FROM submissions WHERE team_name = ? "
                "ORDER BY submitted_at DESC LIMIT 1",
                (team_name,),
            ).fetchone()
            if row is None:
                return None

            sub_id = row["id"]
            result: dict[str, Any] = dict(row)

            # Build result
            br = conn.execute(
                "SELECT success, log, duration_s FROM build_results "
                "WHERE submission_id = ?",
                (sub_id,),
            ).fetchone()
            result["build"] = dict(br) if br else None

            # Test result
            tr = conn.execute(
                "SELECT passed, total, details FROM test_results "
                "WHERE submission_id = ?",
                (sub_id,),
            ).fetchone()
            if tr:
                tr_dict = dict(tr)
                try:
                    tr_dict["details"] = json.loads(tr_dict["details"])
                except (json.JSONDecodeError, TypeError):
                    pass
                result["tests"] = tr_dict
            else:
                result["tests"] = None

            # Bench results
            benches = conn.execute(
                "SELECT scenario, mean_ns, p50_ns, p90_ns, p99_ns, "
                "       min_ns, max_ns, stddev_ns, throughput_ops "
                "  FROM bench_results WHERE submission_id = ?",
                (sub_id,),
            ).fetchall()
            result["benchmarks"] = {
                b["scenario"]: dict(b) for b in benches
            }

            return result
        finally:
            conn.close()


# ------------------------------------------------------------------
#  Submission count
# ------------------------------------------------------------------

def get_team_submission_count(team_name: str) -> int:
    """Return the total number of submissions for a team."""
    with _db_lock:
        conn = _connect()
        try:
            row = conn.execute(
                "SELECT COUNT(*) AS cnt FROM submissions WHERE team_name = ?",
                (team_name,),
            ).fetchone()
            return row["cnt"] if row else 0
        finally:
            conn.close()


# ------------------------------------------------------------------
#  Rate limiting
# ------------------------------------------------------------------

def get_last_submission_time(team_name: str) -> str | None:
    """Return the ISO-8601 submitted_at of the team's most recent submission."""
    with _db_lock:
        conn = _connect()
        try:
            row = conn.execute(
                "SELECT submitted_at FROM submissions WHERE team_name = ? "
                "ORDER BY submitted_at DESC LIMIT 1",
                (team_name,),
            ).fetchone()
            return row["submitted_at"] if row else None
        finally:
            conn.close()
