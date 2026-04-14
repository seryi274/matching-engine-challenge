#!/usr/bin/env bash
# ---------------------------------------------------------------
#  create-team-branches.sh
#
#  Creates a team/{name} branch for each team, seeded with the
#  skeleton student files, and pushes them to origin.
#
#  Usage:
#    ./create-team-branches.sh                      # uses config.py
#    ./create-team-branches.sh alpha beta gamma      # explicit list
# ---------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INFRA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$INFRA_DIR/.." && pwd)"

# Resolve team names
if [ $# -gt 0 ]; then
    TEAMS=("$@")
else
    # Read from config.py
    TEAMS=()
    while IFS= read -r name; do
        TEAMS+=("$name")
    done < <(python3 -c "
import sys; sys.path.insert(0, '$INFRA_DIR')
from server import config
for t in config.TEAM_NAMES:
    print(t)
")
fi

if [ ${#TEAMS[@]} -eq 0 ]; then
    echo "No teams specified. Pass names as arguments or configure TEAM_NAMES in config.py." >&2
    exit 1
fi

echo "Teams: ${TEAMS[*]}"
echo ""

cd "$REPO_ROOT"

# Ensure we are on main and up to date
MAIN_BRANCH=$(git symbolic-ref refs/remotes/origin/HEAD 2>/dev/null | sed 's@^refs/remotes/origin/@@' || echo "main")
git checkout "$MAIN_BRANCH"
git pull --ff-only origin "$MAIN_BRANCH" 2>/dev/null || true

for team in "${TEAMS[@]}"; do
    branch="team/$team"
    echo "--- Creating branch: $branch ---"

    # Delete local branch if it already exists (idempotent re-creation)
    git branch -D "$branch" 2>/dev/null || true

    # Create from main
    git checkout -b "$branch"

    # Ensure skeleton src/ exists with a starter file
    mkdir -p src
    if [ ! -f src/matching_engine.cpp ]; then
        cat > src/matching_engine.cpp <<'CPP'
#include "exchange/matching_engine.h"

namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener) {
    // Your implementation here
}

MatchingEngine::~MatchingEngine() = default;

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    // Reject invalid orders
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) {
        return OrderAck{0, OrderStatus::Rejected};
    }
    // TODO: Implement matching logic
    uint64_t id = next_order_id_++;
    return OrderAck{id, OrderStatus::Accepted};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    // TODO: Implement cancel logic
    return false;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    // TODO: Implement amend logic
    return false;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(const std::string& symbol, Side side) const {
    // TODO: Implement book snapshot
    return {};
}

uint64_t MatchingEngine::getOrderCount() const {
    // TODO: Implement order count
    return 0;
}

}  // namespace exchange
CPP
    fi

    # Stage and commit (only if there are changes)
    git add -A
    if ! git diff --cached --quiet; then
        git commit -m "Initialise team branch: $team

Skeleton matching_engine.cpp with public API stubs.
Students: implement all methods in src/matching_engine.cpp."
    else
        echo "  (no changes to commit -- skeleton already present)"
    fi

    # Push to origin
    git push -f origin "$branch"
    echo "  Pushed $branch to origin."
    echo ""

    # Return to main
    git checkout "$MAIN_BRANCH"
done

echo "============================================="
echo "  All team branches created and pushed."
echo "============================================="
