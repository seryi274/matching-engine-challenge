"""
Central configuration for the matching-engine-challenge CI server.
"""

import os
from pathlib import Path

# ---------------------------------------------------------------
#  Teams
# ---------------------------------------------------------------

TEAM_NAMES: list[str] = [
    "alpha",
    "beta",
    "gamma",
    "delta",
    "epsilon",
    "zeta",
    "eta",
    "theta",
    "iota",
    "kappa",
    "lambda",
    "mu",
    "nu",
    "xi",
    "omicron",
]

# ---------------------------------------------------------------
#  Compiler / build
# ---------------------------------------------------------------

CXX_STANDARD = "c++20"
CXX_FLAGS = "-std=c++20 -O2 -Wall -Werror"
CMAKE_BUILD_TYPE = "Release"

# ---------------------------------------------------------------
#  Timeouts (seconds)
# ---------------------------------------------------------------

BUILD_TIMEOUT_S = 60
TEST_TIMEOUT_S = 120
BENCH_TIMEOUT_S = 300

# ---------------------------------------------------------------
#  Rate limiting
# ---------------------------------------------------------------

RATE_LIMIT_SECONDS = 120  # minimum gap between submissions per team

# ---------------------------------------------------------------
#  Benchmark
# ---------------------------------------------------------------

BENCH_ITERATIONS = 3

# ---------------------------------------------------------------
#  Paths
# ---------------------------------------------------------------

BASE_DIR = Path(os.environ.get("CHALLENGE_BASE_DIR", "/opt/matching-engine"))
DATABASE_PATH = BASE_DIR / "data" / "challenge.db"
WORK_DIR = BASE_DIR / "work"
HARNESS_DIR = BASE_DIR / "harness"
DOCKER_DIR = Path(__file__).resolve().parent.parent / "docker"

# ---------------------------------------------------------------
#  Repository
# ---------------------------------------------------------------

REPO_URL = os.environ.get(
    "CHALLENGE_REPO_URL",
    "https://github.com/your-org/matching-engine-challenge.git",
)

# ---------------------------------------------------------------
#  Webhook
# ---------------------------------------------------------------

WEBHOOK_SECRET = os.environ.get("WEBHOOK_SECRET", "")

# ---------------------------------------------------------------
#  Docker images
# ---------------------------------------------------------------

BENCH_IMAGE = "matching-engine-bench:latest"
SECCOMP_PROFILE = DOCKER_DIR / "seccomp-profile.json"

# ---------------------------------------------------------------
#  Docker resource limits
# ---------------------------------------------------------------

DOCKER_MEMORY_LIMIT = "1g"
DOCKER_PIDS_LIMIT = "64"
