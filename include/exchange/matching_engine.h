#pragma once

#include "types.h"
#include <vector>
#include <string>
#include <memory>
#include <cstdlib>

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
    OrderAck addOrder(const OrderRequest& request);

    // --------------------------------------------------------
    //  Order Cancellation
    // --------------------------------------------------------

    /// Cancel a resting order by its order_id.
    bool cancelOrder(uint64_t order_id);

    // --------------------------------------------------------
    //  Order Amendment
    // --------------------------------------------------------

    /// Modify a resting order's price and/or quantity.
    bool amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity);

    // --------------------------------------------------------
    //  Query (for testing / debugging -- NOT performance-critical)
    // --------------------------------------------------------

    /// Get a snapshot of one side of the book for a given symbol.
    std::vector<PriceLevel> getBookSnapshot(const std::string& symbol, Side side) const;

    /// Get total number of live (resting) orders across all symbols.
    uint64_t getOrderCount() const;

private:
    Listener* listener_;
    uint64_t  next_order_id_ = 1;
    uint64_t  liveorderscount = 0;

    static constexpr int64_t MINPRICE = 1;
    static constexpr int64_t MAXPRICE = 52000;
    static constexpr int64_t NOBID = 0;
    static constexpr int64_t NOASK = MAXPRICE + 1;
    static constexpr uint32_t MAX_CAPACITY = 12000000; 

    struct PriceLevelNode { 
        uint32_t head = 0;
        uint32_t tail = 0;
    };

    struct alignas(16) OrderNode { 
        uint32_t id;
        uint32_t quantity;
        uint32_t next;
        uint32_t prev;
    };

    struct alignas(64) OrderBook {
        uint64_t bid_bits[815]; // 52001 / 64 = 812. Safe margin 815.
        uint64_t ask_bits[815];
        PriceLevelNode bidlevels[MAXPRICE + 2];
        PriceLevelNode asklevels[MAXPRICE + 2];
        int64_t bestbid = NOBID;
        int64_t bestask = NOASK;
        std::string symbol;
        Trade tradebuf;
    };

    OrderBook* __restrict__ active_books_ = nullptr;
    uint64_t* __restrict__ order_lookup_ = nullptr;
    OrderNode* __restrict__ order_pool_   = nullptr;
    uint32_t free_head_ = 1;

    inline uint64_t packLookup(uint32_t pool_idx, int64_t price, uint8_t bookidx, Side side) const noexcept {
        return (uint64_t)pool_idx | ((uint64_t)price << 32) | ((uint64_t)bookidx << 60) | ((uint64_t)(side == Side::Buy ? 1 : 0) << 63);
    }
    inline uint32_t unpackPoolIdx(uint64_t l) const noexcept { return (uint32_t)l; }
    inline int64_t  unpackPrice(uint64_t l) const noexcept   { return (int64_t)((l >> 32) & 0xFFFFFFF); }
    inline uint8_t  unpackBookIdx(uint64_t l) const noexcept { return (uint8_t)((l >> 60) & 0x7); }
    inline Side     unpackSide(uint64_t l) const noexcept    { return (l >> 63) ? Side::Buy : Side::Sell; }

    inline uint16_t getBookIndex(const std::string& symbol) const noexcept;
    inline void removeOrder(OrderBook &__restrict__ book, uint32_t pool_idx, int64_t price, Side side) noexcept;
    void matchBuy (OrderBook &__restrict__ book, uint64_t incoming_id, int64_t limit, uint32_t &remaining) noexcept;
    void  matchSell(OrderBook &__restrict__ book, uint64_t incoming_id, int64_t limit, uint32_t &remaining) noexcept;
}; 

}  // namespace exchange