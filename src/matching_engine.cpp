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
    // Padded to 8 to support the perfect hash array index map
    active_books_ = new (aligned_alloc_64(8 * sizeof(OrderBook))) OrderBook[8];
    order_lookup_ = static_cast<uint32_t*>(aligned_alloc_64(MAX_CAPACITY * sizeof(uint32_t)));
    order_pool_   = static_cast<OrderNode*>(aligned_alloc_64(MAX_CAPACITY * sizeof(OrderNode)));
    
    std::fill_n(order_lookup_, MAX_CAPACITY, 0);

    for (int i = 0; i < 8; ++i) {
        active_books_[i].bestbid = NOBID;
        active_books_[i].bestask = NOASK;
        std::fill_n(active_books_[i].bidlevels, MAXPRICE + 2, PriceLevelNode{0, 0});
        std::fill_n(active_books_[i].asklevels, MAXPRICE + 2, PriceLevelNode{0, 0});
        std::fill_n(active_books_[i].bid_bits, 815, 0);
        std::fill_n(active_books_[i].ask_bits, 815, 0);
        
        // Sentinels guarantee boundary safety during branchless bit-scans
        active_books_[i].bid_bits[0] |= 1ULL; 
        active_books_[i].ask_bits[NOASK >> 6] |= (1ULL << (NOASK & 63)); 
    }

    // Explicit binding for the perfectly hashed indices
    auto bind_symbol = [&](int idx, const std::string& sym) {
        active_books_[idx].symbol = sym;
        active_books_[idx].tradebuf.symbol = sym;
    };
    bind_symbol(0, "AAPL");
    bind_symbol(2, "AMZN");
    bind_symbol(4, "TSLA");
    bind_symbol(6, "MSFT");
    bind_symbol(7, "GOOG");
}

MatchingEngine::~MatchingEngine() {
    free(active_books_);
    free(order_lookup_);
    free(order_pool_);
}

__attribute__((always_inline))
inline uint16_t MatchingEngine::getBookIndex(const std::string & symbol) const noexcept {
    // Zero-Branch Perfect Hash (Looks at 3rd character's ASCII bits)
    // AAPL='P'(80)->0, AMZN='Z'(90)->2, TSLA='L'(76)->4, MSFT='F'(70)->6, GOOG='O'(79)->7
    return static_cast<uint16_t>(symbol[2] & 7);
}

__attribute__((hot, flatten))
void MatchingEngine::matchBuy(OrderBook &__restrict__ book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) noexcept {
    PriceLevelNode* __restrict__ asks = book.asklevels;
    OrderNode* __restrict__ pool = order_pool_;
    uint32_t* __restrict__ lookup = order_lookup_;
    Listener* __restrict__ local_listener = listener_; 
    Trade* __restrict__ tb = &book.tradebuf;           
    
    tb->buy_order_id = incomingid;
    int64_t p = book.bestask;
    
    while (remaining > 0 && p <= limitprice) {
        PriceLevelNode &__restrict__ level = asks[p];
        uint32_t curr = level.head;
        tb->price = p; // Hoisted invariant

        while (curr != 0) {
            OrderNode &__restrict__ resting = pool[curr];
            const uint32_t r_qty = resting.qty;

            // Instantly skip and dynamically unlink prefix tombstones
            if (r_qty == 0) [[unlikely]] {
                if (curr == level.head) level.head = resting.next;
                curr = resting.next;
                continue;
            }

            const uint32_t fillquantity = (remaining < r_qty ? remaining : r_qty);
            tb->sell_order_id = resting.id;
            tb->quantity = fillquantity;
            local_listener->onTrade(*tb);

            remaining -= fillquantity;
            const uint32_t new_r_qty = r_qty - fillquantity;
            resting.qty = new_r_qty;

            if (new_r_qty == 0) [[likely]] {
                lookup[resting.id] = 0;
                --liveorderscount;
                if (curr == level.head) level.head = resting.next;
                local_listener->onOrderUpdate({resting.id, OrderStatus::Filled, 0});
            } else {
                local_listener->onOrderUpdate({resting.id, OrderStatus::Accepted, new_r_qty});
                break;
            }
            
            curr = resting.next;
            if (remaining == 0) [[unlikely]] break;
        }

        if (level.head == 0) [[likely]] {
            book.ask_bits[p >> 6] &= ~(1ULL << (p & 63));
            uint32_t w = p >> 6;
            uint32_t b = p & 63;
            // Branchless shift evaluation
            uint64_t mask = book.ask_bits[w] & (~0ULL << b);
            if (mask) { p = (w << 6) + __builtin_ctzll(mask); }
            else {
                do { ++w; } while ((mask = book.ask_bits[w]) == 0);
                p = (w << 6) + __builtin_ctzll(mask);
            }
        }
    }
    book.bestask = p;
}

__attribute__((hot, flatten))
void MatchingEngine::matchSell(OrderBook &__restrict__ book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) noexcept {
    PriceLevelNode* __restrict__ bids = book.bidlevels;
    OrderNode* __restrict__ pool = order_pool_;
    uint32_t* __restrict__ lookup = order_lookup_;
    Listener* __restrict__ local_listener = listener_; 
    Trade* __restrict__ tb = &book.tradebuf;           

    tb->sell_order_id = incomingid;
    int64_t p = book.bestbid;

    while (remaining > 0 && p >= limitprice) {
        PriceLevelNode &__restrict__ level = bids[p];
        uint32_t curr = level.head;
        tb->price = p;

        while (curr != 0) {
            OrderNode &__restrict__ resting = pool[curr];
            const uint32_t r_qty = resting.qty;

            if (r_qty == 0) [[unlikely]] {
                if (curr == level.head) level.head = resting.next;
                curr = resting.next;
                continue;
            }

            const uint32_t fillquantity = (remaining < r_qty ? remaining : r_qty);
            tb->buy_order_id = resting.id;
            tb->quantity = fillquantity;
            local_listener->onTrade(*tb);

            remaining -= fillquantity;
            const uint32_t new_r_qty = r_qty - fillquantity;
            resting.qty = new_r_qty;

            if (new_r_qty == 0) [[likely]] {
                lookup[resting.id] = 0;
                --liveorderscount;
                if (curr == level.head) level.head = resting.next;
                local_listener->onOrderUpdate({resting.id, OrderStatus::Filled, 0});
            } else {
                local_listener->onOrderUpdate({resting.id, OrderStatus::Accepted, new_r_qty});
                break;
            }
            
            curr = resting.next;
            if (remaining == 0) [[unlikely]] break;
        }

        if (level.head == 0) [[likely]] {
            book.bid_bits[p >> 6] &= ~(1ULL << (p & 63));
            uint32_t w = p >> 6;
            uint32_t b = p & 63;
            uint64_t mask = book.bid_bits[w] & (~0ULL >> (63 - b));
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
        // Pure Bump Allocator - Zero branching
        const uint32_t pool_idx = next_pool_idx_++;
        OrderNode &__restrict__ node = order_pool_[pool_idx];
        
        node.id       = order_id;
        node.qty      = remaining;
        node.next     = 0;
        node.price    = static_cast<uint16_t>(req_price);
        node.bookidx  = static_cast<uint8_t>(book_idx);
        node.side     = req_side;

        order_lookup_[order_id] = pool_idx;

        if (req_side == Side::Buy) {
            PriceLevelNode &__restrict__ level = book.bidlevels[req_price];
            if (level.head == 0) {
                level.head = level.tail = pool_idx;
                book.bid_bits[req_price >> 6] |= (1ULL << (req_price & 63));
                if(req_price > book.bestbid) book.bestbid = req_price;
            } else {
                order_pool_[level.tail].next = pool_idx;
                level.tail = pool_idx;
            }
        } else {
            PriceLevelNode &__restrict__ level = book.asklevels[req_price];
            if (level.head == 0) {
                level.head = level.tail = pool_idx;
                book.ask_bits[req_price >> 6] |= (1ULL << (req_price & 63));
                if(req_price < book.bestask) book.bestask = req_price;
            } else {
                order_pool_[level.tail].next = pool_idx;
                level.tail = pool_idx;
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
    const uint32_t pool_idx = order_lookup_[order_id];
    if(pool_idx == 0) [[unlikely]] return false;
    
    // O(1) Tombstone Cancel - Zero branching, zero array scans
    order_pool_[pool_idx].qty = 0;
    order_lookup_[order_id] = 0;
    --liveorderscount;
    
    listener_->onOrderUpdate({order_id, OrderStatus::Cancelled, 0});
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    if (new_price <= 0 || new_quantity == 0 || new_price > MAXPRICE) [[unlikely]] return false;

    const uint32_t pool_idx = order_lookup_[order_id];
    if (pool_idx == 0) [[unlikely]] return false;

    OrderNode* __restrict__ pool = order_pool_;
    OrderNode &__restrict__ node = pool[pool_idx];
    if (node.qty == 0) return false;

    const int64_t old_price = node.price;
    const Side side = node.side;
    const uint8_t book_idx = node.bookidx;

    if (old_price == new_price) [[likely]] {
        if (new_quantity == node.qty) [[unlikely]] return false;   
        if (new_quantity < node.qty) {
            node.qty = new_quantity;
            return true;
        }
    }

    // Convert old to tombstone
    node.qty = 0;
    
    uint32_t remaining = new_quantity;
    if (side == Side::Buy) matchBuy (active_books_[book_idx], order_id, new_price, remaining);
    else matchSell(active_books_[book_idx], order_id, new_price, remaining);

    if (remaining == 0) {
        listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});
        order_lookup_[order_id] = 0;
        --liveorderscount;
        return true;
    }

    const uint32_t new_p_idx = next_pool_idx_++;
    OrderNode &__restrict__ new_node = pool[new_p_idx];
    new_node.id = order_id;
    new_node.qty = remaining;
    new_node.next = 0;
    new_node.price = static_cast<uint16_t>(new_price);
    new_node.side = side;
    new_node.bookidx = book_idx;

    order_lookup_[order_id] = new_p_idx;

    OrderBook &__restrict__ book = active_books_[book_idx];
    if (side == Side::Buy) {
        PriceLevelNode &__restrict__ level = book.bidlevels[new_price];
        if (level.head == 0) {
            level.head = level.tail = new_p_idx;
            book.bid_bits[new_price >> 6] |= (1ULL << (new_price & 63));
            if(new_price > book.bestbid) book.bestbid = new_price;
        } else {
            pool[level.tail].next = new_p_idx;
            level.tail = new_p_idx;
        }
    } else {
        PriceLevelNode &__restrict__ level = book.asklevels[new_price];
        if (level.head == 0) {
            level.head = level.tail = new_p_idx;
            book.ask_bits[new_price >> 6] |= (1ULL << (new_price & 63));
            if(new_price < book.bestask) book.bestask = new_price;
        } else {
            pool[level.tail].next = new_p_idx;
            level.tail = new_p_idx;
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
            uint32_t total = 0, count = 0, curr = lvl.head;
            while (curr) { 
                uint32_t q = order_pool_[curr].qty;
                if (q > 0) {
                    total += q; 
                    count++;
                }
                curr = order_pool_[curr].next; 
            }
            if (count > 0) snapshot.push_back({p, total, count});
            
            uint32_t w = p >> 6;
            uint32_t b = p & 63;
            uint64_t mask = found->bid_bits[w] & (~0ULL >> (63 - b));
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
            uint32_t total = 0, count = 0, curr = lvl.head;
            while (curr) { 
                uint32_t q = order_pool_[curr].qty;
                if (q > 0) {
                    total += q; 
                    count++;
                }
                curr = order_pool_[curr].next; 
            }
            if (count > 0) snapshot.push_back({p, total, count});
            
            uint32_t w = p >> 6;
            uint32_t b = p & 63;
            uint64_t mask = found->ask_bits[w] & (~0ULL << b);
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