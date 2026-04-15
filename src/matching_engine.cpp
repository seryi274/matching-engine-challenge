#include "exchange/matching_engine.h"

namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{   
    // TODO: Initialize your data structures here.
    
    active_books_ = std::make_unique<OrderBook[]>(5);
    order_lookup_.resize(11000000, LookupEntry{});
    order_pool_.resize(5000000); 
    free_stack_.resize(5000000);
    
    // Build the zero-branch free-stack once
    free_top_ = 0;
    for(uint32_t i = 4999999; i > 0; --i) {
        free_stack_[free_top_++] = i;
    }

    const char* const SYMBOLS[] = {"AAPL", "AMZN", "GOOG", "MSFT", "TSLA"};
    for (int i = 0; i < 5; ++i) {
        active_books_[i].symbol = SYMBOLS[i];
        active_books_[i].tradebuf.symbol = SYMBOLS[i];
        active_books_[i].bestbid = NOBID;
        active_books_[i].bestask = NOASK;
        active_books_[i].livebidlevels = 0;
        active_books_[i].liveasklevels = 0;
    }
}

MatchingEngine::~MatchingEngine() {
    // TODO: Clean up if necessary.
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
inline uint32_t MatchingEngine::allocateNode() noexcept {
    return free_stack_[--free_top_];
}

__attribute__((always_inline))
inline void MatchingEngine::freeNode(uint32_t idx) noexcept {
    free_stack_[free_top_++] = idx;
}

__attribute__((always_inline))
inline void MatchingEngine::unlink_node(PriceLevelNode &__restrict__ level, uint32_t curr) noexcept {
    OrderNode* __restrict__ pool = order_pool_.data();
    const uint32_t prev = pool[curr].prev;
    const uint32_t next = pool[curr].next;
    
    if (prev) pool[prev].next = next;
    else level.head = next;

    if (next) pool[next].prev = prev;
    else level.tail = prev;
}

__attribute__((always_inline))
inline void MatchingEngine::push_back_node(PriceLevelNode &__restrict__ level, uint32_t curr) noexcept {
    OrderNode* __restrict__ pool = order_pool_.data();
    pool[curr].next = 0;
    pool[curr].prev = level.tail;
    
    if (level.tail) pool[level.tail].next = curr;
    else level.head = curr;
    
    level.tail = curr;
}

__attribute__((always_inline))
inline void MatchingEngine::restBuy(OrderBook &__restrict__ book, uint32_t pool_idx, int64_t price) noexcept {
    PriceLevelNode &__restrict__ level = book.bidlevels[price];
    const bool newlevel = (level.head == 0);
    push_back_node(level, pool_idx);
    if(newlevel) [[unlikely]] {
        ++book.livebidlevels;
        if(price > book.bestbid) book.bestbid = price;
    }
}

__attribute__((always_inline))
inline void MatchingEngine::restSell(OrderBook &__restrict__ book, uint32_t pool_idx, int64_t price) noexcept {
    PriceLevelNode &__restrict__ level = book.asklevels[price];
    const bool newlevel = (level.head == 0);
    push_back_node(level, pool_idx);
    if(newlevel) [[unlikely]] {
        ++book.liveasklevels;
        if(price < book.bestask) book.bestask = price;
    }
}

__attribute__((always_inline))
inline void MatchingEngine::removeOrder(OrderBook &__restrict__ book, uint32_t pool_idx, int64_t price, Side side) noexcept {
    if(side == Side::Buy) {
        PriceLevelNode &__restrict__ level = book.bidlevels[price];
        unlink_node(level, pool_idx);
        if(level.head == 0) [[unlikely]] {
            --book.livebidlevels;
            if(price == book.bestbid) {
                if (book.livebidlevels == 0) book.bestbid = NOBID;
                else {
                    int64_t p = price - 1;
                    const PriceLevelNode* __restrict__ bids = book.bidlevels;
                    while(bids[p].head == 0) --p;
                    book.bestbid = p;
                }
            }
        }
    } 
    else { 
        PriceLevelNode &__restrict__ level = book.asklevels[price];
        unlink_node(level, pool_idx);
        if (level.head == 0) [[unlikely]] {
            --book.liveasklevels;
            if (price == book.bestask) {
                if (book.liveasklevels == 0) {
                    book.bestask = NOASK;
                } else {
                    int64_t p = price + 1;
                    const PriceLevelNode* __restrict__ asks = book.asklevels;
                    while (asks[p].head == 0) ++p;
                    book.bestask = p;
                }
            }
        }    
    }
}

__attribute__((hot))
void MatchingEngine::matchBuy(OrderBook &__restrict__ book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) noexcept {
    PriceLevelNode* __restrict__ asks = book.asklevels;
    OrderNode* __restrict__ pool = order_pool_.data();
    LookupEntry* __restrict__ lookup = order_lookup_.data();
    Trade &tb = book.tradebuf;
    tb.buy_order_id = incomingid;

    while (remaining > 0 && book.bestask <= limitprice) {
        const int64_t execprice = book.bestask;
        PriceLevelNode &__restrict__ level = asks[execprice];
        uint32_t curr = level.head;

        while (curr != 0) {
            OrderNode &__restrict__ resting = pool[curr];
            const uint32_t fillquantity = (remaining < resting.quantity ? remaining : resting.quantity);

            tb.sell_order_id = resting.id;
            tb.price = execprice;
            tb.quantity = fillquantity;
            listener_->onTrade(tb);

            remaining -= fillquantity;
            resting.quantity -= fillquantity;
            const uint32_t next = resting.next;

            if (next) __builtin_prefetch(&pool[next], 1, 1);

            if (resting.quantity == 0) [[likely]] {
                lookup[resting.id].pool_idx = 0;
                --liveorderscount;
                level.head = next;
                if(next) pool[next].prev = 0; 
                else level.tail = 0;
                
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
                while (asks[p].head == 0) ++p;
                book.bestask = p;
            }
        }
    }
}

__attribute__((hot))
void MatchingEngine::matchSell(OrderBook &__restrict__ book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) noexcept {
    PriceLevelNode* __restrict__ bids = book.bidlevels;
    OrderNode* __restrict__ pool = order_pool_.data();
    LookupEntry* __restrict__ lookup = order_lookup_.data();
    Trade &tb = book.tradebuf;
    tb.sell_order_id = incomingid;

    while (remaining > 0 && book.bestbid >= limitprice) {
        const int64_t execprice = book.bestbid;
        PriceLevelNode &__restrict__ level = bids[execprice];
        uint32_t curr = level.head;

        while (curr != 0) {
            OrderNode &__restrict__ resting = pool[curr];
            const uint32_t fillquantity = (remaining < resting.quantity ? remaining : resting.quantity);

            tb.buy_order_id = resting.id;
            tb.price = execprice;
            tb.quantity = fillquantity;
            listener_->onTrade(tb);

            remaining -= fillquantity;
            resting.quantity -= fillquantity;
            const uint32_t next = resting.next;

            if (next) __builtin_prefetch(&pool[next], 1, 1);

            if (resting.quantity == 0) [[likely]] {
                lookup[resting.id].pool_idx = 0;
                --liveorderscount;
                level.head = next;
                if(next) pool[next].prev = 0; 
                else level.tail = 0;
                
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
                while (bids[p].head == 0) --p;
                book.bestbid = p;
            }
        }
    }
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    // Step 1: Validate the request.
    if (request.price > MAXPRICE || request.price <= 0 || request.quantity == 0 || request.symbol.empty()) [[unlikely]] {
        return OrderAck{0, OrderStatus::Rejected};
    }

    // Step 2: Assign an order ID.
    const uint32_t order_id = static_cast<uint32_t>(next_order_id_++);
    uint32_t remaining = request.quantity;
    
    // TODO: Step 3: Look up (or create) the order book for request.symbol.
    const uint16_t book_idx = getBookIndex(request.symbol);
    OrderBook &__restrict__ book = active_books_[book_idx];   

    // TODO: Step 4: Attempt to match against the opposite side.
    if (request.side == Side::Buy) {
        if (book.bestask <= request.price && book.bestask != NOASK)
            matchBuy(book, order_id, request.price, remaining);
    } else {
        if (book.bestbid >= request.price && book.bestbid != NOBID)
            matchSell(book, order_id, request.price, remaining);
    }

    // TODO: Step 5: If quantity remains, insert into the book.
    if (remaining > 0) [[likely]] {
        const uint32_t pool_idx = allocateNode();
        OrderNode* __restrict__ pool = order_pool_.data();
        OrderNode &__restrict__ node = pool[pool_idx];
        
        node.id          = order_id;
        node.quantity    = remaining;
        node.prev = node.next = 0;

        LookupEntry* __restrict__ lookup = order_lookup_.data();
        lookup[order_id].pool_idx = pool_idx;
        lookup[order_id].price = static_cast<uint16_t>(request.price);
        lookup[order_id].bookidx = static_cast<uint8_t>(book_idx);
        lookup[order_id].side = request.side;

        if (request.side == Side::Buy) restBuy(book, pool_idx, request.price);
        else restSell(book, pool_idx, request.price);

        ++liveorderscount;

        listener_->onOrderUpdate({order_id, OrderStatus::Accepted, remaining});
        return {order_id, OrderStatus::Accepted};
    }

    // TODO: Step 6: Call listener_->onOrderUpdate() for the incoming order.
    // TODO: Step 7: Return the ack.
    listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});
    return {order_id, OrderStatus::Filled};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    // TODO: Look up the order by order_id.
    if(order_id >= order_lookup_.size()) [[unlikely]] return false;
    
    LookupEntry* __restrict__ lookup = order_lookup_.data();
    const uint32_t pool_idx = lookup[order_id].pool_idx;
    if(pool_idx == 0) [[unlikely]] return false;
    
    OrderBook &__restrict__ book = active_books_[lookup[order_id].bookidx];

    removeOrder(book, pool_idx, lookup[order_id].price, lookup[order_id].side);
    
    lookup[order_id].pool_idx = 0;
    --liveorderscount;
    
    listener_->onOrderUpdate({order_id, OrderStatus::Cancelled, 0});
    freeNode(pool_idx);
    
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    // Validate parameters.
    if (new_price <= 0 || new_quantity == 0 || new_price > MAXPRICE || order_id >= order_lookup_.size()) [[unlikely]] {
        return false;
    }

    // TODO: Look up the order by order_id.
    LookupEntry* __restrict__ lookup = order_lookup_.data();
    const uint32_t pool_idx = lookup[order_id].pool_idx;
    if (pool_idx == 0) [[unlikely]] return false;

    OrderNode* __restrict__ pool = order_pool_.data();
    OrderNode &__restrict__ node = pool[pool_idx];
    OrderBook &__restrict__ book = active_books_[lookup[order_id].bookidx];
    
    const int64_t old_price = lookup[order_id].price;
    const Side side = lookup[order_id].side;

    if (old_price == new_price) [[likely]] {
        if (new_quantity == node.quantity) [[unlikely]] return false;   

        if (new_quantity < node.quantity) {
            node.quantity = new_quantity;
            return true;
        }

        node.quantity = new_quantity;
        if (side == Side::Buy) {
            unlink_node(book.bidlevels[old_price], pool_idx);
            push_back_node(book.bidlevels[old_price], pool_idx);
        } else {
            unlink_node(book.asklevels[old_price], pool_idx);
            push_back_node(book.asklevels[old_price], pool_idx);
        }
        return true;
    }

    removeOrder(book, pool_idx, old_price, side);

    uint32_t remaining = new_quantity;
    if (side == Side::Buy) matchBuy (book, order_id, new_price, remaining);
    else matchSell(book, order_id, new_price, remaining);

    if (remaining == 0) {
        listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});
        lookup[order_id].pool_idx = 0;
        freeNode(pool_idx);
        --liveorderscount;
        return true;
    }

    lookup[order_id].price = new_price; 
    node.quantity = remaining;
    node.prev = node.next = 0;

    if (side == Side::Buy) restBuy (book, pool_idx, new_price);
    else restSell(book, pool_idx, new_price);

    return true;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(const std::string& symbol, Side side) const {
    // TODO: Build a vector of PriceLevel for the requested side.
    std::vector<PriceLevel> snapshot;
    const OrderBook *found = &active_books_[getBookIndex(symbol)];

    if (side == Side::Buy) {
        uint32_t visited = 0;
        for (int64_t p = found->bestbid; p >= MINPRICE && visited < found->livebidlevels; --p) {
            const PriceLevelNode& lvl = found->bidlevels[p];
            if (lvl.head == 0) continue;
            uint32_t total = 0, curr = lvl.head, count = 0;
            while (curr) { 
                total += order_pool_[curr].quantity; 
                count++;
                curr = order_pool_[curr].next; 
            }
            snapshot.push_back({p, total, count});
            ++visited;
        }
    } else {
        uint32_t visited = 0;
        for (int64_t p = found->bestask; p <= MAXPRICE && visited < found->liveasklevels; ++p) {
            const PriceLevelNode& lvl = found->asklevels[p];
            if (lvl.head == 0) continue;
            uint32_t total = 0, curr = lvl.head, count = 0;
            while (curr) { 
                total += order_pool_[curr].quantity; 
                count++;
                curr = order_pool_[curr].next; 
            }
            snapshot.push_back({p, total, count});
            ++visited;
        }
    }
    return snapshot;
}

uint64_t MatchingEngine::getOrderCount() const {
    // TODO: Return total number of resting orders across all symbols.
    return liveorderscount;
}

}  // namespace exchange