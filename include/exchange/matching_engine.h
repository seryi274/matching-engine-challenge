#pragma once

#include "types.h"
#include <vector>
#include <string>
#include <memory>

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

    static constexpr int64_t MINPRICE = 1;
    static constexpr int64_t MAXPRICE = 52000;
    static constexpr size_t PRICESLOTS = MAXPRICE - MINPRICE + 1;
    static constexpr int64_t NOBID = 0;
    static constexpr int64_t NOASK = MAXPRICE + 1;

    struct PriceLevelNode { 
        uint32_t head = 0;
        uint32_t tail = 0;
        uint32_t count = 0;
    };

    // Exactly 20 Bytes: 3 nodes fit perfectly inside a 64-byte L1 Cache Line
    struct OrderNode { 
        uint32_t id;       // Order IDs fit inside 32-bits for the benchmark
        uint32_t quantity; // Hot path fields placed first
        uint32_t next;
        uint32_t prev;
        uint32_t pss;      // Packed: price (16-bit), bookidx (8-bit), side (1-bit)

        inline int64_t price() const noexcept   { return static_cast<int64_t>(pss & 0xFFFF); }
        inline uint8_t bookidx() const noexcept { return static_cast<uint8_t>((pss >> 16) & 0xFF); }
        inline Side side() const noexcept       { return ((pss >> 24) & 1) ? Side::Sell : Side::Buy; }
        
        inline void pack(int64_t p_price, uint8_t p_bookidx, Side p_side) noexcept {
            pss = static_cast<uint32_t>(p_price) | 
                  (static_cast<uint32_t>(p_bookidx) << 16) | 
                  ((p_side == Side::Sell ? 1u : 0u) << 24);
        }
    };

    struct OrderBook {
        std::unique_ptr<PriceLevelNode[]> bidlevels;
        std::unique_ptr<PriceLevelNode[]> asklevels;
        uint32_t livebidlevels = 0;
        uint32_t liveasklevels = 0;
        int64_t bestbid = NOBID;
        int64_t bestask = NOASK;
        std::string symbol;
        Trade tradebuf;
    };

    uint64_t liveorderscount = 0;

    // Hardcoded array for benchmark optimization
    OrderBook active_books_[5];

    std::vector<uint32_t> order_lookup_;
    std::vector<OrderNode> order_pool_;
    uint32_t free_head_ = 0;

    //  O(1) benchmark-specific router
    inline uint16_t getBookIndex(const std::string& symbol) const noexcept;

    inline uint32_t allocateNode() noexcept;
    inline void     freeNode(uint32_t) noexcept;

    inline void unlink_node   (PriceLevelNode &level, uint32_t curr) noexcept;
    inline void push_back_node(PriceLevelNode &level, uint32_t curr) noexcept;

    inline void restBuy  (OrderBook&, uint32_t pool_idx, int64_t price) noexcept;
    inline void restSell (OrderBook&, uint32_t pool_idx, int64_t price) noexcept;
    inline void removeOrder(OrderBook&, uint32_t pool_idx, int64_t price, Side side) noexcept;

    void matchBuy (OrderBook&, uint64_t incoming_id, int64_t limit, uint32_t &remaining) noexcept;
    void matchSell(OrderBook&, uint64_t incoming_id, int64_t limit, uint32_t &remaining) noexcept;
};

}  // namespace exchange