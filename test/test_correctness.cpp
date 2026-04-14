#include "exchange/matching_engine.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace exchange;

// ============================================================
//  Test Listener -- captures all callbacks for assertions
// ============================================================

class TestListener : public Listener {
public:
    std::vector<Trade>       trades;
    std::vector<OrderUpdate> updates;

    void onTrade(const Trade& trade) override {
        trades.push_back(trade);
    }

    void onOrderUpdate(const OrderUpdate& update) override {
        updates.push_back(update);
    }

    void clear() {
        trades.clear();
        updates.clear();
    }
};

// ============================================================
//  Test macros
// ============================================================

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                                               \
    static void test_##name(TestListener& L, MatchingEngine& E); \
    static void run_##name() {                                   \
        tests_run++;                                             \
        TestListener L;                                          \
        MatchingEngine E(&L);                                    \
        std::printf("  [%2d] %-50s ", tests_run, #name);        \
        test_##name(L, E);                                       \
        tests_passed++;                                          \
        std::printf("PASS\n");                                   \
    }                                                            \
    static void test_##name(TestListener& L, MatchingEngine& E)

#define ASSERT_EQ(a, b) do {                                     \
    auto _a = (a); auto _b = (b);                                \
    if (_a != _b) {                                              \
        std::printf("FAIL\n    %s:%d: %s != %s\n",              \
            __FILE__, __LINE__, #a, #b);                         \
        std::exit(1);                                            \
    }                                                            \
} while(0)

#define ASSERT_TRUE(x) do {                                      \
    if (!(x)) {                                                  \
        std::printf("FAIL\n    %s:%d: %s is false\n",           \
            __FILE__, __LINE__, #x);                             \
        std::exit(1);                                            \
    }                                                            \
} while(0)

// ============================================================
//  Tests
// ============================================================

// --- Basic Matching ---

TEST(exact_match_buy_then_sell) {
    auto ack1 = E.addOrder({"AAPL", Side::Buy, 100, 10});
    ASSERT_EQ(ack1.status, OrderStatus::Accepted);
    ASSERT_EQ(L.trades.size(), 0u);
    L.clear();

    auto ack2 = E.addOrder({"AAPL", Side::Sell, 100, 10});
    ASSERT_EQ(ack2.status, OrderStatus::Filled);
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].buy_order_id, ack1.order_id);
    ASSERT_EQ(L.trades[0].sell_order_id, ack2.order_id);
    ASSERT_EQ(L.trades[0].price, 100);
    ASSERT_EQ(L.trades[0].quantity, 10u);
    ASSERT_EQ(L.trades[0].symbol, std::string("AAPL"));
    ASSERT_EQ(E.getOrderCount(), 0u);
}

TEST(exact_match_sell_then_buy) {
    auto ack1 = E.addOrder({"GOOG", Side::Sell, 200, 5});
    ASSERT_EQ(ack1.status, OrderStatus::Accepted);
    L.clear();

    auto ack2 = E.addOrder({"GOOG", Side::Buy, 200, 5});
    ASSERT_EQ(ack2.status, OrderStatus::Filled);
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].price, 200);
    ASSERT_EQ(L.trades[0].quantity, 5u);
    ASSERT_EQ(E.getOrderCount(), 0u);
}

TEST(buy_crosses_above_ask) {
    E.addOrder({"AAPL", Side::Sell, 100, 10});
    L.clear();

    // Buy at 105 should match at the resting sell's price (100)
    auto ack = E.addOrder({"AAPL", Side::Buy, 105, 10});
    ASSERT_EQ(ack.status, OrderStatus::Filled);
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].price, 100);  // resting price
}

TEST(sell_crosses_below_bid) {
    E.addOrder({"AAPL", Side::Buy, 100, 10});
    L.clear();

    auto ack = E.addOrder({"AAPL", Side::Sell, 95, 10});
    ASSERT_EQ(ack.status, OrderStatus::Filled);
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].price, 100);  // resting price
}

// --- No Match ---

TEST(no_match_same_side) {
    E.addOrder({"AAPL", Side::Buy, 100, 10});
    auto ack = E.addOrder({"AAPL", Side::Buy, 101, 5});
    ASSERT_EQ(ack.status, OrderStatus::Accepted);
    ASSERT_EQ(E.getOrderCount(), 2u);
}

TEST(no_match_prices_dont_cross) {
    E.addOrder({"AAPL", Side::Buy, 99, 10});
    L.clear();

    auto ack = E.addOrder({"AAPL", Side::Sell, 100, 10});
    ASSERT_EQ(ack.status, OrderStatus::Accepted);
    ASSERT_EQ(L.trades.size(), 0u);
    ASSERT_EQ(E.getOrderCount(), 2u);
}

// --- Partial Fills ---

TEST(partial_fill_incoming_larger) {
    auto ack1 = E.addOrder({"AAPL", Side::Sell, 100, 5}); (void)ack1;
    L.clear();

    auto ack2 = E.addOrder({"AAPL", Side::Buy, 100, 10});
    ASSERT_EQ(ack2.status, OrderStatus::Accepted);  // 5 remaining
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].quantity, 5u);
    ASSERT_EQ(E.getOrderCount(), 1u);  // buy order resting with 5 remaining
}

TEST(partial_fill_resting_larger) {
    auto ack1 = E.addOrder({"AAPL", Side::Buy, 100, 20}); (void)ack1;
    L.clear();

    auto ack2 = E.addOrder({"AAPL", Side::Sell, 100, 8});
    ASSERT_EQ(ack2.status, OrderStatus::Filled);
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].quantity, 8u);
    ASSERT_EQ(E.getOrderCount(), 1u);  // buy resting with 12 remaining

    auto snapshot = E.getBookSnapshot("AAPL", Side::Buy);
    ASSERT_EQ(snapshot.size(), 1u);
    ASSERT_EQ(snapshot[0].total_quantity, 12u);
}

// --- Multi-level Sweep ---

TEST(sweep_multiple_price_levels) {
    E.addOrder({"AAPL", Side::Sell, 100, 5});
    E.addOrder({"AAPL", Side::Sell, 101, 5});
    E.addOrder({"AAPL", Side::Sell, 102, 5});
    L.clear();

    // Buy at 102 should sweep all three levels
    auto ack = E.addOrder({"AAPL", Side::Buy, 102, 12});
    ASSERT_EQ(L.trades.size(), 3u);
    ASSERT_EQ(L.trades[0].price, 100);  // best ask first
    ASSERT_EQ(L.trades[0].quantity, 5u);
    ASSERT_EQ(L.trades[1].price, 101);
    ASSERT_EQ(L.trades[1].quantity, 5u);
    ASSERT_EQ(L.trades[2].price, 102);
    ASSERT_EQ(L.trades[2].quantity, 2u);  // partial fill at worst level
    ASSERT_EQ(ack.status, OrderStatus::Filled);
}

// --- Price-Time Priority ---

TEST(price_time_priority_fifo) {
    auto ack1 = E.addOrder({"AAPL", Side::Sell, 100, 5});
    auto ack2 = E.addOrder({"AAPL", Side::Sell, 100, 5}); (void)ack2;
    L.clear();

    // Should match against ack1 first (arrived earlier)
    auto ack3 = E.addOrder({"AAPL", Side::Buy, 100, 5}); (void)ack3;
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].sell_order_id, ack1.order_id);
    ASSERT_EQ(E.getOrderCount(), 1u);  // ack2 still resting
}

TEST(price_priority_best_price_first) {
    auto ack1 = E.addOrder({"AAPL", Side::Sell, 102, 5}); (void)ack1;
    auto ack2 = E.addOrder({"AAPL", Side::Sell, 100, 5});
    auto ack3 = E.addOrder({"AAPL", Side::Sell, 101, 5});
    L.clear();

    // Should match at 100 first, then 101
    auto ack4 = E.addOrder({"AAPL", Side::Buy, 101, 8}); (void)ack4;
    ASSERT_EQ(L.trades.size(), 2u);
    ASSERT_EQ(L.trades[0].sell_order_id, ack2.order_id);  // price 100
    ASSERT_EQ(L.trades[0].price, 100);
    ASSERT_EQ(L.trades[1].sell_order_id, ack3.order_id);  // price 101
    ASSERT_EQ(L.trades[1].price, 101);
}

// --- Cancel ---

TEST(cancel_resting_order) {
    auto ack = E.addOrder({"AAPL", Side::Buy, 100, 10});
    ASSERT_EQ(E.getOrderCount(), 1u);
    L.clear();

    bool cancelled = E.cancelOrder(ack.order_id);
    ASSERT_TRUE(cancelled);
    ASSERT_EQ(E.getOrderCount(), 0u);
    ASSERT_EQ(L.updates.size(), 1u);
    ASSERT_EQ(L.updates[0].order_id, ack.order_id);
    ASSERT_EQ(L.updates[0].status, OrderStatus::Cancelled);
    ASSERT_EQ(L.updates[0].remaining_quantity, 0u);
}

TEST(cancel_nonexistent_order) {
    bool cancelled = E.cancelOrder(99999);
    ASSERT_TRUE(!cancelled);
}

TEST(cancel_prevents_future_match) {
    auto ack1 = E.addOrder({"AAPL", Side::Sell, 100, 10});
    E.cancelOrder(ack1.order_id);
    L.clear();

    auto ack2 = E.addOrder({"AAPL", Side::Buy, 100, 10});
    ASSERT_EQ(ack2.status, OrderStatus::Accepted);  // no match, rests
    ASSERT_EQ(L.trades.size(), 0u);
}

// --- Amend ---

TEST(amend_quantity_down_keeps_priority) {
    auto ack1 = E.addOrder({"AAPL", Side::Sell, 100, 10});
    auto ack2 = E.addOrder({"AAPL", Side::Sell, 100, 10}); (void)ack2;

    // Reduce ack1's quantity -- should keep priority
    bool amended = E.amendOrder(ack1.order_id, 100, 5);
    ASSERT_TRUE(amended);
    L.clear();

    // Match should hit ack1 first (still has priority)
    E.addOrder({"AAPL", Side::Buy, 100, 5});
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].sell_order_id, ack1.order_id);
    ASSERT_EQ(L.trades[0].quantity, 5u);
}

TEST(amend_price_loses_priority) {
    auto ack1 = E.addOrder({"AAPL", Side::Sell, 100, 10});
    auto ack2 = E.addOrder({"AAPL", Side::Sell, 100, 10});

    // Change ack1's price to same price -- loses priority
    bool amended = E.amendOrder(ack1.order_id, 99, 10);
    ASSERT_TRUE(amended);
    // Now amend back to 100 -- still at back of queue
    amended = E.amendOrder(ack1.order_id, 100, 10);
    ASSERT_TRUE(amended);
    L.clear();

    // Match should hit ack2 first (ack1 lost priority)
    E.addOrder({"AAPL", Side::Buy, 100, 10});
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].sell_order_id, ack2.order_id);
}

TEST(amend_quantity_up_loses_priority) {
    auto ack1 = E.addOrder({"AAPL", Side::Sell, 100, 5});
    auto ack2 = E.addOrder({"AAPL", Side::Sell, 100, 5});

    // Increase ack1 quantity -- loses priority
    bool amended = E.amendOrder(ack1.order_id, 100, 10);
    ASSERT_TRUE(amended);
    L.clear();

    // Match should hit ack2 first
    E.addOrder({"AAPL", Side::Buy, 100, 5});
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].sell_order_id, ack2.order_id);
}

TEST(amend_triggers_match) {
    auto ack1 = E.addOrder({"AAPL", Side::Buy, 100, 10});
    auto ack2 = E.addOrder({"AAPL", Side::Sell, 105, 10});
    L.clear();

    // Amend the sell down to 100 -- should trigger a match
    bool amended = E.amendOrder(ack2.order_id, 100, 10);
    ASSERT_TRUE(amended);
    ASSERT_EQ(L.trades.size(), 1u);
    ASSERT_EQ(L.trades[0].buy_order_id, ack1.order_id);
    ASSERT_EQ(L.trades[0].sell_order_id, ack2.order_id);
    ASSERT_EQ(L.trades[0].price, 100);
}

TEST(amend_nonexistent_order) {
    bool amended = E.amendOrder(99999, 100, 10);
    ASSERT_TRUE(!amended);
}

TEST(amend_invalid_params) {
    auto ack = E.addOrder({"AAPL", Side::Buy, 100, 10});
    ASSERT_TRUE(!E.amendOrder(ack.order_id, 0, 10));    // price 0
    ASSERT_TRUE(!E.amendOrder(ack.order_id, 100, 0));    // qty 0
    ASSERT_TRUE(!E.amendOrder(ack.order_id, -5, 10));    // negative price
}

// --- Rejection ---

TEST(reject_zero_price) {
    auto ack = E.addOrder({"AAPL", Side::Buy, 0, 10});
    ASSERT_EQ(ack.status, OrderStatus::Rejected);
    ASSERT_EQ(ack.order_id, 0u);
}

TEST(reject_zero_quantity) {
    auto ack = E.addOrder({"AAPL", Side::Buy, 100, 0});
    ASSERT_EQ(ack.status, OrderStatus::Rejected);
}

TEST(reject_empty_symbol) {
    auto ack = E.addOrder({"", Side::Buy, 100, 10});
    ASSERT_EQ(ack.status, OrderStatus::Rejected);
}

TEST(reject_negative_price) {
    auto ack = E.addOrder({"AAPL", Side::Buy, -10, 10});
    ASSERT_EQ(ack.status, OrderStatus::Rejected);
}

// --- Multi-Symbol ---

TEST(multi_symbol_isolation) {
    E.addOrder({"AAPL", Side::Buy, 100, 10});
    L.clear();

    // Sell GOOG should NOT match against AAPL buy
    auto ack = E.addOrder({"GOOG", Side::Sell, 100, 10});
    ASSERT_EQ(ack.status, OrderStatus::Accepted);
    ASSERT_EQ(L.trades.size(), 0u);
    ASSERT_EQ(E.getOrderCount(), 2u);
}

// --- Book Snapshot ---

TEST(book_snapshot_buy_side) {
    E.addOrder({"AAPL", Side::Buy, 100, 10});
    E.addOrder({"AAPL", Side::Buy, 100, 5});
    E.addOrder({"AAPL", Side::Buy, 99, 20});

    auto snapshot = E.getBookSnapshot("AAPL", Side::Buy);
    ASSERT_EQ(snapshot.size(), 2u);
    // Best bid first (highest price)
    ASSERT_EQ(snapshot[0].price, 100);
    ASSERT_EQ(snapshot[0].total_quantity, 15u);
    ASSERT_EQ(snapshot[0].order_count, 2u);
    ASSERT_EQ(snapshot[1].price, 99);
    ASSERT_EQ(snapshot[1].total_quantity, 20u);
    ASSERT_EQ(snapshot[1].order_count, 1u);
}

TEST(book_snapshot_sell_side) {
    E.addOrder({"AAPL", Side::Sell, 101, 10});
    E.addOrder({"AAPL", Side::Sell, 102, 5});

    auto snapshot = E.getBookSnapshot("AAPL", Side::Sell);
    ASSERT_EQ(snapshot.size(), 2u);
    // Best ask first (lowest price)
    ASSERT_EQ(snapshot[0].price, 101);
    ASSERT_EQ(snapshot[0].total_quantity, 10u);
    ASSERT_EQ(snapshot[1].price, 102);
    ASSERT_EQ(snapshot[1].total_quantity, 5u);
}

TEST(book_snapshot_empty_symbol) {
    auto snapshot = E.getBookSnapshot("UNKNOWN", Side::Buy);
    ASSERT_EQ(snapshot.size(), 0u);
}

// --- Order Updates ---

TEST(order_update_on_fill) {
    E.addOrder({"AAPL", Side::Buy, 100, 10});
    L.clear();

    E.addOrder({"AAPL", Side::Sell, 100, 10});
    // Should get updates for both resting (filled) and incoming (filled)
    bool found_resting_fill = false;
    bool found_incoming_fill = false;
    for (const auto& upd : L.updates) {
        if (upd.status == OrderStatus::Filled && upd.remaining_quantity == 0) {
            if (upd.order_id == 1) found_resting_fill = true;
            if (upd.order_id == 2) found_incoming_fill = true;
        }
    }
    ASSERT_TRUE(found_resting_fill);
    ASSERT_TRUE(found_incoming_fill);
}

// ============================================================
//  Runner
// ============================================================

int main() {
    std::printf("Running matching engine correctness tests...\n\n");

    run_exact_match_buy_then_sell();
    run_exact_match_sell_then_buy();
    run_buy_crosses_above_ask();
    run_sell_crosses_below_bid();
    run_no_match_same_side();
    run_no_match_prices_dont_cross();
    run_partial_fill_incoming_larger();
    run_partial_fill_resting_larger();
    run_sweep_multiple_price_levels();
    run_price_time_priority_fifo();
    run_price_priority_best_price_first();
    run_cancel_resting_order();
    run_cancel_nonexistent_order();
    run_cancel_prevents_future_match();
    run_amend_quantity_down_keeps_priority();
    run_amend_price_loses_priority();
    run_amend_quantity_up_loses_priority();
    run_amend_triggers_match();
    run_amend_nonexistent_order();
    run_amend_invalid_params();
    run_reject_zero_price();
    run_reject_zero_quantity();
    run_reject_empty_symbol();
    run_reject_negative_price();
    run_multi_symbol_isolation();
    run_book_snapshot_buy_side();
    run_book_snapshot_sell_side();
    run_book_snapshot_empty_symbol();
    run_order_update_on_fill();

    std::printf("\n%d/%d tests passed.\n", tests_passed, tests_run);

    if (tests_passed == tests_run) {
        std::printf("ALL TESTS PASSED!\n");
        return 0;
    } else {
        return 1;
    }
}
