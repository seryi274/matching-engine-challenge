#include "exchange/matching_engine.h"

namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
    // TODO: Initialize your data structures here.

    active_books_.reserve(32);
    // pre sized lookup and pool based on benchmark
    order_lookup_.resize(8500000);
    order_pool_.reserve(1500000);
    order_pool_.push_back({});

}

MatchingEngine::~MatchingEngine() {
    // TODO: Clean up if necessary.
}

// //hardcoded for benchmark
// inline uint16_t MatchingEngine::getBookIndex(const std::string & symbol) const {
//     const char c2 = symbol[2];
//     switch(c2) {
//         case 'A': return 0;
//         case 'M': return 1;
//         case 'O': return 2;
//         case 'F': return 3;
//         case 'L': return 4;
//         default: return -1;
//     }
// }

uint64_t MatchingEngine::hashSymbol(const std::string& s) const {
    uint64_t h = 0;
    const int n = (s.size() < 8 ? s.size() : 8);
    for (int i = 0; i < n; ++i) 
        h |= static_cast<uint64_t>(static_cast<uint8_t>(s[i])) << (i * 8);
    return h;
}

uint16_t MatchingEngine::getOrCreateBook(const std::string& symbol) {
    const uint64_t sh = hashSymbol(symbol);
    for (size_t i = 0; i < active_books_.size(); ++i) {
        if (active_books_[i].symbol_hash == sh) {
            return static_cast<uint16_t>(i);
        }
    }

    const uint16_t idx = static_cast<uint16_t>(active_books_.size());
    OrderBook b;
    b.bidlevels.reset(new PriceLevelNode[PRICESLOTS]());
    b.asklevels.reset(new PriceLevelNode[PRICESLOTS]());
    b.symbol            = symbol;
    b.symbol_hash       = sh;
    b.tradebuf.symbol   = symbol;   

    b.livebidlevels     = 0;
    b.liveasklevels     = 0;
    b.bestbid           = NOBID;
    b.bestask           = NOASK;

    active_books_.push_back(std::move(b));
    return idx;
}

//node pool

//custom memory allocator
uint32_t MatchingEngine::allocateNode() {
    if(free_head_ != 0) {
        const uint32_t idx = free_head_;
        free_head_ = order_pool_[idx].next;
        return idx;
    }
    order_pool_.push_back({});
    return order_pool_.size() -1;
}

void MatchingEngine::freeNode(uint32_t idx) {
    order_pool_[idx].next = free_head_;
    free_head_ = idx;
}

//intrusive double-linked list helpers

void MatchingEngine::unlink_node(PriceLevelNode &level, uint32_t curr) {
    const uint32_t prev = order_pool_[curr].prev;
    const uint32_t next = order_pool_[curr].next;
    if(prev) order_pool_[prev].next = next;
    else level.head = next;

    if(next) order_pool_[next].prev = prev;
    else level.tail = prev;

    --level.count;
}

void MatchingEngine::push_back_node(PriceLevelNode &level, uint32_t curr) {
    order_pool_[curr].next = 0;
    order_pool_[curr].prev = level.tail;
    if(level.tail) order_pool_[level.tail].next = curr;
    else level.head = curr;
    level.tail = curr;
    ++level.count;
}

//book insert and remove

void MatchingEngine::restBuy(OrderBook &book, uint32_t pool_idx, int64_t price) {
    PriceLevelNode &level = book.bidlevels[price - MINPRICE];
    const bool newlevel = (level.head == 0);
    push_back_node(level, pool_idx);
    if(newlevel) {
        ++book.livebidlevels;
        if(price > book.bestbid) book.bestbid = price;
    }
}

void MatchingEngine::restSell(OrderBook &book, uint32_t pool_idx, int64_t price) {
    PriceLevelNode &level = book.asklevels[price - MINPRICE];
    const bool newlevel = (level.head == 0);
    push_back_node(level, pool_idx);
    if(newlevel) {
        ++book.liveasklevels;
        if(price < book.bestask) book.bestask = price;
    }
}

void MatchingEngine::removeOrder(OrderBook &book, uint32_t pool_idx, int64_t  price, Side side) {
    if(side==Side::Buy) {
        PriceLevelNode &level = book.bidlevels[price - MINPRICE];
        unlink_node(level, pool_idx);
        if(level.head == 0) {
            --book.livebidlevels;
            if(price == book.bestbid) {
                if (book.livebidlevels == 0) book.bestbid = NOBID;
                else {
                    int64_t p = price - 1;
                    const PriceLevelNode *bids = book.bidlevels.get();
                    while(bids[p-MINPRICE].head == 0) --p;
                    book.bestbid = p;
                }
            }
        }
    } 
    else { 
        PriceLevelNode& level = book.asklevels[price - MINPRICE];
        unlink_node(level, pool_idx);
        if (level.head == 0) {
            --book.liveasklevels;
            if (price == book.bestask) {
                if (book.liveasklevels == 0) {
                    book.bestask = NOASK;
                } else {
                    int64_t p = price + 1;
                    const PriceLevelNode* asks = book.asklevels.get();
                    while (asks[p - MINPRICE].head == 0) ++p;
                    book.bestask = p;
                }
            }
        }    
    }
}


//matching core

void MatchingEngine::matchBuy(OrderBook &book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) {
    PriceLevelNode *asks = book.asklevels.get();
    Trade &tb = book.tradebuf;
    OrderUpdate update;
    tb.buy_order_id = incomingid;

    while(remaining > 0 and book.bestask <= limitprice) {
        const int64_t execprice = book.bestask;
        PriceLevelNode &level = asks[execprice - MINPRICE];
        uint32_t curr = level.head;

        while(curr != 0 and remaining > 0) {
            OrderNode &resting = order_pool_[curr];
            const uint32_t fillquantity = (remaining < resting.quantity ? remaining : resting.quantity);

            tb.sell_order_id = resting.id;
            tb.price = execprice;
            tb.quantity = fillquantity;
            listener_->onTrade(tb);

            remaining -= fillquantity;
            resting.quantity -= fillquantity;
            const uint32_t next = resting.next;

            if(resting.quantity == 0) {
                // 1. Clear internal state completely
                order_lookup_[resting.id] = 0;
                --liveorderscount;
                level.head = next;
                if(next) order_pool_[next].prev = 0; 
                else level.tail = 0;
                --level.count;
                
                // 2. Fire external callback
                update = {resting.id, OrderStatus::Filled, 0};
                listener_->onOrderUpdate(update);
                
                freeNode(curr);
            } else {
                update = {resting.id, OrderStatus::Accepted, resting.quantity};
                listener_->onOrderUpdate(update);
            }
            curr = next;
        }

        if(level.head == 0) {
            --book.liveasklevels;
            if(book.liveasklevels == 0) book.bestask = NOASK;
            else {
                int64_t p = execprice +1;
                while(asks[p - MINPRICE].head == 0) ++p;
                book.bestask = p;
            }
        }
    }
}

void MatchingEngine::matchSell(OrderBook &book, uint64_t incomingid, int64_t limitprice, uint32_t &remaining) {
    PriceLevelNode *bids = book.bidlevels.get();
    Trade &tb = book.tradebuf;
    OrderUpdate update;
    tb.sell_order_id = incomingid;

    while(remaining > 0 and book.bestbid >= limitprice) {
        const int64_t execprice = book.bestbid;
        PriceLevelNode &level = bids[execprice - MINPRICE];
        uint32_t curr = level.head;

        while(curr != 0 and remaining > 0) {
            OrderNode &resting = order_pool_[curr];
            const uint32_t fillquantity = (remaining < resting.quantity ? remaining : resting.quantity);

            tb.buy_order_id = resting.id;
            tb.price = execprice;
            tb.quantity = fillquantity;
            listener_->onTrade(tb);

            remaining -= fillquantity;
            resting.quantity -= fillquantity;
            const uint32_t next = resting.next;

            if(resting.quantity == 0) {
                // 1. Clear internal state completely
                order_lookup_[resting.id] = 0;
                --liveorderscount;
                level.head = next;
                if(next) order_pool_[next].prev = 0; 
                else level.tail = 0;
                --level.count;
                
                // 2. Fire external callback
                update = {resting.id, OrderStatus::Filled, 0};
                listener_->onOrderUpdate(update);
                
                freeNode(curr);
            } else {
                update = {resting.id, OrderStatus::Accepted, resting.quantity};
                listener_->onOrderUpdate(update);
            }
            curr = next;
        }

        if(level.head == 0) {
            --book.livebidlevels;
            if(book.livebidlevels == 0) book.bestbid = NOBID;
            else {
                int64_t p = execprice +1;
                while(bids[p - MINPRICE].head == 0) --p;
                book.bestbid = p;
            }
        }
    }
}

//public api

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    // Step 1: Validate the request.
    if (request.price > MAXPRICE || request.price <= 0 || request.quantity == 0 || request.symbol.empty()) {
        return OrderAck{0, OrderStatus::Rejected};
    }

    // Step 2: Assign an order ID.
    uint64_t order_id = next_order_id_++;


    // TODO: Step 3: Look up (or create) the order book for request.symbol.
    //
    //   Hint: You need a per-symbol data structure that maintains
    //   buy orders (bids) and sell orders (asks) separately.

    uint32_t remaining = request.quantity;
    // const uint16_t book_idx = getBookIndex(request.symbol);
    const uint16_t book_idx = getOrCreateBook(request.symbol);
    OrderBook &book = active_books_[book_idx];   


    // TODO: Step 4: Attempt to match against the opposite side.
    //
    //   - Buy orders match against sells where sell.price <= buy.price
    //   - Sell orders match against buys  where buy.price  >= sell.price
    //   - Match at the RESTING order's price
    //   - Match greedily: consume as many resting orders as possible
    //   - Process resting orders in price-time priority:
    //       * Best price first (lowest ask for buys, highest bid for sells)
    //       * At the same price, earliest order first (FIFO)
    //   - For each match, call:
    //       listener_->onTrade(Trade{buy_id, sell_id, symbol, price, qty});
    //       listener_->onOrderUpdate(OrderUpdate{resting_id, status, remaining});

    if(request.side == Side::Buy) matchBuy(book, order_id, request.price, remaining);
    else matchSell(book, order_id, request.price, remaining);

    // TODO: Step 5: If quantity remains, insert into the book.

    if (remaining > 0) {
        if ((order_id >= order_lookup_.size()))
            order_lookup_.resize(order_id * 2, 0);

        const uint32_t pool_idx = allocateNode();
        OrderNode &node  = order_pool_[pool_idx];
        node.id          = order_id;
        node.price       = request.price;
        node.quantity    = remaining;
        node.bookidx    = book_idx;
        node.side        = request.side;
        node.prev = node.next = 0;

        if (request.side == Side::Buy)
            restBuy (book, pool_idx, request.price);
        else
            restSell(book, pool_idx, request.price);

        order_lookup_[order_id] = pool_idx;
        ++liveorderscount;

        listener_->onOrderUpdate({order_id, OrderStatus::Accepted, remaining});

        return {order_id, OrderStatus::Accepted};
    }

    // TODO: Step 6: Call listener_->onOrderUpdate() for the incoming order.
    //   - status = Filled     if fully matched (remaining == 0)
    //   - status = Accepted   if resting on the book (remaining > 0)

    listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});

    // TODO: Step 7: Return the ack.
    //   - status = Filled   if fully matched
    //   - status = Accepted if resting (including partial fills)
    //return OrderAck{order_id, OrderStatus::Rejected};  // placeholder -- replace this
    
    return {order_id, OrderStatus::Filled};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    // TODO: Look up the order by order_id.
    //
    // If found and still resting:
    //   1. Remove it from the order book
    //   2. Call listener_->onOrderUpdate(OrderUpdate{
    //          order_id, OrderStatus::Cancelled, 0});
    //   3. Return true
    //
    // If not found (or already filled/cancelled):
    //   Return false

    if(order_id >= order_lookup_.size()) return false;
    const uint32_t pool_idx = order_lookup_[order_id];
    if(pool_idx == 0) return false;
    const OrderNode &node = order_pool_[pool_idx];
    OrderBook &book = active_books_[node.bookidx];

    removeOrder(book, pool_idx, node.price, node.side);
    
    // 1. Clear internal state completely
    order_lookup_[order_id] = 0;
    --liveorderscount;
    
    // 2. Fire external callback
    listener_->onOrderUpdate({order_id, OrderStatus::Cancelled, 0});
    
    freeNode(pool_idx);
    return true;
    
    //return false;  // placeholder
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    // Validate parameters.
    if (new_price <= 0 || new_quantity == 0 or new_price > MAXPRICE or order_id>=order_lookup_.size()) {
        return false;
    }

    // TODO: Look up the order by order_id.
    //
    // If found and still resting:
    //   1. Determine if time priority is lost:
    //      - Price changed          -> loses priority
    //      - Quantity increased     -> loses priority
    //      - Only quantity decreased -> keeps priority
    //   2. Update the order in your data structures:
    //      - If losing priority: remove and re-insert at the back of the level
    //      - If keeping priority: update quantity in-place
    //   3. If the new price could cause a match (e.g., a buy order's price
    //      is raised above the best ask), run matching logic.
    //   4. Return true
    //
    // If not found: return false


    const uint32_t pool_idx = order_lookup_[order_id];
    if (pool_idx == 0) return false;

    OrderNode &node = order_pool_[pool_idx];
    OrderBook &book = active_books_[node.bookidx];
    const int64_t old_price = node.price;
    const Side side = node.side;

    if (old_price == new_price) {
        if (new_quantity == node.quantity) return false;   

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

    node.price = new_price;
    node.quantity = remaining;
    node.prev = node.next = 0;

    if (side == Side::Buy) restBuy (book, pool_idx, new_price);
    else restSell(book, pool_idx, new_price);

    return true;

    
    //return false;  // placeholder
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    // TODO: Build a vector of PriceLevel for the requested side.
    //
    // Sort best-to-worst:
    //   Buy side:  highest price first
    //   Sell side: lowest price first

    std::vector<PriceLevel> snapshot;
    
    // const OrderBook *found = &active_books_[getBookIndex(symbol)];

    const uint64_t sh = hashSymbol(symbol);
    const OrderBook *found = nullptr;
    for (const auto &b : active_books_) {
        if (b.symbol_hash == sh) { found = &b; break; }
    }
    if (!found) return snapshot;


    if (side == Side::Buy) {
        uint32_t visited = 0;
        for (int64_t p = found->bestbid; p >= MINPRICE && visited < found->livebidlevels; --p)
        {
            const PriceLevelNode& lvl = found->bidlevels[p - MINPRICE];
            if (lvl.count == 0) continue;
            uint32_t total = 0, curr = lvl.head;
            while (curr) { total += order_pool_[curr].quantity; curr = order_pool_[curr].next; }
            snapshot.push_back({p, total, lvl.count});
            ++visited;
        }
    } else {
        uint32_t visited = 0;
        for (int64_t p = found->bestask;
             p <= MAXPRICE && visited < found->liveasklevels; ++p)
        {
            const PriceLevelNode& lvl = found->asklevels[p - MINPRICE];
            if (lvl.count == 0) continue;
            uint32_t total = 0, curr = lvl.head;
            while (curr) { total += order_pool_[curr].quantity; curr = order_pool_[curr].next; }
            snapshot.push_back({p, total, lvl.count});
            ++visited;
        }
    }
    return snapshot;

    //return {};  // placeholder
}

uint64_t MatchingEngine::getOrderCount() const {
    // TODO: Return total number of resting orders across all symbols.
    return liveorderscount;
    //return 0;  // placeholder
}

}  // namespace exchange
