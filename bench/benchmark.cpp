#include "exchange/matching_engine.h"
#include "order_generator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

using namespace exchange;
using Clock = std::chrono::steady_clock;

// ============================================================
//  Null listener -- does nothing, avoids measurement noise
// ============================================================

class NullListener : public Listener {
public:
    void onTrade(const Trade&) override {}
    void onOrderUpdate(const OrderUpdate&) override {}
};

// ============================================================
//  Statistics helpers
// ============================================================

struct Stats {
    int64_t min_ns;
    int64_t max_ns;
    double  mean_ns;
    double  stddev_ns;
    int64_t p50_ns;
    int64_t p90_ns;
    int64_t p99_ns;
    double  throughput_ops;
};

static Stats computeStats(std::vector<int64_t>& latencies, double total_seconds) {
    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();

    double sum = 0;
    for (auto v : latencies) sum += v;
    double mean = sum / n;

    double var_sum = 0;
    for (auto v : latencies) {
        double d = v - mean;
        var_sum += d * d;
    }

    return Stats{
        latencies.front(),
        latencies.back(),
        mean,
        std::sqrt(var_sum / n),
        latencies[n * 50 / 100],
        latencies[n * 90 / 100],
        latencies[n * 99 / 100],
        n / total_seconds
    };
}

// ============================================================
//  Run one scenario
// ============================================================

static Stats runScenario(Scenario scenario, size_t warmup_ops, size_t measure_ops) {
    static const std::vector<std::string> symbols = {
        "AAPL", "GOOG", "MSFT", "AMZN", "TSLA"
    };

    uint64_t seed = 0;
    switch (scenario) {
        case Scenario::Uniform:     seed = 42;   break;
        case Scenario::Realistic:   seed = 1337; break;
        case Scenario::Adversarial: seed = 7777; break;
    }

    OrderGenerator gen(seed, scenario, symbols);

    // Pre-generate all actions
    auto warmup_actions = gen.generate(warmup_ops);
    auto measure_actions = gen.generate(measure_ops);

    NullListener listener;
    MatchingEngine engine(&listener);

    // Warm-up phase (not measured)
    for (const auto& action : warmup_actions) {
        switch (action.type) {
            case ActionType::Add:
                engine.addOrder(action.request);
                break;
            case ActionType::Cancel:
                engine.cancelOrder(action.target_order_id);
                break;
            case ActionType::Amend:
                engine.amendOrder(action.target_order_id,
                                  action.new_price, action.new_quantity);
                break;
        }
    }

    // Measurement phase
    std::vector<int64_t> latencies;
    latencies.reserve(measure_ops);

    auto total_start = Clock::now();

    for (const auto& action : measure_actions) {
        auto start = Clock::now();

        switch (action.type) {
            case ActionType::Add:
                engine.addOrder(action.request);
                break;
            case ActionType::Cancel:
                engine.cancelOrder(action.target_order_id);
                break;
            case ActionType::Amend:
                engine.amendOrder(action.target_order_id,
                                  action.new_price, action.new_quantity);
                break;
        }

        auto end = Clock::now();
        latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    auto total_end = Clock::now();
    double total_seconds = std::chrono::duration<double>(total_end - total_start).count();

    return computeStats(latencies, total_seconds);
}

// ============================================================
//  Main
// ============================================================

static const char* scenarioName(Scenario s) {
    switch (s) {
        case Scenario::Uniform:     return "uniform";
        case Scenario::Realistic:   return "realistic";
        case Scenario::Adversarial: return "adversarial";
    }
    return "unknown";
}

static void printStats(const char* name, const Stats& s) {
    std::printf("  %-12s  min=%6lld  mean=%8.0f  p50=%6lld  p90=%6lld  p99=%6lld  "
                "max=%8lld  stddev=%8.0f  throughput=%10.0f ops/s\n",
                name, (long long)s.min_ns, s.mean_ns, (long long)s.p50_ns,
                (long long)s.p90_ns, (long long)s.p99_ns, (long long)s.max_ns,
                s.stddev_ns, s.throughput_ops);
}

int main(int argc, char* argv[]) {
    constexpr size_t WARMUP_OPS  = 50'000;
    constexpr size_t MEASURE_OPS = 1'000'000;
    constexpr int    ITERATIONS  = 3;

    bool json_output = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0) json_output = true;
    }

    struct ScenarioResult {
        Scenario scenario;
        double   weight;
        Stats    best;  // median of iterations
    };

    ScenarioResult scenarios[] = {
        {Scenario::Uniform,     0.30, {}},
        {Scenario::Realistic,   0.40, {}},
        {Scenario::Adversarial, 0.30, {}},
    };

    if (!json_output) {
        std::printf("Matching Engine Benchmark\n");
        std::printf("  Warmup:  %zu ops\n", WARMUP_OPS);
        std::printf("  Measure: %zu ops x %d iterations\n\n", MEASURE_OPS, ITERATIONS);
    }

    for (auto& sr : scenarios) {
        std::vector<Stats> iteration_stats;

        for (int iter = 0; iter < ITERATIONS; ++iter) {
            auto stats = runScenario(sr.scenario, WARMUP_OPS, MEASURE_OPS);
            iteration_stats.push_back(stats);
        }

        // Take median by p50
        std::sort(iteration_stats.begin(), iteration_stats.end(),
                  [](const Stats& a, const Stats& b) { return a.p50_ns < b.p50_ns; });
        sr.best = iteration_stats[ITERATIONS / 2];

        if (!json_output) {
            std::printf("Scenario: %s\n", scenarioName(sr.scenario));
            for (int i = 0; i < ITERATIONS; ++i) {
                char label[32];
                std::snprintf(label, sizeof(label), "iter %d", i + 1);
                printStats(label, iteration_stats[i]);
            }
            std::printf("  >> median:\n");
            printStats("RESULT", sr.best);
            std::printf("\n");
        }
    }

    // Composite score
    double weighted_p50 = 0;
    double weighted_throughput = 0;
    for (const auto& sr : scenarios) {
        weighted_p50 += sr.weight * sr.best.p50_ns;
        weighted_throughput += sr.weight * sr.best.throughput_ops;
    }

    if (json_output) {
        std::printf("{\n");
        std::printf("  \"scenarios\": {\n");
        for (size_t i = 0; i < 3; ++i) {
            const auto& sr = scenarios[i];
            std::printf("    \"%s\": {\n", scenarioName(sr.scenario));
            std::printf("      \"min_ns\": %lld,\n", (long long)sr.best.min_ns);
            std::printf("      \"mean_ns\": %.0f,\n", sr.best.mean_ns);
            std::printf("      \"p50_ns\": %lld,\n", (long long)sr.best.p50_ns);
            std::printf("      \"p90_ns\": %lld,\n", (long long)sr.best.p90_ns);
            std::printf("      \"p99_ns\": %lld,\n", (long long)sr.best.p99_ns);
            std::printf("      \"max_ns\": %lld,\n", (long long)sr.best.max_ns);
            std::printf("      \"stddev_ns\": %.0f,\n", sr.best.stddev_ns);
            std::printf("      \"throughput_ops\": %.0f\n", sr.best.throughput_ops);
            std::printf("    }%s\n", (i < 2) ? "," : "");
        }
        std::printf("  },\n");
        std::printf("  \"composite\": {\n");
        std::printf("    \"weighted_p50_ns\": %.0f,\n", weighted_p50);
        std::printf("    \"weighted_throughput_ops\": %.0f\n", weighted_throughput);
        std::printf("  }\n");
        std::printf("}\n");
    } else {
        std::printf("=== COMPOSITE SCORE ===\n");
        std::printf("  Weighted p50 latency:  %.0f ns\n", weighted_p50);
        std::printf("  Weighted throughput:   %.0f ops/s\n", weighted_throughput);
    }

    return 0;
}
