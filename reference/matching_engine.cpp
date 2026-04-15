#include "exchange/matching_engine.h"
#include <algorithm>
#include <map>

namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
}

MatchingEngine::~MatchingEngine() = default;

uint32_t MatchingEngine::matchOrder(
    uint64_t order_id, const std::string& symbol,
    Side side, int64_t price, uint32_t quantity)
{
    auto& book = orders_[symbol];
    uint32_t remaining = quantity;

    while (remaining > 0) {
        int best_idx = -1;
        for (int i = 0; i < static_cast<int>(book.size()); i++) {
            auto& o = book[i];
            if (o.side == side) continue;

            bool crosses = (side == Side::Buy)
                ? (o.price <= price)
                : (o.price >= price);
            if (!crosses) continue;

            if (best_idx == -1) {
                best_idx = i;
                continue;
            }

            bool better = (side == Side::Buy)
                ? (o.price < book[best_idx].price)
                : (o.price > book[best_idx].price);
            if (better) best_idx = i;
        }

        if (best_idx == -1) break;

        Order& resting = book[best_idx];
        uint32_t fill_qty = std::min(remaining, resting.remaining_quantity);

        uint64_t buy_id  = (side == Side::Buy) ? order_id : resting.order_id;
        uint64_t sell_id = (side == Side::Buy) ? resting.order_id : order_id;

        listener_->onTrade(Trade{buy_id, sell_id, symbol, resting.price, fill_qty});

        resting.remaining_quantity -= fill_qty;
        remaining -= fill_qty;

        if (resting.remaining_quantity == 0) {
            listener_->onOrderUpdate(OrderUpdate{resting.order_id, OrderStatus::Filled, 0});
            book.erase(book.begin() + best_idx);
        } else {
            listener_->onOrderUpdate(OrderUpdate{
                resting.order_id, OrderStatus::Accepted, resting.remaining_quantity});
        }
    }

    return remaining;
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) {
        return OrderAck{0, OrderStatus::Rejected};
    }

    uint64_t order_id = next_order_id_++;

    uint32_t remaining = matchOrder(
        order_id, request.symbol, request.side, request.price, request.quantity);

    if (remaining == 0) {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Filled, 0});
        return OrderAck{order_id, OrderStatus::Filled};
    }

    orders_[request.symbol].push_back(Order{
        order_id, request.symbol, request.side, request.price, remaining});

    listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Accepted, remaining});
    return OrderAck{order_id, OrderStatus::Accepted};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    for (auto& [symbol, book] : orders_) {
        for (size_t i = 0; i < book.size(); i++) {
            if (book[i].order_id == order_id) {
                book.erase(book.begin() + i);
                listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});
                return true;
            }
        }
    }
    return false;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    if (new_price <= 0 || new_quantity == 0) return false;

    // Find the order across all symbols
    for (auto& [symbol, book] : orders_) {
        for (int i = 0; i < static_cast<int>(book.size()); i++) {
            if (book[i].order_id != order_id) continue;

            Order old_order = book[i];
            bool price_changed = (new_price != old_order.price);
            bool qty_increased = (new_quantity > old_order.remaining_quantity);

            if (!price_changed && !qty_increased) {
                book[i].remaining_quantity = new_quantity;
                return true;
            }

            // Loses priority: remove, try to match, re-insert remainder at end
            book.erase(book.begin() + i);

            uint32_t remaining = matchOrder(
                order_id, old_order.symbol, old_order.side, new_price, new_quantity);

            if (remaining == 0) {
                listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Filled, 0});
                return true;
            }

            orders_[old_order.symbol].push_back(Order{
                order_id, old_order.symbol, old_order.side, new_price, remaining});

            return true;
        }
    }
    return false;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    auto it = orders_.find(symbol);
    if (it == orders_.end()) return {};

    std::map<int64_t, std::pair<uint32_t, uint32_t>> price_map;
    for (const auto& o : it->second) {
        if (o.side == side) {
            price_map[o.price].first  += o.remaining_quantity;
            price_map[o.price].second += 1;
        }
    }

    std::vector<PriceLevel> result;
    if (side == Side::Buy) {
        for (auto pit = price_map.rbegin(); pit != price_map.rend(); ++pit)
            result.push_back(PriceLevel{pit->first, pit->second.first, pit->second.second});
    } else {
        for (auto& [price, agg] : price_map)
            result.push_back(PriceLevel{price, agg.first, agg.second});
    }
    return result;
}

uint64_t MatchingEngine::getOrderCount() const {
    uint64_t count = 0;
    for (const auto& [symbol, book] : orders_)
        count += book.size();
    return count;
}

}  // namespace exchange
