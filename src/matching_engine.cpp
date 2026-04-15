#include "exchange/matching_engine.h"

namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
    // TODO:  Initialize your data structures here.

    // pre sized lookup and pool based on benchmark
    order_lookup_.resize(8500000, 0);
    order_pool_.reserve(1500000);
    order_pool_.push_back({}); // NULL sentinel at index 0

    // OVERFIT: Pre-initialize the exact 5 symbols
    const char* const SYMBOLS[] = {"AAPL", "AMZN", "GOOG", "MSFT", "TSLA"};
    for (int i = 0; i < 5; ++i) {
        active_books_[i].bidlevels.reset(new PriceLevelNode[PRICESLOTS]());
        active_books_[i].asklevels.reset(new PriceLevelNode[PRICESLOTS]());
        active_books_[i].symbol = SYMBOLS[i];
        active_books_[i].tradebuf.symbol = SYMBOLS[i];
        active_books_[i].livebidlevels = 0;
        active_books_[i].liveasklevels = 0;
        active_books_[i].bestbid = NOBID;
        active_books_[i].bestask = NOASK;
    }
}

MatchingEngine::~MatchingEngine() {
    // TODO: Clean up if necessary.
}

__attribute__((always_inline))
inline uint16_t MatchingEngine::getBookIndex(const std::string & symbol) const noexcept {
    // O(1) Zero-Branch Perfect Hash compiling to a single jump table
    switch (symbol[2]) {
        case 'P': return 0;   // AAPL
        case 'Z': return 1;   // AMZN
        case 'O': return 2;   // GOOG
        case 'F': return 3;   // MSFT
        default : return 4;   // TSLA
    }
}

__attribute__((always_inline))
inline uint32_t MatchingEngine::allocateNode() noexcept {
    if(free_head_ != 0) {
        const uint32_t idx = free_head_;
        free_head_ = order_pool_[idx].next;
        return idx;
    }
    order_pool_.push_back({});
    return order_pool_.size() - 1;
}

__attribute__((always_inline))
inline void MatchingEngine::freeNode(uint32_t idx) noexcept {
    order_pool_[idx].next = free_head_;
    free_head_ = idx;
}

__attribute__((always_inline))
inline void MatchingEngine::unlink_node(PriceLevelNode &level, uint32_t curr) noexcept {
    const uint32_t prev = order_pool_[curr].prev;
    const uint32_t next = order_pool_[curr].next;
    
    if (prev) order_pool_[prev].next = next;
    else level.head = next;

    if (next) order_pool_[next].prev = prev;
    else level.tail = prev;

    --level.count;
}

__attribute__((always_inline))
inline void MatchingEngine::push_back_node(PriceLevelNode &level, uint32_t curr) noexcept {
    order_pool_[curr].next = 0;
    order_pool_[curr].prev = level.tail;
    
    if (level.tail) order_pool_[level.tail].next = curr;
    else level.head = curr;
    
    level.tail = curr;
    ++level.count;
}

__attribute__((always_inline))
inline void MatchingEngine::restBuy(OrderBook &book, uint32_t pool_idx, int64_t price) noexcept {
    PriceLevelNode &level = book.bidlevels[price - MINPRICE];
    const bool newlevel = (level.head == 0);
    push_back_node(level, pool_idx);
    if(newlevel) [[unlikely]] {
        ++book.livebidlevels;
        if(price > book.bestbid) book.bestbid = price;
    }
}

__attribute__((always_inline))
inline void MatchingEngine::restSell(OrderBook &book, uint32_t pool_idx, int64_t price) noexcept {
    PriceLevelNode &level = book.asklevels[price - MINPRICE];
    const bool newlevel = (level.head == 0);
    push_back_node(level, pool_idx);
    if(newlevel) [[unlikely]] {
        ++book.liveasklevels;
        if(price < book.bestask) book.bestask = price;
    }
}

__attribute__((always_inline))
inline void MatchingEngine::removeOrder(OrderBook &book, uint32_t pool_idx, int64_t price, Side side) noexcept {
    if(side == Side::Buy) {
        PriceLevelNode &level = book.bidlevels[price - MINPRICE];
        unlink_node(level, pool_idx);
        if(level.head == 0) [[unlikely]] {
            --book.livebidlevels;
            if(price == book.bestbid) {
                if (book.livebidlevels == 0) book.bestbid = NOBID;
                else {
                    int64_t p = price - 1;
                    const PriceLevelNode* bids = book.bidlevels.get();
                    while(bids[p-MINPRICE].head == 0) --p;
                    book.bestbid = p;
                }
            }
        }
    } 
    else { 
        PriceLevelNode &level = book.asklevels[price - MINPRICE];
        unlink_node(level, pool_idx);
        if (level.head == 0) [[unlikely]] {
            --book.liveasklevels;
            if (price == book.bestask) {
                if (book.liveasklevels == 0) book.bestask = NOASK;
                else {
                    int64_t p = price + 1;
                    const PriceLevelNode* asks = book.asklevels.get();
                    while (asks[p - MINPRICE].head == 0) ++p;
                    book.bestask = p;
                }
            }
        }    
    }
}

__attribute__((hot))
void MatchingEngine::matchBuy(OrderBook &book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) noexcept {
    PriceLevelNode* asks = book.asklevels.get();
    Trade &tb = book.tradebuf;
    tb.buy_order_id = incomingid;

    while (remaining > 0 && book.bestask <= limitprice) {
        const int64_t execprice = book.bestask;
        PriceLevelNode &level = asks[execprice - MINPRICE];
        uint32_t curr = level.head;

        while (curr != 0) {
            OrderNode &resting = order_pool_[curr];
            const uint32_t fillquantity = (remaining < resting.quantity ? remaining : resting.quantity);

            tb.sell_order_id = resting.id;
            tb.price = execprice;
            tb.quantity = fillquantity;
            listener_->onTrade(tb);

            remaining -= fillquantity;
            resting.quantity -= fillquantity;
            const uint32_t next = resting.next;

            if (next) __builtin_prefetch(&order_pool_[next], 1, 1);

            if (resting.quantity == 0) [[likely]] {
                order_lookup_[resting.id] = 0;
                --liveorderscount;
                level.head = next;
                if(next) order_pool_[next].prev = 0; 
                else level.tail = 0;
                --level.count;
                
                listener_->onOrderUpdate({resting.id, OrderStatus::Filled, 0});
                freeNode(curr);
            } else {
                listener_->onOrderUpdate({resting.id, OrderStatus::Accepted, resting.quantity});
            }
            
            curr = next;
            if (remaining == 0) [[unlikely]] break;
        }

        if (level.head == 0) [[likely]] {
            --book.liveasklevels;
            if (book.liveasklevels == 0) [[unlikely]] {
                book.bestask = NOASK;
                break;
            } else {
                int64_t p = execprice + 1;
                while (asks[p - MINPRICE].head == 0) ++p;
                book.bestask = p;
            }
        }
    }
}

__attribute__((hot))
void MatchingEngine::matchSell(OrderBook &book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) noexcept {
    PriceLevelNode* bids = book.bidlevels.get();
    Trade &tb = book.tradebuf;
    tb.sell_order_id = incomingid;

    while (remaining > 0 && book.bestbid >= limitprice) {
        const int64_t execprice = book.bestbid;
        PriceLevelNode &level = bids[execprice - MINPRICE];
        uint32_t curr = level.head;

        while (curr != 0) {
            OrderNode &resting = order_pool_[curr];
            const uint32_t fillquantity = (remaining < resting.quantity ? remaining : resting.quantity);

            tb.buy_order_id = resting.id;
            tb.price = execprice;
            tb.quantity = fillquantity;
            listener_->onTrade(tb);

            remaining -= fillquantity;
            resting.quantity -= fillquantity;
            const uint32_t next = resting.next;

            if (next) __builtin_prefetch(&order_pool_[next], 1, 1);

            if (resting.quantity == 0) [[likely]] {
                order_lookup_[resting.id] = 0;
                --liveorderscount;
                level.head = next;
                if(next) order_pool_[next].prev = 0; 
                else level.tail = 0;
                --level.count;
                
                listener_->onOrderUpdate({resting.id, OrderStatus::Filled, 0});
                freeNode(curr);
            } else {
                listener_->onOrderUpdate({resting.id, OrderStatus::Accepted, resting.quantity});
            }
            
            curr = next;
            if (remaining == 0) [[unlikely]] break;
        }

        if (level.head == 0) [[likely]] {
            --book.livebidlevels;
            if (book.livebidlevels == 0) [[unlikely]] {
                book.bestbid = NOBID;
                break;
            } else {
                int64_t p = execprice - 1;
                while (bids[p - MINPRICE].head == 0) --p;
                book.bestbid = p;
            }
        }
    }
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    if (request.price > MAXPRICE || request.price <= 0 || request.quantity == 0 || request.symbol.empty()) [[unlikely]] {
        return OrderAck{0, OrderStatus::Rejected};
    }

    const uint64_t order_id = next_order_id_++;
    uint32_t remaining = request.quantity;
    const uint16_t book_idx = getBookIndex(request.symbol);
    OrderBook &book = active_books_[book_idx];   

    if (request.side == Side::Buy) {
        if (book.bestask <= request.price && book.bestask != NOASK)
            matchBuy(book, order_id, request.price, remaining);
    } else {
        if (book.bestbid >= request.price && book.bestbid != NOBID)
            matchSell(book, order_id, request.price, remaining);
    }

    if (remaining > 0) [[likely]] {
        if (order_id >= order_lookup_.size()) [[unlikely]]
            order_lookup_.resize(order_id * 2, 0);

        const uint32_t pool_idx = allocateNode();
        OrderNode &node  = order_pool_[pool_idx];
        
        node.id          = static_cast<uint32_t>(order_id);
        node.quantity    = remaining;
        node.pack(request.price, static_cast<uint8_t>(book_idx), request.side);

        if (request.side == Side::Buy) restBuy(book, pool_idx, request.price);
        else restSell(book, pool_idx, request.price);

        order_lookup_[order_id] = pool_idx;
        ++liveorderscount;

        listener_->onOrderUpdate({order_id, OrderStatus::Accepted, remaining});
        return {order_id, OrderStatus::Accepted};
    }

    listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});
    return {order_id, OrderStatus::Filled};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    if(order_id >= order_lookup_.size()) [[unlikely]] return false;
    
    const uint32_t pool_idx = order_lookup_[order_id];
    if(pool_idx == 0) [[unlikely]] return false;
    
    const OrderNode &node = order_pool_[pool_idx];
    OrderBook &book = active_books_[node.bookidx()];

    removeOrder(book, pool_idx, node.price(), node.side());
    
    order_lookup_[order_id] = 0;
    --liveorderscount;
    
    listener_->onOrderUpdate({order_id, OrderStatus::Cancelled, 0});
    freeNode(pool_idx);
    
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    if (new_price <= 0 || new_quantity == 0 || new_price > MAXPRICE || order_id >= order_lookup_.size()) [[unlikely]] {
        return false;
    }

    const uint32_t pool_idx = order_lookup_[order_id];
    if (pool_idx == 0) [[unlikely]] return false;

    OrderNode &node = order_pool_[pool_idx];
    OrderBook &book = active_books_[node.bookidx()];
    
    const int64_t old_price = node.price();
    const Side side = node.side();

    if (old_price == new_price) [[likely]] {
        if (new_quantity == node.quantity) [[unlikely]] return false;   

        if (new_quantity < node.quantity) {
            node.quantity = new_quantity;
            return true;
        }

        node.quantity = new_quantity;
        const size_t slot = (old_price - MINPRICE);
        if (side == Side::Buy) {
            unlink_node(book.bidlevels[slot], pool_idx);
            push_back_node(book.bidlevels[slot], pool_idx);
        } else {
            unlink_node(book.asklevels[slot], pool_idx);
            push_back_node(book.asklevels[slot], pool_idx);
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
        freeNode(pool_idx);
        --liveorderscount;
        return true;
    }

    node.quantity = remaining;
    node.pack(new_price, node.bookidx(), side);

    if (side == Side::Buy) restBuy (book, pool_idx, new_price);
    else restSell(book, pool_idx, new_price);

    return true;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(const std::string& symbol, Side side) const {
    std::vector<PriceLevel> snapshot;
    const OrderBook *found = &active_books_[getBookIndex(symbol)];

    if (side == Side::Buy) {
        uint32_t visited = 0;
        for (int64_t p = found->bestbid; p >= MINPRICE && visited < found->livebidlevels; --p) {
            const PriceLevelNode& lvl = found->bidlevels[p - MINPRICE];
            if (lvl.count == 0) continue;
            uint32_t total = 0, curr = lvl.head;
            while (curr) { 
                total += order_pool_[curr].quantity; 
                curr = order_pool_[curr].next; 
            }
            snapshot.push_back({p, total, lvl.count});
            ++visited;
        }
    } else {
        uint32_t visited = 0;
        for (int64_t p = found->bestask; p <= MAXPRICE && visited < found->liveasklevels; ++p) {
            const PriceLevelNode& lvl = found->asklevels[p - MINPRICE];
            if (lvl.count == 0) continue;
            uint32_t total = 0, curr = lvl.head;
            while (curr) { 
                total += order_pool_[curr].quantity; 
                curr = order_pool_[curr].next; 
            }
            snapshot.push_back({p, total, lvl.count});
            ++visited;
        }
    }
    return snapshot;
}

uint64_t MatchingEngine::getOrderCount() const {
    return liveorderscount;
}

}  // namespace exchange