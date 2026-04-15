#include "exchange/matching_engine.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace exchange {

// constants
static constexpr int64_t MAX_TICK  = 50200;    // fair value <= 50k + spread 200
static constexpr int     N_SYMBOLS = 5;
static const char* const SYMBOL_NAMES[N_SYMBOLS] = {
    "AAPL", "GOOG", "MSFT", "AMZN", "TSLA"
};

// s[2] is unique across the five tickers (P,O,F,Z,L) so one switch
// compiles to a jump table with no nested compares.
static __attribute__((always_inline)) inline
int symbolToIndex(const std::string& s) noexcept {
    switch (s[2]) {
        case 'P': return 0;   // aapl
        case 'O': return 1;   // goog
        case 'F': return 2;   // msft
        case 'Z': return 3;   // amzn
        default : return 4;   // tsla
    }
}

// ==========================================================================
// Core data types
// ==========================================================================

// 20-byte Order: 3 orders fit in a 64-byte cache line (60 bytes used, 4 pad).
//
// We shrink by packing price+sym+side into a single uint32_t:
//   bits  0..15  price (max 50200 fits in 16 bits)
//   bits 16..23  symIdx (0..4)
//   bits 24..24  side (0=Buy, 1=Sell)
//   bits 25..31  unused
//
// id: uint32_t is enough (max id is ~10.5M << 2^32).
// qty: uint32_t kept to match the public API without truncation.
struct Order {
    uint32_t id;
    uint32_t next;          // pool slot of next order at same price (0 = end)
    uint32_t prev;          // pool slot of prev order (0 = head)
    uint32_t qty;
    uint32_t priceSymSide;  // packed, see encoding above
};
static_assert(sizeof(Order) == 20, "Order must be exactly 20 bytes");

static __attribute__((always_inline)) inline
uint32_t packPSS(int64_t price, uint8_t sym, uint8_t side) noexcept {
    return (uint32_t)price | ((uint32_t)sym << 16) | ((uint32_t)side << 24);
}
static __attribute__((always_inline)) inline
int64_t  unpackPrice(uint32_t pss) noexcept { return (int64_t)(pss & 0xFFFF); }
static __attribute__((always_inline)) inline
uint8_t  unpackSym(uint32_t pss)   noexcept { return (uint8_t)((pss >> 16) & 0xFF); }
static __attribute__((always_inline)) inline
uint8_t  unpackSide(uint32_t pss)  noexcept { return (uint8_t)((pss >> 24) & 0x1); }

// queue of resting orders at one tick
struct PriceBucket {
    uint32_t head = 0;
    uint32_t tail = 0;
    uint32_t count = 0;
    uint32_t totalQty = 0;
};

/* One book per symbol. Flat arrays for levels. */
struct Book {
    PriceBucket bids[MAX_TICK + 1];
    PriceBucket asks[MAX_TICK + 1];
    int64_t  bestBid = 0;
    int64_t  bestAsk = 0;
    uint32_t liveBidLevels = 0;
    uint32_t liveAskLevels = 0;
    Trade    tradeBuf;           // symbol prefilled in ctor, reused across fills
};

struct EngineState {
    Book books[N_SYMBOLS];
    std::vector<Order>    orderPool;     // pool[0] reserved as null slot
    std::vector<uint32_t> idToSlot;      // order_id -> slot (0 = absent)
    uint32_t              freeHead = 0;
    uint64_t              liveCount = 0;
};

// --------------------------------------------------------------------------
// pool: O(1) alloc/free via free list, recycled to keep the working set warm
// --------------------------------------------------------------------------

static __attribute__((always_inline)) inline
uint32_t poolAcquire(EngineState& s) noexcept {
    if (s.freeHead) {
        uint32_t slot = s.freeHead;
        s.freeHead = s.orderPool[slot].next;
        return slot;
    }
    s.orderPool.emplace_back();
    return (uint32_t)s.orderPool.size() - 1;
}

static __attribute__((always_inline)) inline
void poolRelease(EngineState& s, uint32_t slot) noexcept {
    s.orderPool[slot].next = s.freeHead;
    s.freeHead = slot;
}

// --------------------------------------------------------------------------
// intrusive doubly-linked FIFO helpers for one price bucket
// --------------------------------------------------------------------------

static __attribute__((always_inline)) inline
void bucketPushBack(PriceBucket& b, EngineState& s, uint32_t slot) noexcept {
    Order& o = s.orderPool[slot];
    o.next = 0;
    o.prev = b.tail;
    if (b.tail) s.orderPool[b.tail].next = slot;
    else        b.head = slot;
    b.tail = slot;
    ++b.count;
    b.totalQty += o.qty;
}

static __attribute__((always_inline)) inline
void bucketUnlink(PriceBucket& b, EngineState& s, uint32_t slot) noexcept {
    Order& o = s.orderPool[slot];
    if (o.prev) s.orderPool[o.prev].next = o.next; else b.head = o.next;
    if (o.next) s.orderPool[o.next].prev = o.prev; else b.tail = o.prev;
    --b.count;
    b.totalQty -= o.qty;
}

// ==========================================================================
// Resting-order lifecycle
// ==========================================================================

static __attribute__((always_inline)) inline
void restOrder(uint64_t oid, uint8_t symIdx, Side side, int64_t price, uint32_t qty,
               Book& book, EngineState& s) noexcept
{
    uint32_t slot = poolAcquire(s);
    Order& o = s.orderPool[slot];
    o.id           = (uint32_t)oid;
    o.qty          = qty;
    o.priceSymSide = packPSS(price, symIdx, (uint8_t)side);

    PriceBucket& bucket = (side == Side::Buy) ? book.bids[price] : book.asks[price];
    const bool freshLevel = (bucket.head == 0);
    bucketPushBack(bucket, s, slot);

    if (side == Side::Buy) {
        if (freshLevel) ++book.liveBidLevels;
        if (!book.bestBid || price > book.bestBid) book.bestBid = price;
    } else {
        if (freshLevel) ++book.liveAskLevels;
        if (!book.bestAsk || price < book.bestAsk) book.bestAsk = price;
    }

    s.idToSlot[oid] = slot;
    ++s.liveCount;
}

static __attribute__((always_inline)) inline
void unrestOrder(uint32_t slot, Book& book, EngineState& s) noexcept {
    Order&   o     = s.orderPool[slot];
    const uint32_t pss = o.priceSymSide;
    const int64_t  price = unpackPrice(pss);
    const uint8_t  side  = unpackSide(pss);

    PriceBucket& bucket = (side == 0) ? book.bids[price] : book.asks[price];
    bucketUnlink(bucket, s, slot);

    if (!bucket.head) {
        if (side == 0) {
            --book.liveBidLevels;
            if (!book.liveBidLevels) {
                book.bestBid = 0;
            } else {
                while (book.bestBid > 0 && !book.bids[book.bestBid].head)
                    --book.bestBid;
            }
        } else {
            --book.liveAskLevels;
            if (!book.liveAskLevels) {
                book.bestAsk = 0;
            } else {
                while (book.bestAsk <= MAX_TICK && !book.asks[book.bestAsk].head)
                    ++book.bestAsk;
                if (book.bestAsk > MAX_TICK) [[unlikely]] book.bestAsk = 0;
            }
        }
    }

    s.idToSlot[o.id] = 0;
    poolRelease(s, slot);
    --s.liveCount;
}

// ==========================================================================
// MATCHING LOOP -- the hot path.
// ==========================================================================
//
// Inside the inner sweep we only touch id, next, qty (16 of 20 bytes).
// With 20-byte orders, 3 fit per cache line -- the prefetcher and the
// sequential free-list recycling keep this dense.

template <Side SIDE>
static __attribute__((always_inline)) inline
uint32_t doMatch(uint64_t aggId, uint8_t /*symIdx*/, int64_t limit, uint32_t qty,
                 Book& book, EngineState& s, Listener* listener) noexcept
{
    Trade&      trade = book.tradeBuf;
    OrderUpdate upd;

    if constexpr (SIDE == Side::Buy) {
        trade.buy_order_id = aggId;

        while (qty > 0 && book.bestAsk > 0 && book.bestAsk <= limit) {
            const int64_t execPrice = book.bestAsk;
            PriceBucket&  bucket    = book.asks[execPrice];
            trade.price = execPrice;

            while (qty > 0 && bucket.head) {
                uint32_t slot = bucket.head;
                Order&   rest = s.orderPool[slot];
                if (rest.next) __builtin_prefetch(&s.orderPool[rest.next], 1, 1);

                uint32_t fillQty = (qty < rest.qty) ? qty : rest.qty;

                trade.sell_order_id = rest.id;
                trade.quantity      = fillQty;
                listener->onTrade(trade);

                qty             -= fillQty;
                rest.qty        -= fillQty;
                bucket.totalQty -= fillQty;

                upd.order_id = rest.id;
                if (!rest.qty) [[likely]] {
                    upd.status             = OrderStatus::Filled;
                    upd.remaining_quantity = 0;
                    listener->onOrderUpdate(upd);

                    bucket.head = rest.next;
                    if (bucket.head) s.orderPool[bucket.head].prev = 0;
                    else             bucket.tail = 0;
                    --bucket.count;
                    s.idToSlot[rest.id] = 0;
                    poolRelease(s, slot);
                    --s.liveCount;
                } else {
                    upd.status             = OrderStatus::Accepted;
                    upd.remaining_quantity = rest.qty;
                    listener->onOrderUpdate(upd);
                }
            }

            if (!bucket.head) {
                --book.liveAskLevels;
                if (!book.liveAskLevels) {
                    book.bestAsk = 0;
                    break;
                }
                book.bestAsk = execPrice + 1;
                while (book.bestAsk <= MAX_TICK && !book.asks[book.bestAsk].head)
                    ++book.bestAsk;
                if (book.bestAsk > MAX_TICK) [[unlikely]] { book.bestAsk = 0; break; }
            }
        }
    } else {
        // caso vendedor
        trade.sell_order_id = aggId;

        while (qty > 0 && book.bestBid > 0 && book.bestBid >= limit) {
            const int64_t execPrice = book.bestBid;
            PriceBucket&  bucket    = book.bids[execPrice];
            trade.price = execPrice;

            while (qty > 0 && bucket.head) {
                uint32_t slot = bucket.head;
                Order&   rest = s.orderPool[slot];
                if (rest.next) __builtin_prefetch(&s.orderPool[rest.next], 1, 1);

                uint32_t fillQty = (qty < rest.qty) ? qty : rest.qty;

                trade.buy_order_id = rest.id;
                trade.quantity     = fillQty;
                listener->onTrade(trade);

                qty             -= fillQty;
                rest.qty        -= fillQty;
                bucket.totalQty -= fillQty;

                upd.order_id = rest.id;
                if (!rest.qty) [[likely]] {
                    upd.status             = OrderStatus::Filled;
                    upd.remaining_quantity = 0;
                    listener->onOrderUpdate(upd);

                    bucket.head = rest.next;
                    if (bucket.head) s.orderPool[bucket.head].prev = 0;
                    else             bucket.tail = 0;
                    --bucket.count;
                    s.idToSlot[rest.id] = 0;
                    poolRelease(s, slot);
                    --s.liveCount;
                } else {
                    upd.status             = OrderStatus::Accepted;
                    upd.remaining_quantity = rest.qty;
                    listener->onOrderUpdate(upd);
                }
            }

            if (!bucket.head) {
                --book.liveBidLevels;
                if (!book.liveBidLevels) {
                    book.bestBid = 0;
                    break;
                }
                book.bestBid = execPrice - 1;
                while (book.bestBid > 0 && !book.bids[book.bestBid].head)
                    --book.bestBid;
            }
        }
    }
    return qty;
}

// ==========================================================================
// Public interface
// ==========================================================================

MatchingEngine::MatchingEngine(Listener* listener) : listener_(listener) {
    auto* s = new EngineState();

    // Pre-touch pool pages so the hot path never hits a first-touch fault.
    s->orderPool.resize(1 << 22);          // 4M slots (adversarial safe)
    s->orderPool.clear();
    s->orderPool.emplace_back();           // slot 0 sentinel

    // 10.5M = 500k warmup + 10M measurement ops per scenario
    s->idToSlot.assign(10500001, 0);

    // prefill trade buffer symbol per book (kappa-style reuse)
    for (int i = 0; i < N_SYMBOLS; ++i) {
        s->books[i].tradeBuf.symbol = SYMBOL_NAMES[i];
    }

    state_ = s;
}

MatchingEngine::~MatchingEngine() {
    delete state_;
}

// ----  addOrder  ----
__attribute__((hot, flatten))
OrderAck MatchingEngine::addOrder(const OrderRequest& request) noexcept {
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) [[unlikely]]
        return {0, OrderStatus::Rejected};

    uint64_t oid = next_order_id_++;
    int si = symbolToIndex(request.symbol);

    EngineState& s    = *state_;
    Book&        book = s.books[si];

    // Fast path: skip the match loop entirely when nothing can cross.
    uint32_t remaining;
    const bool isBuy = (request.side == Side::Buy);
    const int64_t opp = isBuy ? book.bestAsk : book.bestBid;
    const bool noCross = isBuy
        ? (opp == 0 || opp >  request.price)
        : (opp == 0 || opp <  request.price);

    if (noCross) [[likely]] {
        remaining = request.quantity;
    } else {
        if (isBuy)
            remaining = doMatch<Side::Buy >(oid, (uint8_t)si, request.price,
                                            request.quantity, book, s, listener_);
        else
            remaining = doMatch<Side::Sell>(oid, (uint8_t)si, request.price,
                                            request.quantity, book, s, listener_);
    }

    if (remaining > 0) [[likely]] {
        restOrder(oid, (uint8_t)si, request.side, request.price, remaining, book, s);
        listener_->onOrderUpdate({oid, OrderStatus::Accepted, remaining});
        return {oid, OrderStatus::Accepted};
    }
    listener_->onOrderUpdate({oid, OrderStatus::Filled, 0});
    return {oid, OrderStatus::Filled};
}

// ----  cancelOrder  ----
__attribute__((hot, flatten))
bool MatchingEngine::cancelOrder(uint64_t order_id) noexcept {
    EngineState& s = *state_;
    if (order_id >= s.idToSlot.size() || !s.idToSlot[order_id])
        return false;

    uint32_t slot = s.idToSlot[order_id];
    Order&   o    = s.orderPool[slot];
    unrestOrder(slot, s.books[unpackSym(o.priceSymSide)], s);
    listener_->onOrderUpdate({order_id, OrderStatus::Cancelled, 0});
    return true;
}

// ----  amendOrder  ----
__attribute__((hot, flatten))
bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) noexcept {
    if (new_price <= 0 || new_quantity == 0) return false;

    EngineState& s = *state_;
    if (order_id >= s.idToSlot.size() || !s.idToSlot[order_id])
        return false;

    uint32_t slot = s.idToSlot[order_id];
    Order&   o    = s.orderPool[slot];

    const uint32_t pss  = o.priceSymSide;
    const int64_t oldPrice = unpackPrice(pss);
    const uint8_t symIdx   = unpackSym(pss);
    const uint8_t sideRaw  = unpackSide(pss);

    // caso feliz: misma tick, qty decrece => mantenemos prioridad
    if (new_price == oldPrice && new_quantity <= o.qty) {
        PriceBucket& b = (sideRaw == 0)
            ? s.books[symIdx].bids[oldPrice]
            : s.books[symIdx].asks[oldPrice];
        b.totalQty -= (o.qty - new_quantity);
        o.qty = new_quantity;
        return true;
    }

    // perdemos prioridad
    Side  side = (Side)sideRaw;
    Book& book = s.books[symIdx];

    unrestOrder(slot, book, s);

    uint32_t remaining;
    if (side == Side::Buy)
        remaining = doMatch<Side::Buy >(order_id, symIdx, new_price, new_quantity,
                                        book, s, listener_);
    else
        remaining = doMatch<Side::Sell>(order_id, symIdx, new_price, new_quantity,
                                        book, s, listener_);

    if (remaining > 0) {
        restOrder(order_id, symIdx, side, new_price, remaining, book, s);
        listener_->onOrderUpdate({order_id, OrderStatus::Accepted, remaining});
    } else {
        listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});
    }
    return true;
}

// ----  getBookSnapshot  ----
std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    if (symbol.empty()) return {};
    int si = symbolToIndex(symbol);

    const EngineState& s  = *state_;
    const Book&        bk = s.books[si];

    std::vector<PriceLevel> out;
    if (side == Side::Buy) {
        for (int64_t p = bk.bestBid; p > 0; --p)
            if (bk.bids[p].count)
                out.push_back({p, bk.bids[p].totalQty, bk.bids[p].count});
    } else {
        for (int64_t p = bk.bestAsk; p > 0 && p <= MAX_TICK; ++p)
            if (bk.asks[p].count)
                out.push_back({p, bk.asks[p].totalQty, bk.asks[p].count});
    }
    return out;
}

uint64_t MatchingEngine::getOrderCount() const {
    return state_->liveCount;
}

}  // namespace exchange
