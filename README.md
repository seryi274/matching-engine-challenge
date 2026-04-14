# Matching Engine Challenge

Build a high-performance C++ matching engine. Compete on correctness first, then on latency.

A matching engine is the core software component of every financial exchange. It receives buy and sell orders, maintains an order book, and determines when trades occur. Your job is to implement one that is both correct and fast.

---

## Table of Contents

- [What Is a Matching Engine?](#what-is-a-matching-engine)
- [Rules](#rules)
- [Getting Started](#getting-started)
- [Interface Overview](#interface-overview)
- [Matching Rules](#matching-rules)
- [Scoring](#scoring)
- [Submission](#submission)
- [Local Development](#local-development)
- [Project Structure](#project-structure)
- [Codebase In Depth](#codebase-in-depth)
- [Infrastructure](#infrastructure)
- [Tips](#tips)

---

## What Is a Matching Engine?

When someone wants to buy 100 shares of AAPL at \$150, their order goes to an exchange. The exchange's matching 
engine checks whether any existing sell orders are willing to sell at $150 or less. If so, a trade happens. If not, the buy order "rests" on the order book, waiting for a compatible sell order to arrive.

You will implement the `MatchingEngine` class, which maintains per-symbol order books, matches incoming orders against resting orders using price-time priority, handles cancellations and amendments, and reports trades and order updates through a callback interface. The engine must handle multiple symbols, partial fills, multi-level sweeps, and edge cases like priority loss on amendments.

## Rules

- Teams of 3-4 people.
- Implement the `MatchingEngine` class in `src/matching_engine.cpp`.
- **DO NOT** modify any files in `include/`. The interface is fixed and the server will overwrite your changes.
- You may add additional `.cpp` and `.h` files in `src/`. The build system automatically picks up all source files in that directory.
- **Single-threaded only.** No threads, no async, no coroutines, no multi-process tricks.
- The server compiles with: `g++ -std=c++20 -O2 -march=native -DNDEBUG`.
- You must pass **all 29 correctness tests** to qualify for the performance ranking. If any test fails, you are not ranked.
- Ranked by weighted p50 latency across 3 benchmark scenarios.

## Getting Started

### 1. Clone and checkout your team branch

```bash
git clone <REPO_URL>
cd matching-engine-challenge
git checkout team/your-team-name
```

### 2. Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces three artifacts:
- `build/libmatching_engine.a` -- your engine as a static library
- `build/test_correctness` -- the correctness test suite
- `build/benchmark` -- the performance benchmark

### 3. Run correctness tests

```bash
./build/test_correctness
```

You should see output like:

```
Running matching engine correctness tests...

  [ 1] exact_match_buy_then_sell                          PASS
  [ 2] exact_match_sell_then_buy                          PASS
  ...
  [29] order_update_on_fill                               PASS

29/29 tests passed.
ALL TESTS PASSED!
```

### 4. Run the benchmark locally

```bash
./build/benchmark
```

This runs 1,000,000 operations across 3 scenarios (uniform, realistic, adversarial) and reports latency percentiles and throughput.

For JSON output (used by the submission pipeline):

```bash
./build/benchmark --json
```

### 5. Submit

```bash
git add -A
git commit -m "Describe what you changed"
git push origin team/your-team-name
```

## Interface Overview

Your engine must conform to the interface defined in two header files. **Do not modify these files.**

### `include/exchange/types.h`

Defines the data types used throughout the system:

| Type | Purpose |
|------|---------|
| `Side` | Enum: `Buy` or `Sell` (uint8_t). |
| `OrderStatus` | Enum: `Accepted`, `Filled`, `Cancelled`, `Rejected`. |
| `OrderRequest` | Input to `addOrder()`. Fields: `symbol` (string), `side`, `price` (int64_t ticks), `quantity` (uint32_t lots). |
| `OrderAck` | Returned by `addOrder()`. Fields: `order_id` (uint64_t), `status`. |
| `Trade` | Emitted via `onTrade()` callback. Fields: `buy_order_id`, `sell_order_id`, `symbol`, `price`, `quantity`. |
| `OrderUpdate` | Emitted via `onOrderUpdate()` callback. Fields: `order_id`, `status`, `remaining_quantity`. |
| `PriceLevel` | Used by `getBookSnapshot()`. Fields: `price`, `total_quantity`, `order_count`. |
| `Listener` | Abstract class with virtual `onTrade(const Trade&)` and `onOrderUpdate(const OrderUpdate&)`. The test harness and benchmark implement this. |

### `include/exchange/matching_engine.h`

Defines the `MatchingEngine` class you must implement:

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `MatchingEngine(Listener* listener)` | Initialize with a callback listener. |
| `addOrder` | `OrderAck addOrder(const OrderRequest&)` | Submit a new limit order. Assign ID, match, rest remainder. |
| `cancelOrder` | `bool cancelOrder(uint64_t order_id)` | Cancel a resting order. Returns false if not found. |
| `amendOrder` | `bool amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity)` | Modify a resting order. May trigger matching. |
| `getBookSnapshot` | `vector<PriceLevel> getBookSnapshot(const string& symbol, Side side) const` | Snapshot of one side of the book. Not performance-critical. |
| `getOrderCount` | `uint64_t getOrderCount() const` | Total resting orders. Not performance-critical. |

Order IDs are monotonically increasing, starting at 1. Rejected orders get `order_id = 0`.

The class provides two private members you can use: `listener_` (the callback pointer) and `next_order_id_` (initialized to 1). Add any additional data structures you need as private members.

## Matching Rules

Your engine must implement **price-time priority (FIFO)** matching:

1. **Price priority.** A buy order matches against the lowest-priced resting sell whose price is at or below the buy's price. A sell order matches against the highest-priced resting buy whose price is at or above the sell's price.

2. **Time priority.** At the same price level, the order that arrived earliest is matched first (FIFO).

3. **Execution price.** The trade price is always the **resting** (passive) order's price, not the incoming (aggressive) order's price.

4. **Greedy matching.** Fill as much as possible immediately. If the incoming order's quantity exceeds the resting order's quantity, fill the resting order completely and continue to the next resting order (or next price level).

5. **Partial fills.** If an incoming order is only partially filled, the remainder rests on the book with status `Accepted`.

6. **Rejection.** Reject orders with `price <= 0`, `quantity == 0`, or empty `symbol`. Rejected orders return `OrderAck{0, Rejected}`.

7. **Cancellation.** `cancelOrder(id)` removes the order from the book and emits an `OrderUpdate` with status `Cancelled`. Returns false if the order does not exist or is already filled/cancelled.

8. **Amendment.** `amendOrder(id, new_price, new_quantity)` modifies a resting order:
   - If the **price changes**: the order **loses** time priority.
   - If the **quantity increases** (price unchanged): the order **loses** time priority.
   - If **only the quantity decreases** (price unchanged): the order **keeps** time priority.
   - After amendment, the order may match if prices now cross.
   - Returns false if the order is not found, or if `new_price <= 0` or `new_quantity == 0`.

9. **Callback ordering.** During a single `addOrder` call that matches N resting orders, call `onTrade()` and `onOrderUpdate()` for each fill in price-time priority order, then `onOrderUpdate()` for the aggressor order.

10. **Symbol isolation.** Orders for different symbols never interact.

## Scoring

### Correctness (pass/fail)

All 29 correctness tests must pass. If any test fails, your submission is **not ranked**.

### Performance (ranking)

Weighted p50 latency across three benchmark scenarios:

| Scenario | Weight | Description |
|----------|--------|-------------|
| Uniform | 30% | Random buy/sell in narrow price band. Baseline throughput. |
| Realistic | 40% | Market microstructure: bursts, pressure, reversals. |
| Adversarial | 30% | Deep book, wide prices, sweeping orders. Worst-case. |

Each scenario: 1,000,000 operations (70% adds, 20-25% cancels, 5-10% amends), 3 iterations, median p50 used.

**Composite score = 0.30 * uniform_p50 + 0.40 * realistic_p50 + 0.30 * adversarial_p50** (lower is better)

## Submission

Push to your team branch:

```bash
git push origin team/your-team-name
```

The server automatically compiles, tests, and benchmarks your code.

**Check status:** `curl http://SERVER_URL/api/status/your-team-name`

**Leaderboard:** `http://SERVER_URL/`

**Rate limit:** 1 submission per 2 minutes.

## Local Development

### Option A: Docker dev container (recommended)

```bash
docker build -t me-dev -f infra/docker/Dockerfile.dev .
docker run -it --rm -v $(pwd):/workspace -w /workspace me-dev bash

# Inside container:
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/test_correctness
./build/benchmark
```

### Option B: Native

Requirements: g++ >= 13 or clang++ >= 16 (C++20), cmake >= 3.16.

```bash
# macOS
brew install gcc cmake

# Ubuntu/Debian
sudo apt-get install g++-13 cmake
```

## Project Structure

```
matching-engine-challenge/
|
|-- include/exchange/
|   |-- types.h                      # Data types, Listener interface (DO NOT MODIFY)
|   |-- matching_engine.h            # MatchingEngine class declaration (DO NOT MODIFY)
|
|-- src/
|   |-- matching_engine.cpp          # YOUR IMPLEMENTATION (edit this)
|
|-- reference/
|   |-- matching_engine.cpp          # Naive reference implementation (released Day 2)
|
|-- test/
|   |-- test_correctness.cpp         # 29 correctness tests
|
|-- bench/
|   |-- benchmark.cpp                # Benchmark harness (3 scenarios, JSON output)
|   |-- order_generator.h            # Deterministic order stream generator
|   |-- order_generator.cpp          # Order generator implementation
|
|-- infra/
|   |-- docker/
|   |   |-- Dockerfile.dev           # Dev container for students
|   |   |-- Dockerfile.bench         # Sandboxed benchmark runner
|   |   |-- entrypoint.sh            # Container entrypoint (build/test/benchmark modes)
|   |   |-- seccomp-profile.json     # Restrictive syscall policy for sandbox
|   |
|   |-- server/
|   |   |-- app.py                   # FastAPI server (webhook, leaderboard API, SSE)
|   |   |-- pipeline.py              # Build -> test -> benchmark orchestrator
|   |   |-- models.py                # SQLite persistence layer
|   |   |-- config.py                # Team list, timeouts, compiler flags
|   |   |-- requirements.txt         # Python dependencies
|   |
|   |-- scripts/
|   |   |-- setup-server.sh          # One-time cloud VM provisioning
|   |   |-- create-team-branches.sh  # Initialize team/* branches in repo
|   |   |-- run-pipeline.sh          # Manual pipeline trigger (fallback)
|   |
|   |-- terraform/
|       |-- main.tf                  # AWS EC2 infrastructure
|       |-- variables.tf             # Configurable parameters
|       |-- outputs.tf               # IP, URLs, SSH command
|       |-- user-data.sh.tftpl       # EC2 bootstrap script
|       |-- teardown.sh              # Destroy all AWS resources
|       |-- terraform.tfvars.example # Example config (copy to terraform.tfvars)
|
|-- leaderboard/
|   |-- static/
|       |-- index.html               # Live leaderboard (dark theme, Chart.js)
|       |-- reveal.html              # Final ceremony reveal page
|       |-- css/style.css            # Projector-optimized dark theme
|       |-- js/leaderboard.js        # Polling, SSE, table rendering
|       |-- js/charts.js             # Chart.js bar charts
|       |-- js/reveal.js             # Animated reveal sequence
|
|-- presentation/
|   |-- slides.html                  # Reveal.js slide deck (open in browser)
|   |-- outline.md                   # Detailed speaker notes
|
|-- CMakeLists.txt                   # Build system (C++20, -O2)
|-- .gitignore                       # Excludes build/, IDE, terraform state
```

## Codebase In Depth

### C++ Challenge Code

#### `include/exchange/types.h`

All shared data types live here. Key design decisions:

- **Integer prices** (`int64_t`). Prices are in "ticks" (the minimum price increment), avoiding floating-point comparison issues. This mirrors how real exchanges represent prices internally.
- **Listener callback interface**. Trades and order updates are reported via virtual method callbacks (`onTrade`, `onOrderUpdate`) rather than return values. This avoids allocating vectors on the hot path and is more realistic -- real exchange APIs use callbacks. The test harness implements `Listener` to capture events for assertion; the benchmark implements a no-op `NullListener` to avoid measurement noise.
- **`OrderRequest` uses `std::string` for symbol**. This is intentional: it represents the realistic external interface. Students who want performance should intern symbols to integer IDs internally -- that optimization will show up clearly in benchmarks.

#### `include/exchange/matching_engine.h`

The fixed class interface. The header documents the full matching specification in comments. Students implement all methods in `src/matching_engine.cpp` but may add any private members, helper classes, or additional source files.

The two provided private members (`listener_`, `next_order_id_`) are there so the skeleton compiles immediately. Students add their own data structures below the `// TODO` comment.

#### `src/matching_engine.cpp`

The student skeleton. Every method has:
- Validation logic already written (reject invalid orders, validate amend params)
- Order ID assignment already written
- Step-by-step TODO comments describing what to implement
- Placeholder return values so it compiles immediately

The skeleton is designed so students can start building incrementally: first get `addOrder` working for a single symbol with no matching, then add matching, then cancel, then amend.

#### `reference/matching_engine.cpp`

A naive-but-correct implementation released at the start of Day 2. Uses standard library containers:
- `std::unordered_map<std::string, Book>` -- per-symbol order books
- `std::map<int64_t, std::list<InternalOrder>>` -- price levels with FIFO queues (bids and asks)
- `std::unordered_map<uint64_t, OrderLocation>` -- O(1) order lookup by ID for cancel/amend

This stores engine state in a global pointer (`g_data`) because the class header only exposes `listener_` and `next_order_id_` as private members. Students should add members directly to the class instead.

The matching logic is factored into a `matchOrder` helper that sweeps the opposite side of the book, emitting trades and updates as it goes. The `amendOrder` implementation removes the order, optionally re-matches at the new price, and re-inserts any remainder.

Performance: ~6M ops/sec on a modern laptop. This is the baseline students optimize from.

#### `test/test_correctness.cpp`

29 tests organized into categories:

| Category | Tests | What it validates |
|---|---|---|
| Basic matching | 4 | Exact match both directions, crossing prices |
| No match | 2 | Same-side orders, non-crossing prices |
| Partial fills | 2 | Incoming larger/smaller than resting |
| Multi-level sweep | 1 | Incoming order matches across 3 price levels |
| Price-time priority | 2 | FIFO at same price, best price first |
| Cancel | 3 | Cancel resting, cancel nonexistent, cancel prevents match |
| Amend | 5 | Qty down keeps priority, price change/qty up lose priority, amend triggers match, invalid params |
| Rejection | 4 | Zero price, zero qty, empty symbol, negative price |
| Multi-symbol | 1 | AAPL orders don't match GOOG |
| Book snapshot | 3 | Bid/ask level aggregation, empty symbol |
| Order updates | 1 | Both resting and incoming get fill updates |

The test harness uses a `TestListener` that captures trades and updates into vectors. Each test creates a fresh `MatchingEngine`, runs a sequence of operations, and asserts on the captured events. Tests stop on first failure with a file/line diagnostic.

The `TEST` macro creates an isolated test function with its own engine and listener, so tests are fully independent.

#### `bench/benchmark.cpp`

The benchmark harness measures per-operation latency using `std::chrono::steady_clock`:

1. Pre-generates all operations into a vector (so generation cost isn't measured)
2. Runs a warm-up phase (50,000 ops) to populate caches and trigger JIT-like optimizations
3. Measures 1,000,000 ops individually, recording nanosecond timestamps
4. Runs 3 iterations per scenario, takes the median p50 as the ranking metric
5. Reports: min, mean, p50, p90, p99, max, stddev, throughput
6. Supports `--json` flag for machine-readable output (used by the submission pipeline)

Uses a `NullListener` (empty virtual methods) to avoid measurement noise from trade recording.

Three scenarios with different characteristics:

| Scenario | Seed | Spread width | Add/Cancel/Amend ratio | Purpose |
|---|---|---|---|---|
| Uniform | 42 | 20 ticks | 70/20/10 | Baseline throughput, narrow book |
| Realistic | 1337 | 50 ticks | 65/25/10 | Pressure events, wider book |
| Adversarial | 7777 | 200 ticks | 80/10/10 | Deep book, many price levels |

#### `bench/order_generator.h` and `bench/order_generator.cpp`

Deterministic order stream generator. Uses `std::mt19937_64` with a fixed seed so every team faces identical input. Features:

- Per-symbol fair values that drift via random walk (bounded between 100 and 50,000 ticks)
- Orders placed within a configurable spread of fair value
- Buy-side bias below fair value, sell-side bias above (realistic microstructure)
- Tracks live order IDs for valid cancel/amend targets (swap-and-pop removal)
- Realistic scenario includes rare "pressure events" (0.1% chance per tick of a large price jump)
- Quantities uniformly distributed 1-100 lots

#### `CMakeLists.txt`

Minimal CMake configuration:
- C++20 required (`CMAKE_CXX_STANDARD 20`)
- Release flags: `-O2 -march=native -DNDEBUG`
- Auto-globs `src/*.cpp` so students can add files without modifying the build
- Builds three targets: `matching_engine` (static lib), `test_correctness`, `benchmark`

### Infrastructure

#### Docker (`infra/docker/`)

Two containers:

**`Dockerfile.dev`** -- Development environment for students. Ubuntu 24.04 with g++-13, cmake, perf tools, valgrind, gdb. Students mount their source code and build inside the container, ensuring a consistent toolchain regardless of host OS (Mac/Windows/Linux).

**`Dockerfile.bench`** -- Sandboxed benchmark runner used by the server. Minimal attack surface, non-root user. The `entrypoint.sh` accepts three modes (`build`, `test`, `benchmark`), assembles the build tree from harness + student mounts, and runs cmake followed by the requested operation.

**`seccomp-profile.json`** -- Default-deny syscall policy. Allows standard C++ execution syscalls (file I/O, memory, threads, time). Blocks ptrace, mount, reboot, all networking, personality, keyctl, kernel modules, and BPF. Applied to benchmark containers to prevent students from escaping the sandbox.

#### Server (`infra/server/`)

A FastAPI application that receives GitHub webhooks and orchestrates the build/test/bench pipeline.

**`config.py`** -- Central configuration: team names, compiler flags (`-std=c++20 -O2 -Wall -Werror`), timeouts (build: 60s, test: 120s, bench: 300s), rate limit (120s), benchmark iterations (3), paths, Docker resource limits (1GB memory, 64 PIDs). All secrets via environment variables.

**`models.py`** -- SQLite persistence using plain `sqlite3` with thread-safe locking. Four tables:

| Table | Purpose |
|---|---|
| `submissions` | team_name, commit_hash, timestamp, status (queued/building/testing/benchmarking/complete/failed) |
| `build_results` | success flag, compiler output log, build duration |
| `test_results` | tests passed/total, per-test JSON details |
| `bench_results` | per-scenario metrics: mean, p50, p90, p99, min, max, stddev, throughput |

The `get_leaderboard()` function uses a CTE query that: finds submissions where all tests passed, computes weighted p50 per submission, selects each team's best submission, and ranks by composite score ascending.

**`pipeline.py`** -- Orchestrates the full submission pipeline:

```
Clone (depth 1) -> Build (Docker, 60s) -> Test (Docker, 120s) -> Benchmark (Docker, 300s) -> Store (SQLite)
```

Each step runs inside a Docker container with `--network=none`, `--memory=1g`, `--pids-limit=64`, `--read-only`, and the seccomp profile. On failure at any step, the error is stored and the pipeline stops. The student's CMakeLists.txt is never used -- the server applies its own.

**`app.py`** -- FastAPI endpoints:

| Endpoint | Method | Purpose |
|---|---|---|
| `/webhook` | POST | Receives GitHub push events. Validates `X-Hub-Signature-256`, extracts team from branch ref, checks rate limit, enqueues pipeline in background thread. |
| `/api/leaderboard` | GET | Returns ranked teams with throughput, latency, submission count. |
| `/api/status/{team}` | GET | Latest submission status with build/test/bench results. |
| `/api/stream` | GET | Server-Sent Events for live leaderboard updates. Heartbeat every 15s. |
| `/` | GET | Serves the leaderboard static files. |

The leaderboard API transforms the raw database results into the format the frontend expects: `team_name`, `throughput`, `avg_latency`, `p99_latency`, `submissions`, `last_updated`.

#### Scripts (`infra/scripts/`)

**`setup-server.sh`** -- One-time provisioning for an Ubuntu VM. Installs Docker, Python, creates a service user, sets CPU governor to performance, disables turbo boost (Intel + AMD), copies harness files, initializes the database, builds Docker images, installs a systemd service for the FastAPI app, and sets up a cron job that polls team branches every 5 minutes as a webhook backup.

**`create-team-branches.sh`** -- Takes team names as arguments. For each team: creates a `team/{name}` branch from main with the skeleton code, commits, and pushes.

**`run-pipeline.sh`** -- Manual fallback for when webhooks fail. Takes a team name, resolves the latest commit from the remote branch, and runs the pipeline directly.

#### Terraform (`infra/terraform/`)

AWS infrastructure as code. Provisions:

- EC2 instance (default `c5.xlarge`, compute-optimized) with Ubuntu 24.04
- Security group: SSH (port 22, configurable CIDR), HTTP (port 8000, open)
- Elastic IP for stable addressing across stop/start cycles
- Optional: custom VPC with public subnet, or use the default VPC
- Optional: dedicated tenancy for benchmark consistency

The `user-data.sh.tftpl` template bootstraps the instance on first boot: clones the repo (with optional GitHub token for private repos, stripped from remote URL after clone), runs `setup-server.sh`, writes a completion marker.

All secrets (webhook secret, GitHub token) flow through Terraform sensitive variables. `terraform.tfvars` is gitignored.

**`teardown.sh`** -- Post-event cleanup: `terraform destroy`, optionally removes GitHub webhooks (via `gh` CLI), and with `--clean-branches` deletes all `team/*` remote branches.

### Leaderboard (`leaderboard/static/`)

A zero-build-step web app using vanilla HTML/CSS/JS with CDN dependencies (Chart.js 4.x, Google Fonts).

**`index.html`** -- Live leaderboard for projecting during the event. Dark theme (#0d1117), large fonts for projector readability. Header shows team count, submission count, connection status. Table displays: rank, team name, throughput, avg latency, p99 latency, submissions, last update. Top 3 highlighted with gold/silver/bronze. Bottom ticker shows recent submission activity. Auto-refreshes via SSE with 10-second polling fallback.

**`reveal.html`** -- Ceremony page for the final reveal. Keyboard-controlled (spacebar/arrow to advance). Sequence: event stats with count-up animation, teams revealed last-to-first with metrics and rank, top 3 with podium styling, winner with confetti (canvas-confetti library). The presenter controls pacing entirely.

**`js/leaderboard.js`** -- Fetches `/api/leaderboard`, connects to `/api/stream` SSE with exponential-backoff reconnect, detects data changes per-team and triggers CSS animations, maintains an activity log for the ticker.

**`js/charts.js`** -- Chart.js 4.x visualizations: horizontal bar chart (throughput per team) and grouped bar chart (p50 vs p99 latency). Dark theme styling, lazy creation, smooth updates.

**`js/reveal.js`** -- Builds a slide array from leaderboard data. Count-up animations, staggered delays, confetti burst on winner reveal. Progress indicator shows advancement.

**`css/style.css`** -- Full dark theme with animations (row flash/pulse on update), responsive layout optimized for 1920x1080 projection, gold/silver/bronze accent colors, scrolling ticker.

### Presentation (`presentation/`)

**`slides.html`** -- Self-contained Reveal.js 5.x presentation (1,047 lines). 41 slides covering Day 1 (markets, order books, performance concepts, challenge intro), Day 2 opening (optimization hints, profiling), and Day 2 closing (real exchanges, skill transfer). Includes 8 inline SVG diagrams, 35 fragment animations, 19 syntax-highlighted code blocks, and speaker notes on every slide. Open directly in a browser to present; press `S` for speaker view.

**`outline.md`** -- Detailed speaker notes and timing guide for the presenter. Includes suggested follow-up questions for team presentations, equipment checklist, and appendix with per-section timing table.

## Tips

1. **Start with correctness.** A fast engine that produces wrong results scores zero. Get all 29 tests passing first.

2. **Use a profiler.** Run `perf stat ./build/benchmark` (Linux) or Instruments (macOS) to see where time is actually spent. The bottleneck is almost never where you think.

3. **Think about memory.**
   - What data structure holds your price levels? (`std::map`? sorted vector? direct-indexed array?)
   - What holds orders at a given level? (`std::list`? `std::deque`? intrusive list?)
   - How many heap allocations per `addOrder`? Can you get that to zero?

4. **Intern your strings.** Map each symbol to an integer ID once in `addOrder`, then use the integer internally.

5. **Know your data.** Benchmark prices center around tick 10,000 with bounded drift. If prices are bounded, a direct-indexed array is O(1) lookup with zero allocations.

6. **Measure before and after every change.** Run the benchmark, write down the numbers, make one change, run again. If no improvement, revert.

7. **Compile in Release mode.** Debug builds (`-O0`) can be 5-10x slower.
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   ```

8. **Read the tests.** The 29 tests in `test/test_correctness.cpp` are the specification. If you're unsure about an edge case, the test tells you.

9. **Read the reference implementation.** Even if you don't copy it, understanding the naive approach helps you see where the optimization opportunities are.

10. **Don't optimize everything.** `getBookSnapshot` and `getOrderCount` are never called during benchmarking. Focus your effort on `addOrder`, `cancelOrder`, and `amendOrder`.
