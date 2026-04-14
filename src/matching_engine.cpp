#include "exchange/matching_engine.h"

#include <algorithm>
#include <list>
#include <map>
#include <unordered_map>

// ── Internal data structures ──────────────────────────────────────────────────

namespace {
using namespace exchange;

struct Order {
    uint64_t id;
    Side     side;
    int64_t  price;
    uint32_t quantity;
};

using Level = std::list<Order>;          // FIFO queue at one price level

struct Book {
    std::map<int64_t, Level, std::greater<int64_t>> bids;  // highest price first
    std::map<int64_t, Level>                        asks;  // lowest  price first
};

struct Loc {
    std::string     symbol;
    Side            side;
    int64_t         price;
    Level::iterator it;
};

struct State {
    std::unordered_map<std::string, Book> books;
    std::unordered_map<uint64_t,   Loc>  orders;   // id → location in book
    uint64_t                             count = 0;
};

std::unordered_map<MatchingEngine*, State> g_states;

// ── Helpers ───────────────────────────────────────────────────────────────────

// Insert an order at the back of its price level and record its location.
void insertResting(Book& book, State& st,
                   uint64_t id, const std::string& sym,
                   Side side, int64_t price, uint32_t qty)
{
    auto go = [&](auto& levels) {
        auto& q = levels[price];
        q.push_back({id, side, price, qty});
        st.orders[id] = {sym, side, price, std::prev(q.end())};
        ++st.count;
    };
    if (side == Side::Buy) go(book.bids);
    else                   go(book.asks);
}

// Remove a resting order from its price level.
void eraseResting(Book& book, State& st, const Loc& loc)
{
    auto go = [&](auto& levels) {
        auto& q = levels[loc.price];
        q.erase(loc.it);
        if (q.empty()) levels.erase(loc.price);
    };
    if (loc.side == Side::Buy) go(book.bids);
    else                       go(book.asks);
    --st.count;
}

// Match an incoming order against the opposite side of the book.
// Fires onTrade + onOrderUpdate for each fill. Returns remaining quantity.
uint32_t doMatch(uint64_t id, const std::string& sym, Side side,
                 int64_t price, uint32_t qty, Book& book, State& st, Listener* L)
{
    auto sweep = [&](auto& levels, auto crosses) {
        for (auto lvl = levels.begin(); lvl != levels.end() && qty > 0 && crosses(lvl->first); ) {
            auto& q = lvl->second;
            while (!q.empty() && qty > 0) {
                auto& r    = q.front();
                uint32_t f = std::min(qty, r.quantity);

                if (side == Side::Buy) L->onTrade({id,   r.id, sym, r.price, f});
                else                   L->onTrade({r.id, id,   sym, r.price, f});

                qty        -= f;
                r.quantity -= f;

                if (r.quantity == 0) {
                    L->onOrderUpdate({r.id, OrderStatus::Filled, 0});
                    st.orders.erase(r.id);
                    q.pop_front();
                    --st.count;
                } else {
                    L->onOrderUpdate({r.id, OrderStatus::Accepted, r.quantity});
                }
            }
            lvl = q.empty() ? levels.erase(lvl) : std::next(lvl);
        }
    };

    if (side == Side::Buy) sweep(book.asks, [price](int64_t p){ return p <= price; });
    else                   sweep(book.bids, [price](int64_t p){ return p >= price; });
    return qty;
}

} // anonymous namespace

// ── MatchingEngine ────────────────────────────────────────────────────────────

namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
    // TODO: Initialize your data structures here.
    g_states[this]; // create per-instance state
}

MatchingEngine::~MatchingEngine() {
    // TODO: Clean up if necessary.
    g_states.erase(this);
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    // Step 1: Validate the request.
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty())
        return {0, OrderStatus::Rejected};

    // Step 2: Assign an order ID.
    uint64_t id = next_order_id_++;

    // TODO: Step 3: Look up (or create) the order book for request.symbol.
    //
    //   Hint: You need a per-symbol data structure that maintains
    //   buy orders (bids) and sell orders (asks) separately.
    auto& st   = g_states[this];
    auto& book = st.books[request.symbol];

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
    uint32_t rem = doMatch(id, request.symbol, request.side,
                           request.price, request.quantity, book, st, listener_);

    // TODO: Step 5: If quantity remains, insert into the book.
    // TODO: Step 6: Call listener_->onOrderUpdate() for the incoming order.
    //   - status = Filled     if fully matched (remaining == 0)
    //   - status = Accepted   if resting on the book (remaining > 0)
    // TODO: Step 7: Return the ack.
    //   - status = Filled   if fully matched
    //   - status = Accepted if resting (including partial fills)
    if (rem > 0) {
        insertResting(book, st, id, request.symbol, request.side, request.price, rem);
        listener_->onOrderUpdate({id, OrderStatus::Accepted, rem});
        return {id, OrderStatus::Accepted};
    }
    listener_->onOrderUpdate({id, OrderStatus::Filled, 0});
    return {id, OrderStatus::Filled};
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
    auto& st = g_states[this];
    auto  it = st.orders.find(order_id);
    if (it == st.orders.end()) return false;

    eraseResting(st.books[it->second.symbol], st, it->second);
    st.orders.erase(it);

    listener_->onOrderUpdate({order_id, OrderStatus::Cancelled, 0});
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    // Validate parameters.
    if (new_price <= 0 || new_quantity == 0) return false;

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
    auto& st = g_states[this];
    auto  it = st.orders.find(order_id);
    if (it == st.orders.end()) return false;

    // Determine if time priority is lost.
    //   Only quantity decreased (price unchanged) → keeps priority.
    //   Price changed or quantity increased       → loses priority.
    const Loc  loc     = it->second;              // copy before erasing
    const bool keep    = (new_price == loc.price) && (new_quantity <= loc.it->quantity);

    if (keep) {
        loc.it->quantity = new_quantity;          // update in-place
        return true;
    }

    // Remove and re-insert at the back of the level (lost priority).
    auto& book = st.books[loc.symbol];
    eraseResting(book, st, loc);
    st.orders.erase(it);

    // If the new price could cause a match, run matching logic.
    uint32_t rem = doMatch(order_id, loc.symbol, loc.side,
                           new_price, new_quantity, book, st, listener_);

    if (rem > 0) {
        insertResting(book, st, order_id, loc.symbol, loc.side, new_price, rem);
        listener_->onOrderUpdate({order_id, OrderStatus::Accepted, rem});
    } else {
        listener_->onOrderUpdate({order_id, OrderStatus::Filled, 0});
    }
    return true;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    auto& st = g_states[const_cast<MatchingEngine*>(this)];
    auto  it = st.books.find(symbol);
    if (it == st.books.end()) return {};

    // TODO: Build a vector of PriceLevel for the requested side.
    //
    // Sort best-to-worst:
    //   Buy side:  highest price first
    //   Sell side: lowest price first
    std::vector<PriceLevel> result;
    auto aggregate = [&](const auto& levels) {
        for (const auto& [price, queue] : levels) {
            uint32_t qty = 0, cnt = 0;
            for (const auto& o : queue) { qty += o.quantity; ++cnt; }
            if (cnt > 0) result.push_back({price, qty, cnt});
        }
    };
    if (side == Side::Buy) aggregate(it->second.bids);
    else                   aggregate(it->second.asks);
    return result;
}

uint64_t MatchingEngine::getOrderCount() const {
    // TODO: Return total number of resting orders across all symbols.
    return g_states[const_cast<MatchingEngine*>(this)].count;
}

} // namespace exchange
