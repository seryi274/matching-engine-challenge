#!/usr/bin/env bash
# Matching Engine Challenge -- Teardown script
#
# Usage:
#   ./teardown.sh                   Destroy all Terraform-managed resources
#   ./teardown.sh --clean-branches  Also delete team/* branches from the remote
#
# The script must be run from the infra/terraform/ directory (or it will cd there).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CLEAN_BRANCHES=false

for arg in "$@"; do
    case "$arg" in
        --clean-branches) CLEAN_BRANCHES=true ;;
        -h|--help)
            echo "Usage: $0 [--clean-branches]"
            echo ""
            echo "  --clean-branches   Delete all team/* branches from the remote repository"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            echo "Usage: $0 [--clean-branches]"
            exit 1
            ;;
    esac
done

# -------------------------------------------------------------------
# 1. Terraform destroy
# -------------------------------------------------------------------
echo "==> Destroying Terraform-managed infrastructure..."

if [ ! -f "main.tf" ]; then
    echo "ERROR: main.tf not found. Run this script from infra/terraform/."
    exit 1
fi

terraform destroy -auto-approve

echo ""
echo "==> Terraform resources destroyed successfully."

# -------------------------------------------------------------------
# 2. Remove GitHub webhook (best-effort, requires gh CLI)
# -------------------------------------------------------------------
if command -v gh &>/dev/null; then
    echo ""
    echo "==> Checking for GitHub webhooks to remove..."

    # Determine the repo from the Terraform variable or git remote
    REPO=""
    if [ -f terraform.tfvars ]; then
        REPO=$(grep -E '^\s*repo_url\s*=' terraform.tfvars 2>/dev/null \
            | head -1 \
            | sed 's/.*=\s*"\(.*\)"/\1/' \
            | sed 's|https://github.com/||;s|\.git$||' || true)
    fi

    if [ -n "$REPO" ]; then
        HOOK_IDS=$(gh api "repos/$REPO/hooks" --jq '.[].id' 2>/dev/null || true)
        if [ -n "$HOOK_IDS" ]; then
            for hid in $HOOK_IDS; do
                echo "    Deleting webhook $hid from $REPO..."
                gh api -X DELETE "repos/$REPO/hooks/$hid" 2>/dev/null || true
            done
            echo "    Webhooks removed."
        else
            echo "    No webhooks found (or insufficient permissions)."
        fi
    else
        echo "    Could not determine repository. Skipping webhook cleanup."
    fi
else
    echo ""
    echo "NOTE: Install the GitHub CLI (gh) to automatically remove webhooks on teardown."
fi

# -------------------------------------------------------------------
# 3. Clean team branches (optional)
# -------------------------------------------------------------------
if [ "$CLEAN_BRANCHES" = true ]; then
    echo ""
    echo "==> Cleaning team/* branches from remote..."

    REPO_ROOT="$SCRIPT_DIR/../.."
    if [ -d "$REPO_ROOT/.git" ]; then
        cd "$REPO_ROOT"
        BRANCHES=$(git ls-remote --heads origin 'refs/heads/team/*' 2>/dev/null \
            | awk '{print $2}' \
            | sed 's|refs/heads/||' || true)

        if [ -n "$BRANCHES" ]; then
            for branch in $BRANCHES; do
                echo "    Deleting remote branch: $branch"
                git push origin --delete "$branch" 2>/dev/null || true
            done
            echo "    Team branches removed."
        else
            echo "    No team/* branches found on remote."
        fi
        cd "$SCRIPT_DIR"
    else
        echo "    WARNING: Could not find git root. Skipping branch cleanup."
    fi
fi

# -------------------------------------------------------------------
# Done
# -------------------------------------------------------------------
echo ""
echo "============================================"
echo "  Teardown complete."
echo "  All AWS resources have been destroyed."
echo "============================================"
