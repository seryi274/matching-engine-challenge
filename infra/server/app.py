"""
FastAPI application for the matching-engine-challenge CI server.

Endpoints:
  POST /webhook              -- GitHub push webhook receiver
  GET  /api/leaderboard      -- current leaderboard (JSON)
  GET  /api/status/{team}    -- latest submission for a team (JSON)
  GET  /api/stream           -- SSE stream of leaderboard updates
  GET  /                     -- static leaderboard SPA
"""

import asyncio
import hashlib
import hmac
import json
import logging
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from fastapi import BackgroundTasks, FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

from . import config
from . import models
from . import pipeline

logger = logging.getLogger(__name__)
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)

# ------------------------------------------------------------------
#  Application
# ------------------------------------------------------------------

app = FastAPI(title="Matching Engine Challenge", version="1.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ------------------------------------------------------------------
#  SSE support
# ------------------------------------------------------------------

_sse_event = asyncio.Event()


def _notify_leaderboard_update() -> None:
    """Signal all SSE listeners that the leaderboard has changed."""
    try:
        loop = asyncio.get_event_loop()
        if loop.is_running():
            loop.call_soon_threadsafe(_sse_event.set)
    except RuntimeError:
        pass


# ------------------------------------------------------------------
#  Rate-limit tracking (in-memory, keyed by team_name)
# ------------------------------------------------------------------

_last_run: dict[str, float] = {}
_last_run_lock = threading.Lock()


def _check_rate_limit(team_name: str) -> bool:
    """Return True if the team is allowed to submit right now."""
    now = time.time()
    with _last_run_lock:
        last = _last_run.get(team_name, 0.0)
        if now - last < config.RATE_LIMIT_SECONDS:
            return False
        _last_run[team_name] = now
        return True


# ------------------------------------------------------------------
#  Background pipeline runner
# ------------------------------------------------------------------

_pipeline_lock = threading.Lock()


def _run_pipeline_bg(team_name: str, commit_hash: str) -> None:
    """Run the full pipeline in a background thread, then notify SSE."""
    with _pipeline_lock:
        try:
            result = pipeline.run_pipeline(team_name, commit_hash)
            logger.info(
                "Pipeline complete: team=%s commit=%s status=%s",
                team_name, commit_hash[:8], result.get("status"),
            )
        except Exception:
            logger.exception("Pipeline crashed for %s/%s", team_name, commit_hash)
        finally:
            _notify_leaderboard_update()


# ------------------------------------------------------------------
#  Webhook signature verification
# ------------------------------------------------------------------

def _verify_signature(payload: bytes, signature_header: str | None) -> bool:
    """Validate X-Hub-Signature-256 from GitHub."""
    secret = config.WEBHOOK_SECRET
    if not secret:
        # No secret configured -- accept everything (dev mode)
        return True
    if not signature_header:
        return False

    if signature_header.startswith("sha256="):
        signature_header = signature_header[7:]

    expected = hmac.new(
        secret.encode(), payload, hashlib.sha256
    ).hexdigest()
    return hmac.compare_digest(expected, signature_header)


# ------------------------------------------------------------------
#  Startup
# ------------------------------------------------------------------

@app.on_event("startup")
async def startup() -> None:
    models.init_db()
    logger.info("Database initialised at %s", config.DATABASE_PATH)


# ------------------------------------------------------------------
#  POST /webhook
# ------------------------------------------------------------------

@app.post("/webhook")
async def webhook(request: Request, background_tasks: BackgroundTasks) -> JSONResponse:
    body = await request.body()

    # Verify signature
    sig = request.headers.get("X-Hub-Signature-256")
    if not _verify_signature(body, sig):
        raise HTTPException(status_code=403, detail="Invalid signature")

    # Parse payload
    try:
        payload = json.loads(body)
    except json.JSONDecodeError:
        raise HTTPException(status_code=400, detail="Invalid JSON")

    # Only handle push events
    event = request.headers.get("X-GitHub-Event", "")
    if event == "ping":
        return JSONResponse({"status": "pong"})
    if event != "push":
        return JSONResponse({"status": "ignored", "event": event})

    # Extract branch ref
    ref: str = payload.get("ref", "")
    if not ref.startswith("refs/heads/team/"):
        return JSONResponse({"status": "ignored", "reason": "not a team branch"})

    team_name = ref.removeprefix("refs/heads/team/")
    commit_hash: str = payload.get("after", payload.get("head_commit", {}).get("id", ""))

    if not commit_hash or commit_hash == "0" * 40:
        return JSONResponse({"status": "ignored", "reason": "branch deleted"})

    # Validate team
    if team_name not in config.TEAM_NAMES:
        raise HTTPException(status_code=403, detail=f"Unknown team: {team_name}")

    # Rate limit
    if not _check_rate_limit(team_name):
        raise HTTPException(
            status_code=429,
            detail=f"Rate limited. Wait {config.RATE_LIMIT_SECONDS}s between pushes.",
        )

    # Enqueue
    background_tasks.add_task(_run_pipeline_bg, team_name, commit_hash)
    logger.info("Enqueued pipeline: team=%s commit=%s", team_name, commit_hash[:8])
    return JSONResponse({"status": "queued", "team": team_name, "commit": commit_hash})


# ------------------------------------------------------------------
#  GET /api/leaderboard
# ------------------------------------------------------------------

@app.get("/api/leaderboard")
async def api_leaderboard() -> JSONResponse:
    raw = models.get_leaderboard()
    teams = _transform_leaderboard(raw)
    return JSONResponse({"teams": teams})


def _transform_leaderboard(raw: list[dict]) -> list[dict]:
    """Transform DB leaderboard rows into the format the frontend expects."""
    teams = []
    for entry in raw:
        # Compute avg_latency and p99_latency from scenario breakdown
        scenarios = entry.get("scenarios", {})
        avg_latency = 0.0
        p99_latency = 0.0
        for s_name, s_data in scenarios.items():
            weight = {"uniform": 0.30, "realistic": 0.40, "adversarial": 0.30}.get(s_name, 0)
            avg_latency += weight * s_data.get("mean_ns", 0)
            p99_latency += weight * s_data.get("p99_ns", 0)

        # Count total submissions for this team
        sub_count = models.get_team_submission_count(entry["team_name"])

        teams.append({
            "team_name": entry["team_name"],
            "throughput": round(entry.get("weighted_throughput_ops", 0)),
            "avg_latency": round(avg_latency),
            "p99_latency": round(p99_latency),
            "weighted_p50_ns": round(entry.get("weighted_p50_ns", 0)),
            "submissions": sub_count,
            "last_updated": entry.get("submitted_at", ""),
            "commit_hash": entry.get("commit_hash", ""),
        })
    return teams


# ------------------------------------------------------------------
#  GET /api/status/{team_name}
# ------------------------------------------------------------------

@app.get("/api/status/{team_name}")
async def api_status(team_name: str) -> JSONResponse:
    status = models.get_team_status(team_name)
    if status is None:
        raise HTTPException(status_code=404, detail="No submissions for this team")
    return JSONResponse(status)


# ------------------------------------------------------------------
#  GET /api/stream  (Server-Sent Events)
# ------------------------------------------------------------------

@app.get("/api/stream")
async def api_stream(request: Request) -> StreamingResponse:
    async def event_generator():
        # Send initial leaderboard immediately
        raw = models.get_leaderboard()
        teams = _transform_leaderboard(raw)
        yield f"data: {json.dumps({'teams': teams})}\n\n"

        while True:
            # Wait for a notification or timeout (heartbeat every 15s)
            try:
                await asyncio.wait_for(
                    _wait_for_sse_event(), timeout=15.0
                )
            except asyncio.TimeoutError:
                yield ": heartbeat\n\n"
                continue

            # Check if the client disconnected
            if await request.is_disconnected():
                break

            raw = models.get_leaderboard()
            teams = _transform_leaderboard(raw)
            yield f"data: {json.dumps({'teams': teams})}\n\n"

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
            "X-Accel-Buffering": "no",
        },
    )


async def _wait_for_sse_event() -> None:
    """Block until the SSE event is set, then clear it."""
    await _sse_event.wait()
    _sse_event.clear()


# ------------------------------------------------------------------
#  Static files (leaderboard SPA)
# ------------------------------------------------------------------

_static_dir = Path(__file__).resolve().parent.parent.parent / "leaderboard" / "static"
if _static_dir.is_dir():
    app.mount("/", StaticFiles(directory=str(_static_dir), html=True), name="static")
