#include "exchange/matching_engine.h"


namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
    // TODO: Initialize your data structures here.
}

MatchingEngine::~MatchingEngine() {
    // TODO: Clean up if necessary.
}

inline void MatchingEngine::match_offers(std::list<BookEntry>* list, uint32_t& amount, const std::string& symbol, const uint32_t& price, const uint64_t& caller_id, const Side& caller_side) {
    //   - For each match, call:
    //       listener_->onTrade(Trade{buy_id, sell_id, symbol, price, qty});
    //       listener_->onOrderUpdate(OrderUpdate{resting_id, status, remaining});
    while (amount && list->size()) {
        uint32_t offer_am = list->begin()->quantity;
        uint64_t offer_id = list->begin()->id;
        uint32_t match_am = std::min(amount, offer_am);
        // match_am should never be zero!
        uint32_t remaining_am = offer_am-match_am;
        amount -= match_am;
        if (caller_side == Side::Buy) {
            listener_->onTrade(Trade{caller_id, offer_id, symbol, price, match_am});
        } else {
            listener_->onTrade(Trade{offer_id, caller_id, symbol, price, match_am});
        }
        if (remaining_am == 0) {
            list->erase(list->begin());
            order_ids.erase(offer_id);
            order_requests.erase(offer_id);
            order_count--;
            listener_->onOrderUpdate(OrderUpdate{offer_id, OrderStatus::Filled, 0});
        } else {
            list->begin()->quantity = remaining_am;
            listener_->onOrderUpdate(OrderUpdate{offer_id, OrderStatus::Accepted, remaining_am});
        }
    }
    
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    // Step 2: Assign an order ID.
    uint64_t order_id = next_order_id_++;
    return addOrderWithId(request, order_id);
}

OrderAck MatchingEngine::addOrderWithId(const OrderRequest& request, uint64_t order_id) {
    // Step 1: Validate the request.
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) {
        return OrderAck{0, OrderStatus::Rejected};
    }

    uint32_t amount = request.quantity;
    std::string symbol = request.symbol;
    int64_t price = request.price;
    exchange::Side side = request.side;

    //std::cout << "Adding order: " << order_id << ' ' << amount << ' ' << symbol << ' ' << price << ' ' << (side==Side::Buy?"Buy":"Sell") << std::endl;

    // TODO: Step 3: Look up (or create) the order book for request.symbol.
    //
    //   Hint: You need a per-symbol data structure that maintains
    //   buy orders (bids) and sell orders (asks) separately.

    if (!bids.count(symbol)) {
        bids[symbol] = std::unordered_map<int64_t, std::list<BookEntry>>();
        asks[symbol] = std::unordered_map<int64_t, std::list<BookEntry>>();
        bid_prc[symbol] = std::priority_queue<int64_t>();
        ask_prc[symbol] = std::priority_queue<int64_t>();
    }

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

    if (side == Side::Buy) {
        // We want to buy, so we need to match offers from the selling side (asks) untill ask.price > price.
        std::priority_queue<int64_t>* ask_prices = &ask_prc.at(symbol);
        std::unordered_map<int64_t, std::list<BookEntry>>* s_asks = &asks.at(symbol);
        while (amount && ask_prices->size()) {
            int best_ask = -ask_prices->top();
            if (best_ask > price) break;
            std::list<BookEntry>* ask_list = &s_asks->at(best_ask);
            //match_offers(std::list<BookEntry>* list, uint32_t& amount, const std::string& symbol, const uint32_t& price, const uint64_t& caller_id, const Side& caller_side)
            match_offers(ask_list, amount, symbol, best_ask, order_id, Side::Buy);
            if (ask_list->empty()) {
                ask_prices->pop();
            }
        }
    } else {
        // We want to sell, so we need to match offers from the buyers (bids) untill bid.price < price.
        std::priority_queue<int64_t>* bid_prices = &bid_prc.at(symbol);
        std::unordered_map<int64_t, std::list<BookEntry>>* s_bids = &bids.at(symbol);
        while (amount && bid_prices->size()) {
            int best_bid = bid_prices->top();
            if (best_bid < price) break;
            std::list<BookEntry>* bid_list = &s_bids->at(best_bid);
            //match_offers(std::list<BookEntry>* list, uint32_t& amount, const std::string& symbol, const uint32_t& price, const uint64_t& caller_id, const Side& caller_side)
            match_offers(bid_list, amount, symbol, best_bid, order_id, Side::Sell);
            if (bid_list->empty()) {
                bid_prices->pop();
            }
        }
    }

    // TODO: Step 5: If quantity remains, insert into the book.

    if (amount) {
        if (side == Side::Buy) {
            std::unordered_map<int64_t, std::list<BookEntry>>* s_bids = &bids.at(symbol);
            if (!s_bids->count(price) || s_bids->at(price).size() == 0) {
                bid_prc[symbol].push(price);
            }
            std::list<BookEntry>* list = &(*s_bids)[price];
            list->push_back(BookEntry{amount, order_id});
            order_ids[order_id] = {list, std::prev(list->end())};
            order_requests[order_id] = request;
            order_count++;
        } else {
            std::unordered_map<int64_t, std::list<BookEntry>>* s_asks = &asks.at(symbol);
            if (!s_asks->count(price) || s_asks->at(price).size() == 0) {
                ask_prc[symbol].push(-price);
            }
            std::list<BookEntry>* list = &(*s_asks)[price];
            list->push_back(BookEntry{amount, order_id});
            order_ids[order_id] = {list, std::prev(list->end())};
            order_requests[order_id] = request;
            order_count++;
        }
    }

    // TODO: Step 6: Call listener_->onOrderUpdate() for the incoming order.
    //   - status = Filled     if fully matched (remaining == 0)
    //   - status = Accepted   if resting on the book (remaining > 0)

    if (amount == 0) {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Filled, amount});
    } else {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Accepted, amount});
    }

    // TODO: Step 7: Return the ack.
    //   - status = Filled   if fully matched
    //   - status = Accepted if resting (including partial fills)
    if (amount == 0) {
        return OrderAck{order_id, OrderStatus::Filled};
    } else {
        return OrderAck{order_id, OrderStatus::Accepted};
    }
    //return OrderAck{order_id, OrderStatus::Rejected};  // placeholder -- replace this
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
    //std::cout << "Cancelling order:" << order_id << std::endl;
    if (!order_ids.count(order_id)) return false;
    auto p = order_ids[order_id];
    p.first->erase(p.second);
    order_ids.erase(order_id);
    order_requests.erase(order_id);
    order_count--;
    listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});
    return true; 
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    // Validate parameters.
    if (new_price <= 0 || new_quantity == 0) {
        return false;
    }
    //std::cout << "Calling amend:" << order_id << ' ' << new_price << ' ' << new_quantity << std::endl;
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
    if (!order_ids.count(order_id)) return false;
    OrderRequest original_req = order_requests.at(order_id);
    auto lst_pr = order_ids.at(order_id);
    if (original_req.price == new_price && lst_pr.second->quantity >= new_quantity) {
        lst_pr.second->quantity = new_quantity;
        return true;
    }
    // Remove and add again lol
    cancelOrder(order_id);
    addOrderWithId(OrderRequest{original_req.symbol, original_req.side, new_price, new_quantity}, order_id);
    return true;
}

inline std::vector<PriceLevel> MatchingEngine::get_snapshot(const std::unordered_map<int64_t, std::list<BookEntry>>& um) const { 
    std::vector<PriceLevel> res;
    for (const auto& p : um) {
        uint32_t su = 0;
        for (const auto& el : p.second) {
            su += el.quantity;
        }
        res.push_back(PriceLevel{p.first, su, (uint32_t)p.second.size()});
    }
    return res;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    // TODO: Build a vector of PriceLevel for the requested side.
    //
    // Sort best-to-worst:
    //   Buy side:  highest price first
    //   Sell side: lowest price first
    
    //std::cout << "Getting snapshot: " << symbol << ' ' << (side==Side::Buy?"Buy":"Sell") << std::endl;
    if (!bids.count(symbol)) return {};

    std::vector<PriceLevel> book;
    std::string symb = symbol;
    if (side == Side::Buy) 
        book = get_snapshot(bids.at(symb));
    else
        book = get_snapshot(asks.at(symb));
    auto cmp = [&](PriceLevel a, PriceLevel b) {
        if (side == Side::Buy) 
            return a.price > b.price;
        else 
            return a.price < b.price;
    };
    std::sort(book.begin(), book.end(), cmp);
    return book;  // placeholder
}

uint64_t MatchingEngine::getOrderCount() const {
    // TODO: Return total number of resting orders across all symbols.

    return order_count;  // placeholder
}

}  // namespace exchange
