#include "exchange/matching_engine.h"

#include <map>
#include <list>
#include <unordered_map>
#include <algorithm>

namespace exchange {

// ============================================================
//  Internal data structures (file-scope, reset in constructor)
// ============================================================

namespace {

struct InternalOrder {
    uint64_t    order_id;
    Side        side;
    int64_t     price;
    uint32_t    remaining;
    std::string symbol;
};

struct OrderBook {
    // asks: lowest price first (best ask = begin)
    std::map<int64_t, std::list<InternalOrder>> asks;
    // bids: highest price first (best bid = begin)
    std::map<int64_t, std::list<InternalOrder>, std::greater<int64_t>> bids;
};

struct OrderLocation {
    std::string symbol;
    Side        side;
    int64_t     price;
    std::list<InternalOrder>::iterator it;
};

std::unordered_map<std::string, OrderBook>     g_books;
std::unordered_map<uint64_t, OrderLocation>    g_order_lookup;

// Match an aggressive order against the opposite side of the book.
// Updates `remaining` in place. Emits Trade and OrderUpdate callbacks.
template<typename MapT>
void doMatch(uint64_t aggressor_id, Side aggressor_side,
             const std::string& symbol, int64_t aggressor_price,
             uint32_t& remaining, MapT& opposite_side,
             Listener* listener)
{
    while (remaining > 0 && !opposite_side.empty()) {
        auto level_it = opposite_side.begin();
        int64_t level_price = level_it->first;

        bool crosses = (aggressor_side == Side::Buy)
            ? (level_price <= aggressor_price)
            : (level_price >= aggressor_price);
        if (!crosses) break;

        auto& order_list = level_it->second;

        while (remaining > 0 && !order_list.empty()) {
            auto& resting = order_list.front();
            uint32_t fill_qty = std::min(remaining, resting.remaining);

            uint64_t buy_id  = (aggressor_side == Side::Buy)  ? aggressor_id : resting.order_id;
            uint64_t sell_id = (aggressor_side == Side::Sell) ? aggressor_id : resting.order_id;

            listener->onTrade(Trade{buy_id, sell_id, symbol, level_price, fill_qty});

            remaining -= fill_qty;
            resting.remaining -= fill_qty;

            if (resting.remaining == 0) {
                listener->onOrderUpdate(OrderUpdate{resting.order_id, OrderStatus::Filled, 0});
                g_order_lookup.erase(resting.order_id);
                order_list.pop_front();
            } else {
                listener->onOrderUpdate(OrderUpdate{resting.order_id, OrderStatus::Accepted, resting.remaining});
            }
        }

        if (order_list.empty()) {
            opposite_side.erase(level_it);
        }
    }
}

// Insert a resting order into the book and register it in the lookup map.
void restOrder(const InternalOrder& order, OrderBook& book) {
    if (order.side == Side::Buy) {
        auto& level = book.bids[order.price];
        level.push_back(order);
        g_order_lookup[order.order_id] = OrderLocation{
            order.symbol, order.side, order.price, std::prev(level.end())};
    } else {
        auto& level = book.asks[order.price];
        level.push_back(order);
        g_order_lookup[order.order_id] = OrderLocation{
            order.symbol, order.side, order.price, std::prev(level.end())};
    }
}

// Remove an order from the book by its location.
void removeFromBook(const OrderLocation& loc, OrderBook& book) {
    if (loc.side == Side::Buy) {
        auto level_it = book.bids.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) book.bids.erase(level_it);
    } else {
        auto level_it = book.asks.find(loc.price);
        level_it->second.erase(loc.it);
        if (level_it->second.empty()) book.asks.erase(level_it);
    }
}

} // anonymous namespace

// ============================================================
//  MatchingEngine implementation
// ============================================================

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
    g_books.clear();
    g_order_lookup.clear();
}

MatchingEngine::~MatchingEngine() {
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    // Validate
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) {
        return OrderAck{0, OrderStatus::Rejected};
    }

    uint64_t order_id = next_order_id_++;
    uint32_t remaining = request.quantity;
    OrderBook& book = g_books[request.symbol];

    // Match against opposite side
    if (request.side == Side::Buy) {
        doMatch(order_id, Side::Buy, request.symbol, request.price, remaining, book.asks, listener_);
    } else {
        doMatch(order_id, Side::Sell, request.symbol, request.price, remaining, book.bids, listener_);
    }

    // Final status
    if (remaining == 0) {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Filled, 0});
        return OrderAck{order_id, OrderStatus::Filled};
    }

    // Rest remainder on the book
    InternalOrder order{order_id, request.side, request.price, remaining, request.symbol};
    restOrder(order, book);
    listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Accepted, remaining});
    return OrderAck{order_id, OrderStatus::Accepted};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    auto it = g_order_lookup.find(order_id);
    if (it == g_order_lookup.end()) return false;

    auto& loc = it->second;
    auto& book = g_books[loc.symbol];

    removeFromBook(loc, book);
    g_order_lookup.erase(it);

    listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    if (new_price <= 0 || new_quantity == 0) return false;

    auto it = g_order_lookup.find(order_id);
    if (it == g_order_lookup.end()) return false;

    auto& loc = it->second;
    bool price_changed     = (new_price != loc.price);
    bool quantity_increased = (new_quantity > loc.it->remaining);
    bool loses_priority     = price_changed || quantity_increased;

    if (!loses_priority) {
        // Only quantity decreased — keep priority, update in-place
        loc.it->remaining = new_quantity;
        return true;
    }

    // Loses priority: remove, attempt match, re-insert remainder
    Side side = loc.side;
    std::string symbol = loc.symbol;
    auto& book = g_books[symbol];

    removeFromBook(loc, book);
    g_order_lookup.erase(it);

    uint32_t remaining = new_quantity;
    if (side == Side::Buy) {
        doMatch(order_id, Side::Buy, symbol, new_price, remaining, book.asks, listener_);
    } else {
        doMatch(order_id, Side::Sell, symbol, new_price, remaining, book.bids, listener_);
    }

    if (remaining == 0) {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Filled, 0});
    } else {
        InternalOrder order{order_id, side, new_price, remaining, symbol};
        restOrder(order, book);
    }

    return true;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    auto book_it = g_books.find(symbol);
    if (book_it == g_books.end()) return {};

    const auto& book = book_it->second;
    std::vector<PriceLevel> result;

    if (side == Side::Buy) {
        for (const auto& [price, orders] : book.bids) {
            uint32_t total_qty = 0;
            for (const auto& o : orders) total_qty += o.remaining;
            result.push_back(PriceLevel{price, total_qty, static_cast<uint32_t>(orders.size())});
        }
    } else {
        for (const auto& [price, orders] : book.asks) {
            uint32_t total_qty = 0;
            for (const auto& o : orders) total_qty += o.remaining;
            result.push_back(PriceLevel{price, total_qty, static_cast<uint32_t>(orders.size())});
        }
    }

    return result;
}

uint64_t MatchingEngine::getOrderCount() const {
    return g_order_lookup.size();
}

}  // namespace exchange
