#pragma once

// sanity check build
#include "types.h"
#include <vector>
#include <string>

namespace exchange {

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
    OrderAck addOrder(const OrderRequest& request) noexcept;

    // --------------------------------------------------------
    //  Order Cancellation
    // --------------------------------------------------------

    /// Cancel a resting order by its order_id.
    ///
    /// If found and still resting, remove from book and call
    /// listener->onOrderUpdate() with status = Cancelled.
    ///
    /// Returns true if cancelled, false if order not found or already filled.
    bool cancelOrder(uint64_t order_id) noexcept;

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
    bool amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) noexcept;

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
    //  STUDENT: Internal data structures and helpers.
    // ============================================================

    static constexpr int64_t MAX_TICK  = 50200;
    static constexpr int     N_SYMBOLS = 5;

    // 16-byte Order: 4 fit per 64-byte cache line.
    struct Order {
        uint32_t id;
        uint32_t next;
        uint32_t prev;
        uint16_t price;
        uint8_t  qty;
        uint8_t  symSide;       // bit 0 = side, bits 1..3 = symIdx
    };

    struct PriceBucket {
        uint32_t head     = 0;
        uint32_t tail     = 0;
        uint32_t count    = 0;
        uint32_t totalQty = 0;
    };

    struct Book {
        PriceBucket bids[MAX_TICK + 1];
        PriceBucket asks[MAX_TICK + 1];
        int64_t  bestBid = 0;
        int64_t  bestAsk = 0;
        uint32_t liveBidLevels = 0;
        uint32_t liveAskLevels = 0;
        Trade    tradeBuf;      // symbol prefilled per book
    };

    struct EngineState {
        Book books[N_SYMBOLS];
        std::vector<Order>    orderPool;
        std::vector<uint32_t> idToSlot;
        uint32_t              freeHead = 0;
        uint64_t              liveCount = 0;
    };

    static const char* const SYMBOL_NAMES[N_SYMBOLS];

    // ---- helper methods (implemented in .cpp) ------------------
    static int symbolToIndex(const std::string& s) noexcept;
    static uint8_t packSymSide(uint8_t sym, uint8_t side) noexcept;
    static uint8_t unpackSym(uint8_t ss) noexcept;
    static uint8_t unpackSide(uint8_t ss) noexcept;

    static uint32_t poolAcquire(EngineState& s) noexcept;
    static void     poolRelease(EngineState& s, uint32_t slot) noexcept;

    static void bucketPushBack(PriceBucket& b, EngineState& s, uint32_t slot) noexcept;
    static void bucketUnlink  (PriceBucket& b, EngineState& s, uint32_t slot) noexcept;

    static void restOrder(uint64_t oid, uint8_t symIdx, Side side,
                          int64_t price, uint32_t qty,
                          Book& book, EngineState& s) noexcept;
    static void unrestOrder(uint32_t slot, Book& book, EngineState& s) noexcept;

    template <Side SIDE>
    static uint32_t doMatch(uint64_t aggId, uint8_t symIdx, int64_t limit, uint32_t qty,
                            Book& book, EngineState& s, Listener* listener) noexcept;

    // ---- per-instance state ------------------------------------
    Listener*   listener_;
    uint64_t    next_order_id_ = 1;
    EngineState state_;             // direct embed, no pointer indirection
};

}  // namespace exchange
