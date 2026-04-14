#!/bin/bash
set -euo pipefail

# ---------------------------------------------------------------
#  Entrypoint for the benchmark runner container.
#
#  Usage:
#    entrypoint.sh build       -- compile only, exit 0/1
#    entrypoint.sh test        -- compile + run correctness tests
#    entrypoint.sh benchmark   -- compile + run benchmark (--json)
#
#  Mount points expected:
#    /harness   (ro)  -- CMakeLists.txt, bench/, test/, include/exchange/
#    /student   (ro)  -- src/*.cpp, include/exchange/ (student headers overlay harness)
# ---------------------------------------------------------------

MODE="${1:-build}"

# ---- Assemble build tree in /build ----
cp /harness/CMakeLists.txt /build/
cp -r --no-preserve=mode,ownership /harness/include /build/
cp -r --no-preserve=mode,ownership /harness/bench /build/
cp -r --no-preserve=mode,ownership /harness/test /build/
chmod -R u+w /build/include/
mkdir -p /build/src
cp /student/src/*.cpp /build/src/ 2>/dev/null || cp /student/src/*.cc /build/src/ 2>/dev/null || true
# Copy student headers (allows adding private members to matching_engine.h)
cp -r --no-preserve=mode,ownership /student/include/exchange/* /build/include/exchange/ 2>/dev/null || true

# ---- Build ----
cd /build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS="-std=c++20 -O2 -Wall -Werror" \
      -S . -B out >/dev/null 2>&1

cmake --build out --parallel "$(nproc)" 2>&1

if [ "$MODE" = "build" ]; then
    echo "BUILD_OK"
    exit 0
fi

# ---- Test ----
if [ "$MODE" = "test" ]; then
    exec /build/out/test_correctness
fi

# ---- Benchmark ----
if [ "$MODE" = "benchmark" ]; then
    exec /build/out/benchmark --json
fi

echo "Unknown mode: $MODE" >&2
exit 1
