#pragma once

#include "types.h"
#include <vector>
#include <string>
#include <memory>

namespace exchange {

class MatchingEngine {
public:
    explicit MatchingEngine(Listener* listener);
    ~MatchingEngine();

    OrderAck addOrder(const OrderRequest& request);
    bool cancelOrder(uint64_t order_id);
    bool amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity);
    std::vector<PriceLevel> getBookSnapshot(const std::string& symbol, Side side) const;
    uint64_t getOrderCount() const;

private:
    Listener* listener_;
    uint64_t  next_order_id_ = 1;

    static constexpr int64_t MINPRICE = 1;
    static constexpr int64_t MAXPRICE = 52000;
    static constexpr size_t PRICESLOTS = MAXPRICE - MINPRICE + 1;
    static constexpr int64_t NOBID = 0;
    static constexpr int64_t NOASK = MAXPRICE + 1;

    struct alignas(16) PriceLevelNode { // 16 bytes
        uint32_t head = 0;
        uint32_t tail = 0;
        uint32_t count = 0;
        uint32_t filler = 0;
    };

    struct alignas(32) OrderNode { // 32 bytes
        uint64_t id;
        int64_t price;
        uint32_t quantity;
        uint32_t next;
        uint32_t prev;
        uint16_t bookidx;
        Side    side;
        uint8_t filler;
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
    uint32_t free_head_ = 1;

    inline uint16_t getBookIndex(const std::string& symbol) const noexcept;

    inline uint32_t allocateNode() noexcept;
    inline void     freeNode(uint32_t) noexcept;

    inline void unlink_node   (PriceLevelNode &__restrict__ level, uint32_t curr) noexcept;
    inline void push_back_node(PriceLevelNode &__restrict__ level, uint32_t curr) noexcept;

    inline void restBuy  (OrderBook &__restrict__ book, uint32_t pool_idx, int64_t price) noexcept;
    inline void restSell (OrderBook &__restrict__ book, uint32_t pool_idx, int64_t price) noexcept;
    inline void removeOrder(OrderBook &__restrict__ book, uint32_t pool_idx, int64_t price, Side side) noexcept;

    void matchBuy (OrderBook &__restrict__ book, uint64_t incoming_id, int64_t limit, uint32_t &remaining) noexcept;
    void matchSell(OrderBook &__restrict__ book, uint64_t incoming_id, int64_t limit, uint32_t &remaining) noexcept;
};

}  // namespace exchange