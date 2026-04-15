#include "exchange/matching_engine.h"

#include <map>
#include <vector>

namespace exchange {

namespace {

// ============================================================
//  Symbol interning — convert strings to small ints
// ============================================================
constexpr int MAX_SYM = 64;
int g_nsym;
std::string g_sym_names[MAX_SYM];

inline int intern(const std::string& s) {
    for (int i = 0; i < g_nsym; i++)
        if (g_sym_names[i] == s) return i;
    g_sym_names[g_nsym] = s;
    return g_nsym++;
}

// ============================================================
//  Order — flat vector indexed by order_id, no heap per order
// ============================================================
struct Order {
    uint32_t remaining;  // hot: used in matching loop
    uint32_t next;       // hot: FIFO traversal (order_id, 0=none)
    uint32_t prev;       // warm: unlink
    int16_t  sym;        // cold
    Side     side;       // cold
    bool     active;     // cold
    int64_t  price;      // warm: used in level lookup
};

static std::vector<Order> g_ord;  // index 0 unused; g_ord[order_id]
static uint64_t g_cnt;            // active resting orders

// ============================================================
//  Price level — head/tail of intrusive doubly-linked list
// ============================================================
struct Level {
    uint32_t head;
    uint32_t tail;
};

// ============================================================
//  Per-symbol book
// ============================================================
struct Book {
    std::map<int64_t, Level, std::greater<int64_t>> bids; // highest first
    std::map<int64_t, Level> asks;                         // lowest first
};

static Book g_books[MAX_SYM];

// ============================================================
//  Helpers
// ============================================================

template<typename M>
inline void unlinkOrder(uint32_t id, M& levels) {
    Order& o = g_ord[id];
    auto it = levels.find(o.price);
    Level& lv = it->second;

    if (o.prev) g_ord[o.prev].next = o.next;
    else lv.head = o.next;

    if (o.next) g_ord[o.next].prev = o.prev;
    else lv.tail = o.prev;

    if (!lv.head) levels.erase(it);

    o.active = false;
    --g_cnt;
}

template<typename M>
inline void linkOrder(uint32_t id, M& levels) {
    Order& o = g_ord[id];
    auto [it, ins] = levels.try_emplace(o.price, Level{id, id});

    if (!ins) {
        Level& lv = it->second;
        o.prev = lv.tail;
        o.next = 0;
        g_ord[lv.tail].next = id;
        lv.tail = id;
    } else {
        o.prev = 0;
        o.next = 0;
    }

    o.active = true;
    ++g_cnt;
}

template<typename M>
void doMatch(uint32_t agg, Side agg_side, int sym,
             int64_t agg_price, uint32_t& rem,
             M& opp, Listener* L)
{
    const std::string& sn = g_sym_names[sym];

    while (rem && !opp.empty()) {
        auto it = opp.begin();
        int64_t lp = it->first;

        if (agg_side == Side::Buy ? lp > agg_price : lp < agg_price)
            break;

        Level& lv = it->second;

        while (rem && lv.head) {
            uint32_t rid = lv.head;
            Order& r = g_ord[rid];
            uint32_t fill = rem < r.remaining ? rem : r.remaining;

            uint64_t bid, sid;
            if (agg_side == Side::Buy) { bid = agg; sid = rid; }
            else { bid = rid; sid = agg; }

            L->onTrade(Trade{bid, sid, sn, lp, fill});

            rem -= fill;
            r.remaining -= fill;

            if (r.remaining == 0) {
                L->onOrderUpdate(OrderUpdate{(uint64_t)rid, OrderStatus::Filled, 0});
                uint32_t nx = r.next;
                r.active = false;
                --g_cnt;
                lv.head = nx;
                if (nx) g_ord[nx].prev = 0;
                else lv.tail = 0;
            } else {
                L->onOrderUpdate(OrderUpdate{(uint64_t)rid, OrderStatus::Accepted, r.remaining});
            }
        }

        if (!lv.head) opp.erase(it);
    }
}

} // anonymous namespace

// ============================================================
//  MatchingEngine
// ============================================================

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
    for (int i = 0; i < g_nsym; i++) {
        g_books[i].bids.clear();
        g_books[i].asks.clear();
    }
    g_nsym = 0;
    g_ord.clear();
    g_ord.reserve(8'000'000);
    g_ord.resize(1); // index 0 unused
    g_cnt = 0;
}

MatchingEngine::~MatchingEngine() {}

OrderAck MatchingEngine::addOrder(const OrderRequest& req) {
    if (__builtin_expect(req.price <= 0 || req.quantity == 0 || req.symbol.empty(), 0))
        return {0, OrderStatus::Rejected};

    uint32_t oid = static_cast<uint32_t>(next_order_id_++);
    int sym = intern(req.symbol);
    uint32_t rem = req.quantity;
    Book& bk = g_books[sym];

    if (req.side == Side::Buy)
        doMatch(oid, Side::Buy, sym, req.price, rem, bk.asks, listener_);
    else
        doMatch(oid, Side::Sell, sym, req.price, rem, bk.bids, listener_);

    // Store order in flat vector at index oid
    g_ord.push_back(Order{rem, 0, 0, static_cast<int16_t>(sym), req.side, false, req.price});

    if (rem == 0) {
        listener_->onOrderUpdate(OrderUpdate{oid, OrderStatus::Filled, 0});
        return {oid, OrderStatus::Filled};
    }

    if (req.side == Side::Buy)
        linkOrder(oid, bk.bids);
    else
        linkOrder(oid, bk.asks);

    listener_->onOrderUpdate(OrderUpdate{oid, OrderStatus::Accepted, rem});
    return {oid, OrderStatus::Accepted};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    uint32_t oid = static_cast<uint32_t>(order_id);
    if (__builtin_expect(oid >= g_ord.size(), 0)) return false;
    Order& o = g_ord[oid];
    if (__builtin_expect(!o.active, 0)) return false;

    Book& bk = g_books[o.sym];
    if (o.side == Side::Buy) unlinkOrder(oid, bk.bids);
    else unlinkOrder(oid, bk.asks);

    listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_qty) {
    if (__builtin_expect(new_price <= 0 || new_qty == 0, 0)) return false;

    uint32_t oid = static_cast<uint32_t>(order_id);
    if (__builtin_expect(oid >= g_ord.size(), 0)) return false;
    Order& o = g_ord[oid];
    if (__builtin_expect(!o.active, 0)) return false;

    bool loses = (new_price != o.price) || (new_qty > o.remaining);

    if (!loses) {
        o.remaining = new_qty;
        return true;
    }

    Side side = o.side;
    int sym = o.sym;
    Book& bk = g_books[sym];

    if (side == Side::Buy) unlinkOrder(oid, bk.bids);
    else unlinkOrder(oid, bk.asks);

    o.price = new_price;
    uint32_t rem = new_qty;

    if (side == Side::Buy)
        doMatch(oid, Side::Buy, sym, new_price, rem, bk.asks, listener_);
    else
        doMatch(oid, Side::Sell, sym, new_price, rem, bk.bids, listener_);

    if (rem == 0) {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Filled, 0});
    } else {
        o.remaining = rem;
        if (side == Side::Buy) linkOrder(oid, bk.bids);
        else linkOrder(oid, bk.asks);
    }

    return true;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    int sym = -1;
    for (int i = 0; i < g_nsym; i++)
        if (g_sym_names[i] == symbol) { sym = i; break; }
    if (sym < 0) return {};

    const Book& bk = g_books[sym];
    std::vector<PriceLevel> result;

    auto snap = [&](const auto& levels) {
        for (const auto& [price, lv] : levels) {
            uint32_t qty = 0, cnt = 0;
            uint32_t cur = lv.head;
            while (cur) {
                qty += g_ord[cur].remaining;
                ++cnt;
                cur = g_ord[cur].next;
            }
            result.push_back({price, qty, cnt});
        }
    };

    if (side == Side::Buy) snap(bk.bids);
    else snap(bk.asks);
    return result;
}

uint64_t MatchingEngine::getOrderCount() const {
    return g_cnt;
}

}  // namespace exchange
