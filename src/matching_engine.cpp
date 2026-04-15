#include "exchange/matching_engine.h"

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exchange {

namespace {

struct InternalOrder {
    uint64_t order_id;
    std::string symbol;
    Side side;
    int64_t price;
    uint32_t quantity;
};

using OrderList = std::list<InternalOrder>;

struct Book {
    std::map<int64_t, OrderList, std::greater<int64_t>> bids;  // highest price first
    std::map<int64_t, OrderList> asks;                         // lowest price first
};

struct OrderLocation {
    std::string symbol;
    Side side;
    int64_t price;
    OrderList::iterator it;
};

struct EngineState {
    std::unordered_map<std::string, Book> books;
    std::unordered_map<uint64_t, OrderLocation> locations;
    uint64_t resting_order_count = 0;
};

static std::unordered_map<const MatchingEngine*, EngineState> g_engine_states;

static void insertRestingOrder(
    EngineState& state,
    uint64_t order_id,
    const std::string& symbol,
    Side side,
    int64_t price,
    uint32_t quantity)
{
    Book& book = state.books[symbol];

    if (side == Side::Buy) {
        auto level_it = book.bids.try_emplace(price, OrderList{}).first;
        level_it->second.push_back(InternalOrder{order_id, symbol, side, price, quantity});
        auto order_it = std::prev(level_it->second.end());
        state.locations[order_id] = OrderLocation{symbol, side, price, order_it};
    } else {
        auto level_it = book.asks.try_emplace(price, OrderList{}).first;
        level_it->second.push_back(InternalOrder{order_id, symbol, side, price, quantity});
        auto order_it = std::prev(level_it->second.end());
        state.locations[order_id] = OrderLocation{symbol, side, price, order_it};
    }

    ++state.resting_order_count;
}

static uint32_t matchIncoming(
    EngineState& state,
    Listener* listener,
    uint64_t incoming_order_id,
    const std::string& symbol,
    Side incoming_side,
    int64_t incoming_price,
    uint32_t incoming_quantity)
{
    auto book_it = state.books.find(symbol);
    if (book_it == state.books.end()) {
        return incoming_quantity;
    }

    Book& book = book_it->second;
    uint32_t remaining = incoming_quantity;

    if (incoming_side == Side::Buy) {
        auto level_it = book.asks.begin();

        while (remaining > 0 &&
               level_it != book.asks.end() &&
               level_it->first <= incoming_price) {
            auto& queue = level_it->second;

            while (remaining > 0 && !queue.empty()) {
                auto resting_it = queue.begin();
                const uint32_t fill_qty = std::min(remaining, resting_it->quantity);
                const int64_t trade_price = resting_it->price;
                const uint64_t sell_order_id = resting_it->order_id;

                if (listener != nullptr) {
                    listener->onTrade(
                        Trade{incoming_order_id, sell_order_id, symbol, trade_price, fill_qty});
                }

                remaining -= fill_qty;
                resting_it->quantity -= fill_qty;

                const OrderStatus resting_status =
                    (resting_it->quantity == 0) ? OrderStatus::Filled : OrderStatus::Accepted;

                if (listener != nullptr) {
                    listener->onOrderUpdate(
                        OrderUpdate{sell_order_id, resting_status, resting_it->quantity});
                }

                if (resting_it->quantity == 0) {
                    state.locations.erase(sell_order_id);
                    queue.erase(resting_it);
                    --state.resting_order_count;
                }
            }

            if (queue.empty()) {
                auto erase_it = level_it++;
                book.asks.erase(erase_it);
            } else {
                break;
            }
        }
    } else {
        auto level_it = book.bids.begin();

        while (remaining > 0 &&
               level_it != book.bids.end() &&
               level_it->first >= incoming_price) {
            auto& queue = level_it->second;

            while (remaining > 0 && !queue.empty()) {
                auto resting_it = queue.begin();
                const uint32_t fill_qty = std::min(remaining, resting_it->quantity);
                const int64_t trade_price = resting_it->price;
                const uint64_t buy_order_id = resting_it->order_id;

                if (listener != nullptr) {
                    listener->onTrade(
                        Trade{buy_order_id, incoming_order_id, symbol, trade_price, fill_qty});
                }

                remaining -= fill_qty;
                resting_it->quantity -= fill_qty;

                const OrderStatus resting_status =
                    (resting_it->quantity == 0) ? OrderStatus::Filled : OrderStatus::Accepted;

                if (listener != nullptr) {
                    listener->onOrderUpdate(
                        OrderUpdate{buy_order_id, resting_status, resting_it->quantity});
                }

                if (resting_it->quantity == 0) {
                    state.locations.erase(buy_order_id);
                    queue.erase(resting_it);
                    --state.resting_order_count;
                }
            }

            if (queue.empty()) {
                auto erase_it = level_it++;
                book.bids.erase(erase_it);
            } else {
                break;
            }
        }
    }

    return remaining;
}

}  // namespace

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
    // TODO: Initialize your data structures here.
    g_engine_states.try_emplace(this);
}

MatchingEngine::~MatchingEngine() {
    // TODO: Clean up if necessary.
    g_engine_states.erase(this);
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    // Step 1: Validate the request.
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) {
        return OrderAck{0, OrderStatus::Rejected};
    }

    // Step 2: Assign an order ID.
    uint64_t order_id = next_order_id_++;

    EngineState& state = g_engine_states[this];

    // TODO: Step 3: Look up (or create) the order book for request.symbol.
    //
    //   Hint: You need a per-symbol data structure that maintains
    //   buy orders (bids) and sell orders (asks) separately.
    state.books.try_emplace(request.symbol, Book{});

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
    uint32_t remaining = matchIncoming(
        state,
        listener_,
        order_id,
        request.symbol,
        request.side,
        request.price,
        request.quantity);

    // TODO: Step 5: If quantity remains, insert into the book.
    if (remaining > 0) {
        insertRestingOrder(
            state,
            order_id,
            request.symbol,
            request.side,
            request.price,
            remaining);
    }

    // TODO: Step 6: Call listener_->onOrderUpdate() for the incoming order.
    //   - status = Filled     if fully matched (remaining == 0)
    //   - status = Accepted   if resting on the book (remaining > 0)
    const OrderStatus final_status =
        (remaining == 0) ? OrderStatus::Filled : OrderStatus::Accepted;

    if (listener_ != nullptr) {
        listener_->onOrderUpdate(OrderUpdate{order_id, final_status, remaining});
    }

    // TODO: Step 7: Return the ack.
    //   - status = Filled   if fully matched
    //   - status = Accepted if resting (including partial fills)
    return OrderAck{order_id, final_status};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    EngineState& state = g_engine_states[this];

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
    auto loc_it = state.locations.find(order_id);
    if (loc_it == state.locations.end()) {
        return false;
    }

    const OrderLocation loc = loc_it->second;
    auto book_it = state.books.find(loc.symbol);
    if (book_it == state.books.end()) {
        return false;
    }

    Book& book = book_it->second;

    if (loc.side == Side::Buy) {
        auto level_it = book.bids.find(loc.price);
        if (level_it == book.bids.end()) {
            return false;
        }

        level_it->second.erase(loc.it);
        if (level_it->second.empty()) {
            book.bids.erase(level_it);
        }
    } else {
        auto level_it = book.asks.find(loc.price);
        if (level_it == book.asks.end()) {
            return false;
        }

        level_it->second.erase(loc.it);
        if (level_it->second.empty()) {
            book.asks.erase(level_it);
        }
    }

    state.locations.erase(loc_it);
    --state.resting_order_count;

    if (listener_ != nullptr) {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});
    }

    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    // Validate parameters.
    if (new_price <= 0 || new_quantity == 0) {
        return false;
    }

    EngineState& state = g_engine_states[this];

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
    auto loc_it = state.locations.find(order_id);
    if (loc_it == state.locations.end()) {
        return false;
    }

    const OrderLocation old_loc = loc_it->second;
    auto& current_order = *(old_loc.it);

    const std::string symbol = current_order.symbol;
    const Side side = current_order.side;
    const int64_t old_price = current_order.price;
    const uint32_t old_quantity = current_order.quantity;

    const bool price_changed = (new_price != old_price);
    const bool quantity_increased = (new_quantity > old_quantity);
    const bool loses_priority = price_changed || quantity_increased;

    if (!loses_priority) {
        current_order.quantity = new_quantity;
        return true;
    }

    auto book_it = state.books.find(symbol);
    if (book_it == state.books.end()) {
        return false;
    }

    Book& book = book_it->second;

    if (side == Side::Buy) {
        auto level_it = book.bids.find(old_price);
        if (level_it == book.bids.end()) {
            return false;
        }

        level_it->second.erase(old_loc.it);
        if (level_it->second.empty()) {
            book.bids.erase(level_it);
        }
    } else {
        auto level_it = book.asks.find(old_price);
        if (level_it == book.asks.end()) {
            return false;
        }

        level_it->second.erase(old_loc.it);
        if (level_it->second.empty()) {
            book.asks.erase(level_it);
        }
    }

    state.locations.erase(loc_it);
    --state.resting_order_count;

    uint32_t remaining = matchIncoming(
        state,
        listener_,
        order_id,
        symbol,
        side,
        new_price,
        new_quantity);

    if (remaining > 0) {
        insertRestingOrder(state, order_id, symbol, side, new_price, remaining);
    }

    if (remaining < new_quantity && listener_ != nullptr) {
        const OrderStatus final_status =
            (remaining == 0) ? OrderStatus::Filled : OrderStatus::Accepted;
        listener_->onOrderUpdate(OrderUpdate{order_id, final_status, remaining});
    }

    return true;
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

    auto state_it = g_engine_states.find(this);
    if (state_it == g_engine_states.end()) {
        return snapshot;
    }

    const EngineState& state = state_it->second;
    auto book_it = state.books.find(symbol);
    if (book_it == state.books.end()) {
        return snapshot;
    }

    const Book& book = book_it->second;

    if (side == Side::Buy) {
        for (const auto& [price, orders] : book.bids) {
            uint32_t total_quantity = 0;
            uint32_t order_count = 0;

            for (const auto& order : orders) {
                total_quantity += order.quantity;
                ++order_count;
            }

            snapshot.push_back(PriceLevel{price, total_quantity, order_count});
        }
    } else {
        for (const auto& [price, orders] : book.asks) {
            uint32_t total_quantity = 0;
            uint32_t order_count = 0;

            for (const auto& order : orders) {
                total_quantity += order.quantity;
                ++order_count;
            }

            snapshot.push_back(PriceLevel{price, total_quantity, order_count});
        }
    }

    return snapshot;
}

uint64_t MatchingEngine::getOrderCount() const {
    // TODO: Return total number of resting orders across all symbols.
    auto state_it = g_engine_states.find(this);
    if (state_it == g_engine_states.end()) {
        return 0;
    }

    return state_it->second.resting_order_count;
}

}  // namespace exchange