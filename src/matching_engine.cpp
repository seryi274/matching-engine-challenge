#include "exchange/matching_engine.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace exchange {

const char* const MatchingEngine::SYMBOL_NAMES[MatchingEngine::N_SYMBOLS] = {
    "AAPL", "GOOG", "MSFT", "AMZN", "TSLA"
};

// ==========================================================================
// Helper methods (defined as private statics so they can access nested types)
// ==========================================================================

__attribute__((always_inline)) inline
int MatchingEngine::symbolToIndex(const std::string& s) noexcept {
    switch (s[2]) {
        case 'P': return 0;   // aapl
        case 'O': return 1;   // goog
        case 'F': return 2;   // msft
        case 'Z': return 3;   // amzn
        default : return 4;   // tsla
    }
}

__attribute__((always_inline)) inline
uint8_t MatchingEngine::packSymSide(uint8_t sym, uint8_t side) noexcept {
    return (uint8_t)((sym << 1) | side);
}
__attribute__((always_inline)) inline
uint8_t MatchingEngine::unpackSym(uint8_t ss) noexcept { return (uint8_t)(ss >> 1); }
__attribute__((always_inline)) inline
uint8_t MatchingEngine::unpackSide(uint8_t ss) noexcept { return (uint8_t)(ss & 1); }

// --------------------------------------------------------------------------
// pool: O(1) alloc/free via free list
// --------------------------------------------------------------------------

__attribute__((always_inline)) inline
uint32_t MatchingEngine::poolAcquire(EngineState& s) noexcept {
    if (s.freeHead) {
        uint32_t slot = s.freeHead;
        s.freeHead = s.orderPool[slot].next;
        return slot;
    }
    s.orderPool.emplace_back();
    return (uint32_t)s.orderPool.size() - 1;
}

__attribute__((always_inline)) inline
void MatchingEngine::poolRelease(EngineState& s, uint32_t slot) noexcept {
    s.orderPool[slot].next = s.freeHead;
    s.freeHead = slot;
}

// --------------------------------------------------------------------------
// intrusive FIFO helpers
// --------------------------------------------------------------------------

__attribute__((always_inline)) inline
void MatchingEngine::bucketPushBack(PriceBucket& b, EngineState& s, uint32_t slot) noexcept {
    Order& o = s.orderPool[slot];
    o.next = 0;
    o.prev = b.tail;
    if (b.tail) s.orderPool[b.tail].next = slot;
    else        b.head = slot;
    b.tail = slot;
    ++b.count;
    b.totalQty += o.qty;
}

__attribute__((always_inline)) inline
void MatchingEngine::bucketUnlink(PriceBucket& b, EngineState& s, uint32_t slot) noexcept {
    Order& o = s.orderPool[slot];
    if (o.prev) s.orderPool[o.prev].next = o.next; else b.head = o.next;
    if (o.next) s.orderPool[o.next].prev = o.prev; else b.tail = o.prev;
    --b.count;
    b.totalQty -= o.qty;
}

// --------------------------------------------------------------------------
// resting-order lifecycle
// --------------------------------------------------------------------------

__attribute__((always_inline)) inline
void MatchingEngine::restOrder(uint64_t oid, uint8_t symIdx, Side side,
                               int64_t price, uint32_t qty,
                               Book& book, EngineState& s) noexcept
{
    uint32_t slot = poolAcquire(s);
    Order& o = s.orderPool[slot];
    o.id      = (uint32_t)oid;
    o.qty     = (uint8_t)qty;
    o.price   = (uint16_t)price;
    o.symSide = packSymSide(symIdx, (uint8_t)side);

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

__attribute__((always_inline)) inline
void MatchingEngine::unrestOrder(uint32_t slot, Book& book, EngineState& s) noexcept {
    Order&  o     = s.orderPool[slot];
    const int64_t price = (int64_t)o.price;
    const uint8_t side  = unpackSide(o.symSide);

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

// --------------------------------------------------------------------------
// matching loop (templated on side)
// --------------------------------------------------------------------------

template <Side SIDE>
__attribute__((always_inline)) inline
uint32_t MatchingEngine::doMatch(uint64_t aggId, uint8_t /*symIdx*/, int64_t limit, uint32_t qty,
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
                if (!book.liveAskLevels) { book.bestAsk = 0; break; }
                book.bestAsk = execPrice + 1;
                while (book.bestAsk <= MAX_TICK && !book.asks[book.bestAsk].head)
                    ++book.bestAsk;
                if (book.bestAsk > MAX_TICK) [[unlikely]] { book.bestAsk = 0; break; }
            }
        }
    } else {
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
                if (!book.liveBidLevels) { book.bestBid = 0; break; }
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
    // state_ is inlined as a direct member.
    state_.orderPool.resize(1 << 22);
    state_.orderPool.clear();
    state_.orderPool.emplace_back();      // slot 0 sentinel

    state_.idToSlot.assign(10500001, 0);

    for (int i = 0; i < N_SYMBOLS; ++i) {
        state_.books[i].tradeBuf.symbol = SYMBOL_NAMES[i];
    }
}

MatchingEngine::~MatchingEngine() = default;

__attribute__((hot, flatten))
OrderAck MatchingEngine::addOrder(const OrderRequest& request) noexcept {
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) [[unlikely]]
        return {0, OrderStatus::Rejected};

    uint64_t oid = next_order_id_++;
    int si = symbolToIndex(request.symbol);

    Book& book = state_.books[si];

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
                                            request.quantity, book, state_, listener_);
        else
            remaining = doMatch<Side::Sell>(oid, (uint8_t)si, request.price,
                                            request.quantity, book, state_, listener_);
    }

    if (remaining > 0) [[likely]] {
        restOrder(oid, (uint8_t)si, request.side, request.price, remaining, book, state_);
        listener_->onOrderUpdate({oid, OrderStatus::Accepted, remaining});
        return {oid, OrderStatus::Accepted};
    }
    listener_->onOrderUpdate({oid, OrderStatus::Filled, 0});
    return {oid, OrderStatus::Filled};
}

__attribute__((hot, flatten))
bool MatchingEngine::cancelOrder(uint64_t order_id) noexcept {
    if (order_id >= state_.idToSlot.size() || !state_.idToSlot[order_id])
        return false;

    uint32_t slot = state_.idToSlot[order_id];
    Order&   o    = state_.orderPool[slot];
    unrestOrder(slot, state_.books[unpackSym(o.symSide)], state_);
    listener_->onOrderUpdate({order_id, OrderStatus::Cancelled, 0});
    return true;
}

__attribute__((hot, flatten))
bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) noexcept {
    if (new_price <= 0 || new_quantity == 0) return false;

    if (order_id >= state_.idToSlot.size() || !state_.idToSlot[order_id])
        return false;

    uint32_t slot = state_.idToSlot[order_id];
    Order&   o    = state_.orderPool[slot];

    const int64_t oldPrice = (int64_t)o.price;
    const uint8_t symIdx   = unpackSym(o.symSide);
    const uint8_t sideRaw  = unpackSide(o.symSide);

    if (new_price == oldPrice && new_quantity <= (uint32_t)o.qty) {
        PriceBucket& b = (sideRaw == 0)
            ? state_.books[symIdx].bids[oldPrice]
            : state_.books[symIdx].asks[oldPrice];
        b.totalQty -= ((uint32_t)o.qty - new_quantity);
        o.qty = (uint8_t)new_quantity;
        return true;
    }

    Side  side = (Side)sideRaw;
    Book& book = state_.books[symIdx];

    unrestOrder(slot, book, state_);

    uint32_t remaining;
    if (side == Side::Buy)
        remaining = doMatch<Side::Buy >(order_id, symIdx, new_price, new_quantity,
                                        book, state_, listener_);
    else
        remaining = doMatch<Side::Sell>(order_id, symIdx, new_price, new_quantity,
                                        book, state_, listener_);

    if (remaining > 0) {
        restOrder(order_id, symIdx, side, new_price, remaining, book, state_);
        listener_->onOrderUpdate({order_id, OrderStatus::Accepted, remaining});
    } else {
        listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});
    }
    return true;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    if (symbol.empty()) return {};
    int si = symbolToIndex(symbol);

    const Book& bk = state_.books[si];

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
    return state_.liveCount;
}

}  // namespace exchange
