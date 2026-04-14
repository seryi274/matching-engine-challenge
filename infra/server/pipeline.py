"""
Pipeline orchestrator for the matching-engine-challenge CI server.

Given a (team_name, commit_hash) pair this module:
  1. Clones the team branch (shallow)
  2. Builds inside a sandboxed Docker container
  3. Runs the correctness test suite
  4. Runs the benchmark (multiple iterations, JSON output)
  5. Stores every result in SQLite via models.py
  6. Returns a summary dict

Every step updates the submission status.  On failure the pipeline
stores the error, marks the submission as failed, and stops early.
"""

import json
import logging
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any

from . import config
from . import models

logger = logging.getLogger(__name__)


class PipelineError(Exception):
    """Raised when a pipeline step fails in an expected way."""


# ------------------------------------------------------------------
#  Public entry point
# ------------------------------------------------------------------

def run_pipeline(team_name: str, commit_hash: str) -> dict[str, Any]:
    """
    Execute the full build-test-bench pipeline for one submission.

    Returns a dict with keys: submission_id, status, build, tests, benchmarks.
    """
    submission_id = models.insert_submission(team_name, commit_hash)
    result: dict[str, Any] = {"submission_id": submission_id, "team_name": team_name}

    work_dir = config.WORK_DIR / f"sub_{submission_id}"
    work_dir.mkdir(parents=True, exist_ok=True)
    clone_dir = work_dir / "repo"

    try:
        # -- 1. Clone ---------------------------------------------------
        models.update_submission_status(submission_id, "building")
        _clone_repo(team_name, commit_hash, clone_dir)

        # -- 2. Build ---------------------------------------------------
        build_ok, build_log, build_dur = _docker_build(clone_dir, submission_id)
        models.insert_build_result(submission_id, build_ok, build_log, build_dur)
        result["build"] = {
            "success": build_ok,
            "log": build_log,
            "duration_s": build_dur,
        }
        if not build_ok:
            models.update_submission_status(submission_id, "failed")
            result["status"] = "failed"
            return result

        # -- 3. Correctness tests ---------------------------------------
        models.update_submission_status(submission_id, "testing")
        passed, total, test_output = _docker_test(clone_dir, submission_id)
        details = {"raw_output": test_output}
        models.insert_test_result(submission_id, passed, total, json.dumps(details))
        result["tests"] = {"passed": passed, "total": total}
        if passed != total:
            models.update_submission_status(submission_id, "failed")
            result["status"] = "failed"
            return result

        # -- 4. Benchmark -----------------------------------------------
        models.update_submission_status(submission_id, "benchmarking")
        bench_data = _docker_bench(clone_dir, submission_id)
        result["benchmarks"] = bench_data
        for scenario_name, stats in bench_data.get("scenarios", {}).items():
            models.insert_bench_result(submission_id, scenario_name, stats)

        # -- 5. Done -----------------------------------------------------
        models.update_submission_status(submission_id, "complete")
        result["status"] = "complete"

    except PipelineError as exc:
        logger.error("Pipeline failed for %s/%s: %s", team_name, commit_hash, exc)
        models.update_submission_status(submission_id, "failed")
        result["status"] = "failed"
        result["error"] = str(exc)

    except Exception as exc:
        logger.exception("Unexpected error in pipeline for %s/%s", team_name, commit_hash)
        models.update_submission_status(submission_id, "failed")
        result["status"] = "failed"
        result["error"] = str(exc)

    finally:
        # Clean up working directory
        shutil.rmtree(work_dir, ignore_errors=True)

    return result


# ------------------------------------------------------------------
#  Step 1 -- Clone
# ------------------------------------------------------------------

def _clone_repo(team_name: str, commit_hash: str, dest: Path) -> None:
    branch = f"team/{team_name}"
    cmd = [
        "git", "clone",
        "--depth", "1",
        "--branch", branch,
        "--single-branch",
        config.REPO_URL,
        str(dest),
    ]
    try:
        subprocess.run(
            cmd,
            check=True,
            capture_output=True,
            text=True,
            timeout=config.BUILD_TIMEOUT_S,
        )
    except subprocess.CalledProcessError as exc:
        raise PipelineError(f"git clone failed: {exc.stderr.strip()}") from exc
    except subprocess.TimeoutExpired:
        raise PipelineError("git clone timed out")

    # Verify commit hash matches (shallow clone checks out HEAD of branch)
    try:
        head = subprocess.run(
            ["git", "-C", str(dest), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except subprocess.CalledProcessError as exc:
        raise PipelineError(f"Cannot read HEAD: {exc.stderr.strip()}") from exc

    if not head.startswith(commit_hash[:7]):
        logger.warning(
            "Commit mismatch for %s: expected %s, got %s (proceeding with HEAD)",
            team_name, commit_hash, head,
        )


# ------------------------------------------------------------------
#  Docker helpers
# ------------------------------------------------------------------

def _base_docker_args() -> list[str]:
    """Common Docker run arguments for all sandboxed containers."""
    args = [
        "docker", "run",
        "--rm",
        "--network=none",
        f"--memory={config.DOCKER_MEMORY_LIMIT}",
        f"--pids-limit={config.DOCKER_PIDS_LIMIT}",
        "--read-only",
        "--tmpfs", "/tmp:rw,noexec,nosuid,size=128m",
        "--tmpfs", "/build:rw,exec,nosuid,size=512m,uid=999,gid=999",
    ]
    seccomp = config.SECCOMP_PROFILE
    if seccomp.exists():
        args += ["--security-opt", f"seccomp={seccomp}"]
    return args


def _mount_args(clone_dir: Path) -> list[str]:
    """Bind-mount the harness and student source trees."""
    harness_dir = config.HARNESS_DIR
    return [
        "-v", f"{harness_dir}:/harness:ro",
        "-v", f"{clone_dir}:/student:ro",
    ]


# ------------------------------------------------------------------
#  Step 2 -- Build
# ------------------------------------------------------------------

def _docker_build(clone_dir: Path, submission_id: int) -> tuple[bool, str, float]:
    """
    Build the student code inside Docker.
    Returns (success, log_output, duration_seconds).
    """
    cmd = _base_docker_args() + _mount_args(clone_dir) + [
        config.BENCH_IMAGE, "build",
    ]
    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=config.BUILD_TIMEOUT_S,
        )
        duration = time.monotonic() - t0
        output = proc.stdout + proc.stderr
        success = proc.returncode == 0 and "BUILD_OK" in proc.stdout
        return success, output, duration

    except subprocess.TimeoutExpired:
        duration = time.monotonic() - t0
        return False, "Build timed out", duration


# ------------------------------------------------------------------
#  Step 3 -- Correctness tests
# ------------------------------------------------------------------

_PASS_LINE_RE = re.compile(r"PASS")
_SUMMARY_RE = re.compile(r"(\d+)/(\d+) tests passed")


def _docker_test(clone_dir: Path, submission_id: int) -> tuple[int, int, str]:
    """
    Run test_correctness inside Docker.
    Returns (passed, total, raw_output).
    """
    cmd = _base_docker_args() + _mount_args(clone_dir) + [
        config.BENCH_IMAGE, "test",
    ]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=config.TEST_TIMEOUT_S,
        )
        output = proc.stdout + proc.stderr

        # Try to parse the summary line:  "29/29 tests passed."
        m = _SUMMARY_RE.search(output)
        if m:
            passed = int(m.group(1))
            total = int(m.group(2))
        else:
            # Fall back: count individual PASS lines
            pass_count = len(_PASS_LINE_RE.findall(output))
            # Count total test lines (lines matching "  [ N] test_name  PASS" or FAIL)
            total_lines = len(re.findall(r"\[\s*\d+\]", output))
            passed = pass_count
            total = total_lines if total_lines > 0 else pass_count

        return passed, total, output

    except subprocess.TimeoutExpired:
        return 0, 0, "Test execution timed out"


# ------------------------------------------------------------------
#  Step 4 -- Benchmark
# ------------------------------------------------------------------

def _docker_bench(clone_dir: Path, submission_id: int) -> dict[str, Any]:
    """
    Run the benchmark N times, collect JSON output from each iteration,
    and return the median-of-medians result (the benchmark binary already
    runs 3 internal iterations and picks the median; we run the whole
    binary config.BENCH_ITERATIONS times and take the best p50).
    """
    all_runs: list[dict[str, Any]] = []

    for iteration in range(config.BENCH_ITERATIONS):
        cmd = _base_docker_args() + _mount_args(clone_dir) + [
            config.BENCH_IMAGE, "benchmark",
        ]
        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=config.BENCH_TIMEOUT_S,
            )
            if proc.returncode != 0:
                raise PipelineError(
                    f"Benchmark iteration {iteration + 1} failed (rc={proc.returncode}): "
                    f"{proc.stderr.strip()}"
                )

            data = _parse_bench_json(proc.stdout)
            all_runs.append(data)

        except subprocess.TimeoutExpired:
            raise PipelineError(
                f"Benchmark iteration {iteration + 1} timed out "
                f"(limit={config.BENCH_TIMEOUT_S}s)"
            )

    # Pick the run with the best (lowest) composite weighted_p50_ns
    best_run = min(
        all_runs,
        key=lambda r: r.get("composite", {}).get("weighted_p50_ns", float("inf")),
    )
    return best_run


def _parse_bench_json(raw: str) -> dict[str, Any]:
    """
    Extract the JSON object from benchmark stdout.
    The binary prints JSON when invoked with --json.
    """
    # Find the outermost { ... } block in the output
    depth = 0
    start = -1
    for i, ch in enumerate(raw):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start >= 0:
                try:
                    return json.loads(raw[start : i + 1])
                except json.JSONDecodeError as exc:
                    raise PipelineError(f"Malformed benchmark JSON: {exc}") from exc

    raise PipelineError("No JSON object found in benchmark output")
