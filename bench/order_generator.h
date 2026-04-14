#pragma once

#include "exchange/types.h"
#include <cstdint>
#include <random>
#include <string>
#include <vector>
#include <unordered_set>

namespace exchange {

/// Type of action the benchmark will execute.
enum class ActionType : uint8_t {
    Add    = 0,
    Cancel = 1,
    Amend  = 2
};

/// A single benchmark action (pre-generated).
struct BenchAction {
    ActionType type;

    // For Add:
    OrderRequest request;

    // For Cancel/Amend:
    uint64_t target_order_id = 0;

    // For Amend:
    int64_t  new_price    = 0;
    uint32_t new_quantity = 0;
};

/// Benchmark scenario type.
enum class Scenario : uint8_t {
    Uniform     = 0,  // Random buy/sell, narrow price band
    Realistic   = 1,  // Microstructure: bursts, pressure, reversals
    Adversarial = 2   // Deep book + sweeping orders
};

/// Deterministic order stream generator.
/// Same seed = same stream for all teams.
class OrderGenerator {
public:
    /// Construct with a seed and scenario.
    OrderGenerator(uint64_t seed, Scenario scenario,
                   const std::vector<std::string>& symbols);

    /// Generate a batch of actions.
    std::vector<BenchAction> generate(size_t count);

private:
    std::mt19937_64 rng_;
    Scenario scenario_;
    std::vector<std::string> symbols_;

    // Fair values per symbol (drift over time)
    std::vector<double> fair_values_;

    // Track live order IDs for cancel/amend targets
    std::vector<uint64_t> live_order_ids_;
    uint64_t next_expected_id_ = 1;

    // Scenario parameters
    double add_ratio_    = 0.70;
    double cancel_ratio_ = 0.20;
    // amend_ratio_ = 1.0 - add_ratio_ - cancel_ratio_

    BenchAction generateAdd();
    BenchAction generateCancel();
    BenchAction generateAmend();

    void advanceFairValues();
};

}  // namespace exchange
