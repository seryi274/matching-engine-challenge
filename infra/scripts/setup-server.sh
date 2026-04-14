#!/usr/bin/env bash
# ---------------------------------------------------------------
#  setup-server.sh
#
#  Provisions a fresh Ubuntu 22.04+ VM for the matching-engine
#  challenge CI server.  Run once as root (or via sudo).
# ---------------------------------------------------------------
set -euo pipefail

CHALLENGE_BASE="/opt/matching-engine"
REPO_DIR="$CHALLENGE_BASE/repo"
HARNESS_DIR="$CHALLENGE_BASE/harness"
DATA_DIR="$CHALLENGE_BASE/data"
WORK_DIR="$CHALLENGE_BASE/work"
INFRA_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SERVER_DIR="$INFRA_DIR/server"
DOCKER_DIR="$INFRA_DIR/docker"
SCRIPTS_DIR="$INFRA_DIR/scripts"
VENV_DIR="$CHALLENGE_BASE/venv"
SERVICE_USER="challenge"

echo "==> Installing system packages..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq \
    docker.io \
    python3 \
    python3-pip \
    python3-venv \
    git \
    cpufrequtils \
    msr-tools \
    jq

# ---------------------------------------------------------------
#  Create service user
# ---------------------------------------------------------------
if ! id "$SERVICE_USER" &>/dev/null; then
    echo "==> Creating service user: $SERVICE_USER"
    useradd -r -m -s /bin/bash "$SERVICE_USER"
    usermod -aG docker "$SERVICE_USER"
fi

# ---------------------------------------------------------------
#  Directory structure
# ---------------------------------------------------------------
echo "==> Creating directories..."
mkdir -p "$HARNESS_DIR" "$DATA_DIR" "$WORK_DIR" "$REPO_DIR"
chown -R "$SERVICE_USER":"$SERVICE_USER" "$CHALLENGE_BASE"

# ---------------------------------------------------------------
#  Python virtual environment + dependencies
# ---------------------------------------------------------------
echo "==> Setting up Python venv..."
python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install --upgrade pip
"$VENV_DIR/bin/pip" install -r "$SERVER_DIR/requirements.txt"

# ---------------------------------------------------------------
#  CPU governor -> performance
# ---------------------------------------------------------------
echo "==> Setting CPU governor to performance..."
if command -v cpufreq-set &>/dev/null; then
    for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
        cpufreq-set -g performance -c "$(basename "$cpu" | tr -dc '0-9')" 2>/dev/null || true
    done
elif [ -d /sys/devices/system/cpu/cpu0/cpufreq ]; then
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$gov" 2>/dev/null || true
    done
fi

# ---------------------------------------------------------------
#  Disable turbo boost (Intel + AMD)
# ---------------------------------------------------------------
echo "==> Disabling turbo boost..."
# Intel
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || true
fi
# AMD
if [ -f /sys/devices/system/cpu/cpufreq/boost ]; then
    echo 0 > /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || true
fi

# ---------------------------------------------------------------
#  Copy harness files
# ---------------------------------------------------------------
echo "==> Copying harness files..."
REPO_ROOT="$(cd "$INFRA_DIR/.." && pwd)"

cp "$REPO_ROOT/CMakeLists.txt" "$HARNESS_DIR/"
cp -r "$REPO_ROOT/include" "$HARNESS_DIR/"
cp -r "$REPO_ROOT/bench" "$HARNESS_DIR/"
cp -r "$REPO_ROOT/test" "$HARNESS_DIR/"
chown -R "$SERVICE_USER":"$SERVICE_USER" "$HARNESS_DIR"

# ---------------------------------------------------------------
#  Initialise SQLite database
# ---------------------------------------------------------------
echo "==> Initialising database..."
sudo -u "$SERVICE_USER" \
    CHALLENGE_BASE_DIR="$CHALLENGE_BASE" \
    "$VENV_DIR/bin/python" -c "
import sys, os
sys.path.insert(0, '$INFRA_DIR')
os.environ['CHALLENGE_BASE_DIR'] = '$CHALLENGE_BASE'
from server import models
models.init_db()
print('Database ready at', '$DATA_DIR/challenge.db')
"

# ---------------------------------------------------------------
#  Build Docker images
# ---------------------------------------------------------------
echo "==> Building Docker images..."
docker build -t matching-engine-dev -f "$DOCKER_DIR/Dockerfile.dev" "$DOCKER_DIR"
docker build -t matching-engine-bench -f "$DOCKER_DIR/Dockerfile.bench" "$DOCKER_DIR"

# ---------------------------------------------------------------
#  Create team branches (if they don't already exist on remote)
# ---------------------------------------------------------------
echo "==> Creating team branches..."
CREATE_SCRIPT="$SCRIPTS_DIR/create-team-branches.sh"
if [ -x "$CREATE_SCRIPT" ] || [ -f "$CREATE_SCRIPT" ]; then
    chmod +x "$CREATE_SCRIPT"
    # Run from the repo root. Git identity and credentials should
    # already be configured by user-data.sh or by the operator.
    cd "$REPO_ROOT"
    bash "$CREATE_SCRIPT" || echo "  WARNING: team branch creation had errors (may already exist)"
    cd "$INFRA_DIR"
else
    echo "  WARNING: $CREATE_SCRIPT not found, skipping branch creation"
fi

# ---------------------------------------------------------------
#  Systemd service
# ---------------------------------------------------------------
echo "==> Installing systemd service..."
cat > /etc/systemd/system/matching-engine.service <<UNIT
[Unit]
Description=Matching Engine Challenge CI Server
After=network.target docker.service
Requires=docker.service

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_USER
WorkingDirectory=$INFRA_DIR
Environment=CHALLENGE_BASE_DIR=$CHALLENGE_BASE
Environment=PYTHONPATH=$INFRA_DIR
Environment=CHALLENGE_REPO_URL=${CHALLENGE_REPO_URL:-}
Environment=WEBHOOK_SECRET=${WEBHOOK_SECRET:-}
ExecStart=$VENV_DIR/bin/uvicorn server.app:app --host 0.0.0.0 --port 8000 --workers 1
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

# Hardening
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=$CHALLENGE_BASE /var/run/docker.sock
ProtectHome=yes
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable matching-engine.service
systemctl start matching-engine.service
echo "  Service started: matching-engine.service"

# ---------------------------------------------------------------
#  Cron job -- poll team branches every 5 minutes
# ---------------------------------------------------------------
echo "==> Installing cron job..."
CRON_SCRIPT="$SCRIPTS_DIR/poll-branches.sh"

cat > "$CRON_SCRIPT" <<'POLL'
#!/usr/bin/env bash
# ---------------------------------------------------------------
#  poll-branches.sh
#
#  Checks each team branch for new commits that have not yet been
#  processed, and triggers the pipeline for any that are found.
# ---------------------------------------------------------------
set -euo pipefail

CHALLENGE_BASE="${CHALLENGE_BASE_DIR:-/opt/matching-engine}"
INFRA_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VENV="$CHALLENGE_BASE/venv"
REPO_URL="${CHALLENGE_REPO_URL:-}"
DB="$CHALLENGE_BASE/data/challenge.db"

if [ -z "$REPO_URL" ]; then
    echo "CHALLENGE_REPO_URL not set, skipping" >&2
    exit 0
fi

TEAM_NAMES=$("$VENV/bin/python" -c "
import sys; sys.path.insert(0, '$INFRA_DIR')
from server import config
print(' '.join(config.TEAM_NAMES))
")

for team in $TEAM_NAMES; do
    branch="team/$team"
    # Fetch the latest commit hash from the remote
    remote_hash=$(git ls-remote "$REPO_URL" "refs/heads/$branch" 2>/dev/null | awk '{print $1}')
    if [ -z "$remote_hash" ]; then
        continue
    fi

    # Check if we already processed this commit
    already=$(sqlite3 "$DB" \
        "SELECT COUNT(*) FROM submissions WHERE team_name='$team' AND commit_hash='$remote_hash';" \
        2>/dev/null || echo "0")

    if [ "$already" = "0" ]; then
        echo "New commit for $team: $remote_hash"
        CHALLENGE_BASE_DIR="$CHALLENGE_BASE" \
        PYTHONPATH="$INFRA_DIR" \
            "$VENV/bin/python" -c "
import sys, os; sys.path.insert(0, '$INFRA_DIR')
os.environ['CHALLENGE_BASE_DIR'] = '$CHALLENGE_BASE'
from server.pipeline import run_pipeline
result = run_pipeline('$team', '$remote_hash')
print(f\"Pipeline result for $team: {result.get('status', 'unknown')}\")
"
    fi
done
POLL

chmod +x "$CRON_SCRIPT"

# Install crontab entry for the service user
CRON_LINE="*/5 * * * * CHALLENGE_BASE_DIR=$CHALLENGE_BASE CHALLENGE_REPO_URL=\$(grep CHALLENGE_REPO_URL /etc/environment 2>/dev/null | cut -d= -f2-) PYTHONPATH=$INFRA_DIR $CRON_SCRIPT >> $CHALLENGE_BASE/data/poll.log 2>&1"

(crontab -u "$SERVICE_USER" -l 2>/dev/null | grep -v "poll-branches" ; echo "$CRON_LINE") | crontab -u "$SERVICE_USER" -
echo "  Cron installed for $SERVICE_USER"

# ---------------------------------------------------------------
#  Done
# ---------------------------------------------------------------
echo ""
echo "============================================="
echo "  Setup complete!"
echo "  Service:     systemctl status matching-engine"
echo "  Logs:        journalctl -u matching-engine -f"
echo "  Endpoint:    http://<this-host>:8000"
echo "  DB:          $DATA_DIR/challenge.db"
echo "============================================="
