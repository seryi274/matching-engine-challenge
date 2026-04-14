#include "order_generator.h"
#include <algorithm>
#include <cmath>

namespace exchange {

OrderGenerator::OrderGenerator(uint64_t seed, Scenario scenario,
                               const std::vector<std::string>& symbols)
    : rng_(seed), scenario_(scenario), symbols_(symbols)
{
    fair_values_.resize(symbols_.size());
    for (size_t i = 0; i < symbols_.size(); ++i) {
        fair_values_[i] = 10000.0;  // Start at tick 10000
    }

    switch (scenario_) {
        case Scenario::Uniform:
            add_ratio_    = 0.70;
            cancel_ratio_ = 0.20;
            break;
        case Scenario::Realistic:
            add_ratio_    = 0.65;
            cancel_ratio_ = 0.25;
            break;
        case Scenario::Adversarial:
            add_ratio_    = 0.80;
            cancel_ratio_ = 0.10;
            break;
    }
}

std::vector<BenchAction> OrderGenerator::generate(size_t count) {
    std::vector<BenchAction> actions;
    actions.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        advanceFairValues();

        std::uniform_real_distribution<double> dist(0.0, 1.0);
        double roll = dist(rng_);

        if (roll < add_ratio_ || live_order_ids_.empty()) {
            actions.push_back(generateAdd());
        } else if (roll < add_ratio_ + cancel_ratio_) {
            actions.push_back(generateCancel());
        } else {
            actions.push_back(generateAmend());
        }
    }

    return actions;
}

BenchAction OrderGenerator::generateAdd() {
    BenchAction action;
    action.type = ActionType::Add;

    // Pick a random symbol
    std::uniform_int_distribution<size_t> sym_dist(0, symbols_.size() - 1);
    size_t sym_idx = sym_dist(rng_);

    // Pick side
    std::uniform_int_distribution<int> side_dist(0, 1);
    Side side = static_cast<Side>(side_dist(rng_));

    // Price around fair value
    double fair = fair_values_[sym_idx];
    int spread_width = 20;

    switch (scenario_) {
        case Scenario::Uniform:
            spread_width = 20;  // Narrow band
            break;
        case Scenario::Realistic:
            spread_width = 50;  // Wider, more realistic
            break;
        case Scenario::Adversarial:
            spread_width = 200; // Very wide, creates deep book
            break;
    }

    std::uniform_int_distribution<int> price_offset(-spread_width, spread_width);
    int64_t price = static_cast<int64_t>(fair) + price_offset(rng_);
    if (price <= 0) price = 1;

    // Bias: buys tend to be below fair, sells above
    if (side == Side::Buy) {
        price = std::min(price, static_cast<int64_t>(fair) + 5);
    } else {
        price = std::max(price, static_cast<int64_t>(fair) - 5);
    }
    if (price <= 0) price = 1;

    // Quantity
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);
    uint32_t quantity = qty_dist(rng_);

    action.request = OrderRequest{symbols_[sym_idx], side, price, quantity};

    // Track that this order ID will be live (optimistically -- it may fill)
    live_order_ids_.push_back(next_expected_id_);
    next_expected_id_++;

    return action;
}

BenchAction OrderGenerator::generateCancel() {
    BenchAction action;
    action.type = ActionType::Cancel;

    // Pick a random live order
    std::uniform_int_distribution<size_t> idx_dist(0, live_order_ids_.size() - 1);
    size_t idx = idx_dist(rng_);

    action.target_order_id = live_order_ids_[idx];

    // Remove from our tracking (swap-and-pop)
    live_order_ids_[idx] = live_order_ids_.back();
    live_order_ids_.pop_back();

    return action;
}

BenchAction OrderGenerator::generateAmend() {
    BenchAction action;
    action.type = ActionType::Amend;

    // Pick a random live order
    std::uniform_int_distribution<size_t> idx_dist(0, live_order_ids_.size() - 1);
    size_t idx = idx_dist(rng_);

    action.target_order_id = live_order_ids_[idx];

    // Random new price and quantity
    std::uniform_int_distribution<int> price_offset(-10, 10);
    // We don't know the original price, so pick something reasonable
    // In a real scenario we'd track this, but for benchmarking the
    // engine must handle arbitrary values gracefully
    action.new_price = 10000 + price_offset(rng_);
    if (action.new_price <= 0) action.new_price = 1;

    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);
    action.new_quantity = qty_dist(rng_);

    return action;
}

void OrderGenerator::advanceFairValues() {
    std::normal_distribution<double> drift(0.0, 0.5);

    for (auto& fv : fair_values_) {
        fv += drift(rng_);
        if (fv < 100.0) fv = 100.0;    // Floor
        if (fv > 50000.0) fv = 50000.0; // Ceiling
    }

    // Scenario-specific behavior
    if (scenario_ == Scenario::Realistic) {
        // Occasionally create pressure events (sudden moves)
        std::uniform_real_distribution<double> event_dist(0.0, 1.0);
        if (event_dist(rng_) < 0.001) {  // 0.1% chance per tick
            std::uniform_int_distribution<size_t> sym_dist(0, fair_values_.size() - 1);
            size_t idx = sym_dist(rng_);
            std::normal_distribution<double> jump(0.0, 50.0);
            fair_values_[idx] += jump(rng_);
            if (fair_values_[idx] < 100.0) fair_values_[idx] = 100.0;
        }
    }
}

}  // namespace exchange
