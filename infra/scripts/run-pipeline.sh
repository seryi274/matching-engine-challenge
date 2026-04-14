#!/usr/bin/env bash
# ---------------------------------------------------------------
#  run-pipeline.sh
#
#  Manual fallback to run the CI pipeline for a single team.
#  Extracts the latest commit hash from the team's branch and
#  invokes pipeline.py directly.
#
#  Usage:
#    ./run-pipeline.sh <team_name>
#
#  Environment:
#    CHALLENGE_BASE_DIR   (default: /opt/matching-engine)
#    CHALLENGE_REPO_URL   (default: from config.py)
# ---------------------------------------------------------------
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 <team_name>" >&2
    exit 1
fi

TEAM_NAME="$1"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INFRA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$INFRA_DIR/.." && pwd)"

CHALLENGE_BASE="${CHALLENGE_BASE_DIR:-/opt/matching-engine}"
export CHALLENGE_BASE_DIR="$CHALLENGE_BASE"
export PYTHONPATH="${PYTHONPATH:-}:$INFRA_DIR"

# Resolve the Python interpreter
if [ -x "$CHALLENGE_BASE/venv/bin/python" ]; then
    PYTHON="$CHALLENGE_BASE/venv/bin/python"
else
    PYTHON="python3"
fi

# Verify team is registered
VALID=$("$PYTHON" -c "
import sys; sys.path.insert(0, '$INFRA_DIR')
from server import config
print('yes' if '$TEAM_NAME' in config.TEAM_NAMES else 'no')
")

if [ "$VALID" != "yes" ]; then
    echo "Error: '$TEAM_NAME' is not a registered team." >&2
    echo "Registered teams:" >&2
    "$PYTHON" -c "
import sys; sys.path.insert(0, '$INFRA_DIR')
from server import config
for t in config.TEAM_NAMES:
    print(f'  - {t}')
" >&2
    exit 1
fi

# Get the repo URL from config
REPO_URL=$("$PYTHON" -c "
import sys, os; sys.path.insert(0, '$INFRA_DIR')
os.environ.setdefault('CHALLENGE_BASE_DIR', '$CHALLENGE_BASE')
from server import config
print(config.REPO_URL)
")

BRANCH="team/$TEAM_NAME"

echo "==> Fetching latest commit for branch: $BRANCH"

# Try to get commit hash from remote
COMMIT_HASH=$(git ls-remote "$REPO_URL" "refs/heads/$BRANCH" 2>/dev/null | awk '{print $1}')

if [ -z "$COMMIT_HASH" ]; then
    # Fallback: try local branch
    echo "  Remote lookup failed, trying local branch..."
    cd "$REPO_ROOT"
    COMMIT_HASH=$(git rev-parse "origin/$BRANCH" 2>/dev/null || git rev-parse "$BRANCH" 2>/dev/null || true)
fi

if [ -z "$COMMIT_HASH" ]; then
    echo "Error: Could not find branch '$BRANCH' on remote or locally." >&2
    exit 1
fi

echo "  Commit: $COMMIT_HASH"
echo ""
echo "==> Running pipeline for team=$TEAM_NAME commit=$COMMIT_HASH"
echo ""

"$PYTHON" -c "
import sys, os, json
sys.path.insert(0, '$INFRA_DIR')
os.environ['CHALLENGE_BASE_DIR'] = '$CHALLENGE_BASE'

from server.pipeline import run_pipeline
result = run_pipeline('$TEAM_NAME', '$COMMIT_HASH')

print()
print('=' * 60)
print('  Pipeline Result')
print('=' * 60)
print(json.dumps(result, indent=2, default=str))
print()

status = result.get('status', 'unknown')
if status == 'complete':
    print('SUCCESS')
    sys.exit(0)
else:
    print(f'FAILED (status={status})')
    sys.exit(1)
"
