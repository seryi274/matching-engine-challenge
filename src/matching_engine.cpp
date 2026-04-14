#include "exchange/matching_engine.h"
#include <algorithm> 
#include <list>
#include <unordered_map>
#include <vector>

namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener), order_count(0), next_order_id_(1)
{
}

MatchingEngine::~MatchingEngine() {
}

// Ensure the signature matches the header exactly
void MatchingEngine::match_offers(std::list<BookEntry>* list, uint32_t& amount, const std::string& symbol, const int64_t& price, const uint64_t& caller_id, const Side& caller_side) {
    while (amount && !list->empty()) {
        auto& front_entry = list->front();
        uint32_t offer_am = front_entry.quantity;
        uint64_t offer_id = front_entry.id;
        uint32_t match_am = std::min(amount, offer_am);
        
        uint32_t remaining_am = offer_am - match_am;
        amount -= match_am;

        if (caller_side == Side::Buy) {
            listener_->onTrade(Trade{caller_id, offer_id, symbol, price, match_am});
        } else {
            listener_->onTrade(Trade{offer_id, caller_id, symbol, price, match_am});
        }

        if (remaining_am == 0) {
            order_ids.erase(offer_id);
            order_requests.erase(offer_id);
            list->pop_front();
            order_count--;
            listener_->onOrderUpdate(OrderUpdate{offer_id, OrderStatus::Filled, 0});
        } else {
            front_entry.quantity = remaining_am;
            listener_->onOrderUpdate(OrderUpdate{offer_id, OrderStatus::Accepted, remaining_am});
        }
    }
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    uint64_t order_id = next_order_id_++;
    return addOrderWithId(request, order_id);
}

OrderAck MatchingEngine::addOrderWithId(const OrderRequest& request, uint64_t order_id) {
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) {
        return OrderAck{0, OrderStatus::Rejected};
    }

    uint32_t amount = request.quantity;
    const std::string& symbol = request.symbol;
    int64_t price = request.price;
    Side side = request.side;

    // Initialize symbol books if they don't exist
    if (bids.find(symbol) == bids.end()) {
        bids[symbol] = {};
        asks[symbol] = {};
        bid_prc[symbol] = std::priority_queue<int64_t>();
        ask_prc[symbol] = std::priority_queue<int64_t>();
    }

    // Matching Logic
    if (side == Side::Buy) {
        auto& ask_prices = ask_prc.at(symbol);
        auto& s_asks = asks.at(symbol);
        while (amount && !ask_prices.empty()) {
            int64_t best_ask = -ask_prices.top(); // Prices stored as negative in PQ for min-heap behavior
            if (best_ask > price) break;
            
            auto& ask_list = s_asks.at(best_ask);
            match_offers(&ask_list, amount, symbol, best_ask, order_id, Side::Buy);
            
            if (ask_list.empty()) {
                ask_prices.pop();
            }
        }
    } else {
        auto& bid_prices = bid_prc.at(symbol);
        auto& s_bids = bids.at(symbol);
        while (amount && !bid_prices.empty()) {
            int64_t best_bid = bid_prices.top();
            if (best_bid < price) break;
            
            auto& bid_list = s_bids.at(best_bid);
            match_offers(&bid_list, amount, symbol, best_bid, order_id, Side::Sell);
            
            if (bid_list.empty()) {
                bid_prices.pop();
            }
        }
    }

    // Resting Order Logic
    if (amount > 0) {
        if (side == Side::Buy) {
            auto& s_bids = bids.at(symbol);
            if (s_bids[price].empty()) {
                bid_prc[symbol].push(price);
            }
            auto& list = s_bids[price];
            list.push_back(BookEntry{amount, order_id});
            order_ids[order_id] = {&list, std::prev(list.end())};
            order_requests[order_id] = request;
            order_count++;
        } else {
            auto& s_asks = asks.at(symbol);
            if (s_asks[price].empty()) {
                ask_prc[symbol].push(-price);
            }
            auto& list = s_asks[price];
            list.push_back(BookEntry{amount, order_id});
            order_ids[order_id] = {&list, std::prev(list.end())};
            order_requests[order_id] = request;
            order_count++;
        }
    }

    OrderStatus final_status = (amount == 0) ? OrderStatus::Filled : OrderStatus::Accepted;
    listener_->onOrderUpdate(OrderUpdate{order_id, final_status, amount});
    return OrderAck{order_id, final_status};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    if (order_ids.find(order_id) == order_ids.end()) return false;
    
    auto p = order_ids[order_id];
    p.first->erase(p.second);
    order_ids.erase(order_id);
    order_requests.erase(order_id);
    order_count--;
    
    listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});
    return true; 
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    if (new_price <= 0 || new_quantity == 0 || order_ids.find(order_id) == order_ids.end()) {
        return false;
    }

    const OrderRequest& original_req = order_requests.at(order_id);
    auto it_pair = order_ids.at(order_id);
    
    // Priority retention check
    if (original_req.price == new_price && new_quantity <= it_pair.second->quantity) {
        it_pair.second->quantity = new_quantity;
        return true;
    }

    cancelOrder(order_id);
    addOrderWithId(OrderRequest{original_req.symbol, original_req.side, new_price, new_quantity}, order_id);
    return true;
}

std::vector<PriceLevel> MatchingEngine::get_snapshot(const std::unordered_map<int64_t, std::list<BookEntry>>& um) const { 
    std::vector<PriceLevel> res;
    for (const auto& kv : um) {
        if (kv.second.empty()) continue;
        uint32_t total_qty = 0;
        for (const auto& entry : kv.second) {
            total_qty += entry.quantity;
        }
        res.push_back(PriceLevel{kv.first, total_qty, static_cast<uint32_t>(kv.second.size())});
    }
    return res;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(const std::string& symbol, Side side) const {
    if (bids.find(symbol) == bids.end()) return {};

    std::vector<PriceLevel> book;
    if (side == Side::Buy) 
        book = get_snapshot(bids.at(symbol));
    else
        book = get_snapshot(asks.at(symbol));

    std::sort(book.begin(), book.end(), [&](const PriceLevel& a, const PriceLevel& b) {
        return (side == Side::Buy) ? (a.price > b.price) : (a.price < b.price);
    });
    
    return book;
}

uint64_t MatchingEngine::getOrderCount() const {
    return order_count;
}

} // namespace exchange
