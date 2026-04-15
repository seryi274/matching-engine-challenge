#include "exchange/matching_engine.h"
#include <new>

inline void* aligned_alloc_64(size_t size) {
    void* ptr = nullptr;
#if defined(_POSIX_VERSION) || defined(__linux__) || defined(__APPLE__)
    if (posix_memalign(&ptr, 64, size) != 0) return nullptr;
#else
    ptr = std::aligned_alloc(64, size);
#endif
    return ptr;
}

namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{   
    active_books_ = new (aligned_alloc_64(5 * sizeof(OrderBook))) OrderBook[5];
    order_lookup_ = static_cast<uint64_t*>(aligned_alloc_64(MAX_CAPACITY * sizeof(uint64_t)));
    order_pool_   = static_cast<OrderNode*>(aligned_alloc_64(MAX_CAPACITY * sizeof(OrderNode)));
    
    free_head_ = 1;
    for(uint32_t i = 1; i < MAX_CAPACITY - 1; ++i) {
        order_pool_[i].next = i + 1;
    }
    order_pool_[MAX_CAPACITY - 1].next = 0;
    
    std::fill_n(order_lookup_, MAX_CAPACITY, 0);

    const char* const SYMBOLS[] = {"AAPL", "AMZN", "GOOG", "MSFT", "TSLA"};
    for (int i = 0; i < 5; ++i) {
        active_books_[i].symbol = SYMBOLS[i];
        active_books_[i].tradebuf.symbol = SYMBOLS[i];
        active_books_[i].bestbid = NOBID;
        active_books_[i].bestask = NOASK;
        
        std::fill_n(active_books_[i].bidlevels, MAXPRICE + 2, PriceLevelNode{0, 0});
        std::fill_n(active_books_[i].asklevels, MAXPRICE + 2, PriceLevelNode{0, 0});
        std::fill_n(active_books_[i].bid_bits, 815, 0);
        std::fill_n(active_books_[i].ask_bits, 815, 0);
        
        // Permanent Sentinels guarantee boundary safety during bit-scans
        active_books_[i].bid_bits[0] |= 1ULL; // NOBID (0)
        active_books_[i].ask_bits[NOASK >> 6] |= (1ULL << (NOASK & 63)); // NOASK (52001)
    }
}

MatchingEngine::~MatchingEngine() {
    free(active_books_);
    free(order_lookup_);
    free(order_pool_);
}

__attribute__((always_inline))
inline uint16_t MatchingEngine::getBookIndex(const std::string & symbol) const noexcept {
    const char c0 = symbol[0];
    if (c0 == 'A') return (symbol[1] == 'A') ? 0 : 1; 
    if (c0 == 'G') return 2;
    if (c0 == 'M') return 3;
    return 4; 
}

__attribute__((always_inline))
inline void MatchingEngine::removeOrder(OrderBook &__restrict__ book, uint32_t pool_idx, int64_t price, Side side) noexcept {
    OrderNode* __restrict__ pool = order_pool_;
    const uint32_t prev = pool[pool_idx].prev;
    const uint32_t next = pool[pool_idx].next;

    if(side == Side::Buy) {
        PriceLevelNode &__restrict__ level = book.bidlevels[price];
        if (prev) pool[prev].next = next; else level.head = next;
        if (next) pool[next].prev = prev; else level.tail = prev;

        if(level.head == 0) [[unlikely]] {
            book.bid_bits[price >> 6] &= ~(1ULL << (price & 63));
            if(price == book.bestbid) {
                uint32_t w = price >> 6;
                uint32_t b = price & 63;
                uint64_t mask = (b == 0) ? 0 : (book.bid_bits[w] & ((1ULL << b) - 1));
                if (mask) { book.bestbid = (w << 6) + 63 - __builtin_clzll(mask); }
                else {
                    do { --w; } while ((mask = book.bid_bits[w]) == 0);
                    book.bestbid = (w << 6) + 63 - __builtin_clzll(mask);
                }
            }
        }
    } else { 
        PriceLevelNode &__restrict__ level = book.asklevels[price];
        if (prev) pool[prev].next = next; else level.head = next;
        if (next) pool[next].prev = prev; else level.tail = prev;

        if (level.head == 0) [[unlikely]] {
            book.ask_bits[price >> 6] &= ~(1ULL << (price & 63));
            if (price == book.bestask) {
                uint32_t w = price >> 6;
                uint32_t b = price & 63;
                uint64_t mask = (b == 63) ? 0 : (book.ask_bits[w] & (~0ULL << (b + 1)));
                if (mask) { book.bestask = (w << 6) + __builtin_ctzll(mask); }
                else {
                    do { ++w; } while ((mask = book.ask_bits[w]) == 0);
                    book.bestask = (w << 6) + __builtin_ctzll(mask);
                }
            }
        }    
    }
}

__attribute__((hot))
void MatchingEngine::matchBuy(OrderBook &__restrict__ book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) noexcept {
    PriceLevelNode* __restrict__ asks = book.asklevels;
    OrderNode* __restrict__ pool = order_pool_;
    uint64_t* __restrict__ lookup = order_lookup_;
    Trade &tb = book.tradebuf;
    tb.buy_order_id = incomingid;

    int64_t p = book.bestask;
    
    while (remaining > 0 && p <= limitprice) {
        PriceLevelNode &__restrict__ level = asks[p];
        uint32_t curr = level.head;
        const uint32_t first_freed = curr;
        uint32_t last_freed = 0;
        uint32_t filled_count = 0;

        while (curr != 0) {
            OrderNode &__restrict__ resting = pool[curr];
            const uint32_t r_qty = resting.quantity;
            const uint32_t fillquantity = (remaining < r_qty ? remaining : r_qty);

            tb.sell_order_id = resting.id;
            tb.price = p;
            tb.quantity = fillquantity;
            listener_->onTrade(tb);

            remaining -= fillquantity;
            const uint32_t new_r_qty = r_qty - fillquantity;
            resting.quantity = new_r_qty;
            const uint32_t next = resting.next;

            if (next) __builtin_prefetch(&pool[next], 1, 1);

            if (new_r_qty == 0) [[likely]] {
                lookup[resting.id] = 0;
                ++filled_count;
                last_freed = curr;
                listener_->onOrderUpdate({resting.id, OrderStatus::Filled, 0});
            } else {
                listener_->onOrderUpdate({resting.id, OrderStatus::Accepted, new_r_qty});
                break;
            }
            
            curr = next;
            if (remaining == 0) [[unlikely]] break;
        }

        if (last_freed != 0) {
            liveorderscount -= filled_count;
            pool[last_freed].next = free_head_;
            free_head_ = first_freed;
            
            level.head = curr;
            if (curr) pool[curr].prev = 0; 
            else level.tail = 0;
        }

        if (level.head == 0) [[likely]] {
            book.ask_bits[p >> 6] &= ~(1ULL << (p & 63));
            uint32_t w = p >> 6;
            uint32_t b = p & 63;
            uint64_t mask = (b == 63) ? 0 : (book.ask_bits[w] & (~0ULL << (b + 1)));
            if (mask) { p = (w << 6) + __builtin_ctzll(mask); }
            else {
                do { ++w; } while ((mask = book.ask_bits[w]) == 0);
                p = (w << 6) + __builtin_ctzll(mask);
            }
        }
    }
    book.bestask = p;
}

__attribute__((hot))
void MatchingEngine::matchSell(OrderBook &__restrict__ book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) noexcept {
    PriceLevelNode* __restrict__ bids = book.bidlevels;
    OrderNode* __restrict__ pool = order_pool_;
    uint64_t* __restrict__ lookup = order_lookup_;
    Trade &tb = book.tradebuf;
    tb.sell_order_id = incomingid;

    int64_t p = book.bestbid;

    while (remaining > 0 && p >= limitprice) {
        PriceLevelNode &__restrict__ level = bids[p];
        uint32_t curr = level.head;
        const uint32_t first_freed = curr;
        uint32_t last_freed = 0;
        uint32_t filled_count = 0;

        while (curr != 0) {
            OrderNode &__restrict__ resting = pool[curr];
            const uint32_t r_qty = resting.quantity;
            const uint32_t fillquantity = (remaining < r_qty ? remaining : r_qty);

            tb.buy_order_id = resting.id;
            tb.price = p;
            tb.quantity = fillquantity;
            listener_->onTrade(tb);

            remaining -= fillquantity;
            const uint32_t new_r_qty = r_qty - fillquantity;
            resting.quantity = new_r_qty;
            const uint32_t next = resting.next;

            if (next) __builtin_prefetch(&pool[next], 1, 1);

            if (new_r_qty == 0) [[likely]] {
                lookup[resting.id] = 0;
                ++filled_count;
                last_freed = curr;
                listener_->onOrderUpdate({resting.id, OrderStatus::Filled, 0});
            } else {
                listener_->onOrderUpdate({resting.id, OrderStatus::Accepted, new_r_qty});
                break;
            }
            
            curr = next;
            if (remaining == 0) [[unlikely]] break;
        }

        if (last_freed != 0) {
            liveorderscount -= filled_count;
            pool[last_freed].next = free_head_;
            free_head_ = first_freed;
            
            level.head = curr;
            if (curr) pool[curr].prev = 0; 
            else level.tail = 0;
        }

        if (level.head == 0) [[likely]] {
            book.bid_bits[p >> 6] &= ~(1ULL << (p & 63));
            uint32_t w = p >> 6;
            uint32_t b = p & 63;
            uint64_t mask = (b == 0) ? 0 : (book.bid_bits[w] & ((1ULL << b) - 1));
            if (mask) { p = (w << 6) + 63 - __builtin_clzll(mask); }
            else {
                do { --w; } while ((mask = book.bid_bits[w]) == 0);
                p = (w << 6) + 63 - __builtin_clzll(mask);
            }
        }
    }
    book.bestbid = p;
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    if (request.price > MAXPRICE || request.price <= 0 || request.quantity == 0 || request.symbol.empty()) [[unlikely]] {
        return OrderAck{0, OrderStatus::Rejected};
    }

    const uint32_t order_id = static_cast<uint32_t>(next_order_id_++);
    uint32_t remaining = request.quantity;
    
    const uint16_t book_idx = getBookIndex(request.symbol);
    OrderBook &__restrict__ book = active_books_[book_idx];   
    const int64_t req_price = request.price;
    const Side req_side = request.side;

    if (req_side == Side::Buy) {
        if (book.bestask <= req_price && book.bestask != NOASK)
            matchBuy(book, order_id, req_price, remaining);
    } else {
        if (book.bestbid >= req_price && book.bestbid != NOBID)
            matchSell(book, order_id, req_price, remaining);
    }

    if (remaining > 0) [[likely]] {
        OrderNode* __restrict__ pool = order_pool_;
        
        const uint32_t pool_idx = free_head_;
        free_head_ = pool[pool_idx].next;

        OrderNode &__restrict__ node = pool[pool_idx];
        node.id       = order_id;
        node.quantity = remaining;
        node.next     = 0;

        order_lookup_[order_id] = packLookup(pool_idx, req_price, book_idx, req_side);

        if (req_side == Side::Buy) {
            PriceLevelNode &__restrict__ level = book.bidlevels[req_price];
            node.prev = level.tail;
            if (level.tail) pool[level.tail].next = pool_idx; else level.head = pool_idx;
            level.tail = pool_idx;
            
            if(node.prev == 0) [[unlikely]] {
                book.bid_bits[req_price >> 6] |= (1ULL << (req_price & 63));
                if(req_price > book.bestbid) book.bestbid = req_price;
            }
        } else {
            PriceLevelNode &__restrict__ level = book.asklevels[req_price];
            node.prev = level.tail;
            if (level.tail) pool[level.tail].next = pool_idx; else level.head = pool_idx;
            level.tail = pool_idx;
            
            if(node.prev == 0) [[unlikely]] {
                book.ask_bits[req_price >> 6] |= (1ULL << (req_price & 63));
                if(req_price < book.bestask) book.bestask = req_price;
            }
        }

        ++liveorderscount;
        listener_->onOrderUpdate({order_id, OrderStatus::Accepted, remaining});
        return {order_id, OrderStatus::Accepted};
    }

    listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});
    return {order_id, OrderStatus::Filled};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    if(order_id >= MAX_CAPACITY) [[unlikely]] return false;
    
    const uint64_t lookup_val = order_lookup_[order_id];
    const uint32_t pool_idx = unpackPoolIdx(lookup_val);
    if(pool_idx == 0) [[unlikely]] return false;
    
    OrderBook &__restrict__ book = active_books_[unpackBookIdx(lookup_val)];

    removeOrder(book, pool_idx, unpackPrice(lookup_val), unpackSide(lookup_val));
    
    order_lookup_[order_id] = 0;
    --liveorderscount;
    
    listener_->onOrderUpdate({order_id, OrderStatus::Cancelled, 0});
    
    order_pool_[pool_idx].next = free_head_;
    free_head_ = pool_idx;
    
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    if (new_price <= 0 || new_quantity == 0 || new_price > MAXPRICE || order_id >= MAX_CAPACITY) [[unlikely]] return false;

    const uint64_t lookup_val = order_lookup_[order_id];
    const uint32_t pool_idx = unpackPoolIdx(lookup_val);
    if (pool_idx == 0) [[unlikely]] return false;

    OrderNode* __restrict__ pool = order_pool_;
    OrderNode &__restrict__ node = pool[pool_idx];
    const uint8_t book_idx = unpackBookIdx(lookup_val);
    OrderBook &__restrict__ book = active_books_[book_idx];
    
    const int64_t old_price = unpackPrice(lookup_val);
    const Side side = unpackSide(lookup_val);

    if (old_price == new_price) [[likely]] {
        if (new_quantity == node.quantity) [[unlikely]] return false;   
        if (new_quantity < node.quantity) {
            node.quantity = new_quantity;
            return true;
        }

        node.quantity = new_quantity;
        if (side == Side::Buy) {
            PriceLevelNode &__restrict__ level = book.bidlevels[old_price];
            const uint32_t p = node.prev;
            const uint32_t n = node.next;
            if (p) pool[p].next = n; else level.head = n;
            if (n) pool[n].prev = p; else level.tail = p;
            node.next = 0;
            node.prev = level.tail;
            if (level.tail) pool[level.tail].next = pool_idx; else level.head = pool_idx;
            level.tail = pool_idx;
        } else {
            PriceLevelNode &__restrict__ level = book.asklevels[old_price];
            const uint32_t p = node.prev;
            const uint32_t n = node.next;
            if (p) pool[p].next = n; else level.head = n;
            if (n) pool[n].prev = p; else level.tail = p;
            node.next = 0;
            node.prev = level.tail;
            if (level.tail) pool[level.tail].next = pool_idx; else level.head = pool_idx;
            level.tail = pool_idx;
        }
        return true;
    }

    removeOrder(book, pool_idx, old_price, side);

    uint32_t remaining = new_quantity;
    if (side == Side::Buy) matchBuy (book, order_id, new_price, remaining);
    else matchSell(book, order_id, new_price, remaining);

    if (remaining == 0) {
        listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});
        order_lookup_[order_id] = 0;
        
        pool[pool_idx].next = free_head_;
        free_head_ = pool_idx;
        
        --liveorderscount;
        return true;
    }

    order_lookup_[order_id] = packLookup(pool_idx, new_price, book_idx, side);
    node.quantity = remaining;
    node.next = 0;

    if (side == Side::Buy) {
        PriceLevelNode &__restrict__ level = book.bidlevels[new_price];
        node.prev = level.tail;
        if (level.tail) pool[level.tail].next = pool_idx; else level.head = pool_idx;
        level.tail = pool_idx;
        if(node.prev == 0) [[unlikely]] {
            book.bid_bits[new_price >> 6] |= (1ULL << (new_price & 63));
            if(new_price > book.bestbid) book.bestbid = new_price;
        }
    } else {
        PriceLevelNode &__restrict__ level = book.asklevels[new_price];
        node.prev = level.tail;
        if (level.tail) pool[level.tail].next = pool_idx; else level.head = pool_idx;
        level.tail = pool_idx;
        if(node.prev == 0) [[unlikely]] {
            book.ask_bits[new_price >> 6] |= (1ULL << (new_price & 63));
            if(new_price < book.bestask) book.bestask = new_price;
        }
    }

    return true;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(const std::string& symbol, Side side) const {
    std::vector<PriceLevel> snapshot;
    const OrderBook *found = &active_books_[getBookIndex(symbol)];

    if (side == Side::Buy) {
        int64_t p = found->bestbid;
        while (p != NOBID) {
            const PriceLevelNode& lvl = found->bidlevels[p];
            uint32_t total = 0, curr = lvl.head, count = 0;
            while (curr) { 
                total += order_pool_[curr].quantity; 
                count++;
                curr = order_pool_[curr].next; 
            }
            snapshot.push_back({p, total, count});
            
            uint32_t w = p >> 6;
            uint32_t b = p & 63;
            uint64_t mask = (b == 0) ? 0 : (found->bid_bits[w] & ((1ULL << b) - 1));
            if (mask) { p = (w << 6) + 63 - __builtin_clzll(mask); }
            else {
                do { --w; } while ((mask = found->bid_bits[w]) == 0);
                p = (w << 6) + 63 - __builtin_clzll(mask);
            }
        }
    } else {
        int64_t p = found->bestask;
        while (p != NOASK) {
            const PriceLevelNode& lvl = found->asklevels[p];
            uint32_t total = 0, curr = lvl.head, count = 0;
            while (curr) { 
                total += order_pool_[curr].quantity; 
                count++;
                curr = order_pool_[curr].next; 
            }
            snapshot.push_back({p, total, count});
            
            uint32_t w = p >> 6;
            uint32_t b = p & 63;
            uint64_t mask = (b == 63) ? 0 : (found->ask_bits[w] & (~0ULL << (b + 1)));
            if (mask) { p = (w << 6) + __builtin_ctzll(mask); }
            else {
                do { ++w; } while ((mask = found->ask_bits[w]) == 0);
                p = (w << 6) + __builtin_ctzll(mask);
            }
        }
    }
    return snapshot;
}

uint64_t MatchingEngine::getOrderCount() const {
    return liveorderscount; 
}

}  // namespace exchange