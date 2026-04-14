#pragma once

#include <cstdint>
#include <string>

namespace exchange {
    // ============================================================
    //  Enumerations
    // ============================================================

    enum class Side : uint8_t {
        Buy = 0,
        Sell = 1
    };

    enum class OrderStatus : uint8_t {
        Accepted = 0, // Order is live on the book
        Filled = 1, // Order fully filled
        Cancelled = 2, // Order cancelled by user
        Rejected = 3 // Order rejected (invalid parameters)
    };

    // ============================================================
    //  Core Structures
    // ============================================================

    /// Incoming order request. This is purely an input message --
    /// students design their own internal order representation.
    struct OrderRequest {
        std::string symbol; // Instrument identifier (e.g., "AAPL")
        Side side;
        int64_t price; // Price in ticks (integer, always > 0 for limit)
        uint32_t quantity; // Number of lots (always > 0)
    };

    /// Returned after addOrder.
    struct OrderAck {
        uint64_t order_id; // Assigned by engine (0 if rejected)
        OrderStatus status;
    };

    /// Emitted whenever two orders match.
    struct Trade {
        uint64_t buy_order_id;
        uint64_t sell_order_id;
        std::string symbol;
        int64_t price; // Execution price (passive order's price)
        uint32_t quantity; // Executed quantity
    };

    /// Emitted when an order's status changes.
    struct OrderUpdate {
        uint64_t order_id;
        OrderStatus status;
        uint32_t remaining_quantity; // 0 if filled or cancelled
    };

    // ============================================================
    //  Listener Interface
    // ============================================================

    /// The benchmark/test harness implements this and passes it to
    /// the MatchingEngine. Callbacks are invoked synchronously during
    /// addOrder / cancelOrder / amendOrder.
    ///
    /// Callback ordering: during a single addOrder call that matches
    /// against N resting orders, the engine must call onTrade() and
    /// onOrderUpdate() for each fill in price-time priority order,
    /// then onOrderUpdate() for the aggressor order itself.
    class Listener {
    public:
        virtual ~Listener() = default;

        virtual void onTrade(const Trade& trade) = 0;

        virtual void onOrderUpdate(const OrderUpdate& update) = 0;
    };

    // ============================================================
    //  Book Snapshot (for debugging / verification only)
    // ============================================================

    struct PriceLevel {
        int64_t price;
        uint32_t total_quantity;
        uint32_t order_count;
    };

    struct Order {
        uint64_t order_id;
        uint32_t quantity;
    };

    struct OrderBook {
        std::map<int64_t, std::list<Order>, std::greater<>> bids;
        std::map<int64_t, std::list<Order>> asks;
    };
} // namespace exchange
