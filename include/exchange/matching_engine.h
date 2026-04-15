#pragma once

#include "types.h"
#include <vector>
#include <string>

namespace exchange {

struct EngineState;  // defined in matching_engine.cpp


/// ============================================================
///  MatchingEngine
///
///  Students implement ALL methods in src/matching_engine.cpp.
///  They may add any private members, helper classes, or internal
///  data structures they wish. The public interface is FIXED --
///  DO NOT MODIFY THIS FILE.
///
///  MATCHING RULES:
///    - Price-time priority (FIFO at each price level)
///    - A buy order matches against resting sells if buy.price >= sell.price
///    - A sell order matches against resting buys  if sell.price <= buy.price
///    - Execution price is always the RESTING (passive) order's price
///    - Orders match greedily: fill as much as possible immediately
///    - Any unmatched remainder rests on the book
///
///  THREAD SAFETY:
///    - The engine is single-threaded. No locking is required.
///
/// ============================================================
class MatchingEngine {
public:
    /// Construct engine with a listener that receives trade/update callbacks.
    /// The listener pointer must remain valid for the engine's lifetime.
    explicit MatchingEngine(Listener* listener);

    ~MatchingEngine();

    // --------------------------------------------------------
    //  Order Entry
    // --------------------------------------------------------

    /// Submit a new limit order.
    ///
    /// The engine:
    ///   1. Assigns a unique order_id (monotonically increasing, starting at 1)
    ///   2. Attempts to match against resting orders on the opposite side
    ///   3. Calls listener->onTrade() for each execution
    ///   4. Calls listener->onOrderUpdate() for each affected resting order
    ///   5. Calls listener->onOrderUpdate() for the incoming order
    ///   6. If any quantity remains, rests the order on the book
    ///   7. Returns an OrderAck with the assigned order_id and status
    ///
    /// Reject if: price <= 0, quantity == 0, or symbol is empty.
    OrderAck addOrder(const OrderRequest& request);

    // --------------------------------------------------------
    //  Order Cancellation
    // --------------------------------------------------------

    /// Cancel a resting order by its order_id.
    ///
    /// If found and still resting, remove from book and call
    /// listener->onOrderUpdate() with status = Cancelled.
    ///
    /// Returns true if cancelled, false if order not found or already filled.
    bool cancelOrder(uint64_t order_id);

    // --------------------------------------------------------
    //  Order Amendment
    // --------------------------------------------------------

    /// Modify a resting order's price and/or quantity.
    ///
    /// Rules:
    ///   - Order must exist and be resting on the book
    ///   - Side and symbol cannot change
    ///   - If price changes: order LOSES time priority
    ///   - If only quantity decreases (price unchanged): KEEPS time priority
    ///   - If quantity increases (even if price unchanged): LOSES time priority
    ///   - new_quantity must be > 0, new_price must be > 0
    ///   - After amendment, the order MAY match against the opposite side
    ///
    /// Returns true if amended, false if order not found or invalid parameters.
    bool amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity);

    // --------------------------------------------------------
    //  Query (for testing / debugging -- NOT performance-critical)
    // --------------------------------------------------------

    /// Get a snapshot of one side of the book for a given symbol.
    /// Buy side:  highest price first (best bid at front)
    /// Sell side: lowest price first  (best ask at front)
    std::vector<PriceLevel> getBookSnapshot(const std::string& symbol, Side side) const;

    /// Get total number of live (resting) orders across all symbols.
    uint64_t getOrderCount() const;

private:
    // ============================================================
    //  STUDENT: Add your data structures here.
    //
    //  Suggested starting point (naive but correct):
    //    - std::unordered_map<std::string, OrderBook> books_
    //      where OrderBook contains two std::map<int64_t, std::list<Order>>
    //      (one for bids sorted descending, one for asks sorted ascending)
    //    - std::unordered_map<uint64_t, pointer/iterator> order_lookup_
    //      for O(1) cancel/amend by order_id
    //
    //  Better approaches to explore:
    //    - Flat sorted arrays with binary search
    //    - Custom memory pools / arena allocators
    //    - Cache-aligned price level nodes
    //    - Intrusive linked lists
    //    - Pre-allocated order arrays with free lists
    // ============================================================

    Listener* listener_;
    uint64_t  next_order_id_ = 1;

    // typed pointer to per-instance state; defined in the .cpp
    EngineState* state_ = nullptr;
};

}  // namespace exchange
