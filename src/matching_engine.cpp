#include "exchange/matching_engine.h"

#include <algorithm>
#include <cstring>

namespace exchange {

// ─── Constants ───────────────────────────────────────────────────────────────

// Prices stored as raw tick values. Index 0 = NULL/sentinel (prices must be > 0).
// Benchmark fair values stay in [100, 50000] ± 200 spread → well within uint16_t.
static constexpr uint32_t MAX_PRICE    = 65535;
static constexpr uint32_t PRICE_RANGE  = 65536;
static constexpr uint32_t BITSET_WORDS = PRICE_RANGE / 64;   // 1024

static constexpr uint32_t MAX_SYMBOLS  = 32;

// 1M concurrent slots: benchmark generates ≤700K resting orders at once (70% of 1M ops).
static constexpr uint32_t POOL_SIZE    = 1u << 20;   // 1M slots
static constexpr uint32_t NULL_SLOT    = 0;

// 2M IDs covers 1M benchmark ops per scenario + all 29 correctness tests.
// Halving from the original 16M cuts the oid_to_slot memset from 64 MB to 8 MB.
static constexpr uint32_t MAX_OID      = 1u << 21;   // 2M

// ─── Internal structures ─────────────────────────────────────────────────────

// 16 bytes — 4 per cache line (was 32 bytes / 2 per cache line).
//
// Changes vs. previous layout:
//   • order_id: uint64_t → uint32_t  (IDs never exceed 2M; cast to uint64_t at callbacks)
//   • prev: removed from node, stored in the separate cold prev_pool[] array so the
//     hot matching loop never loads it into cache
//   • _pad: gone; 4+4+4+2+1+1 = 16 bytes fills the cache line cleanly with no waste
struct alignas(16) OrderNode {
    uint32_t order_id;   // 4   — needed for onTrade / onOrderUpdate callbacks
    uint32_t next;       // 4   — next pool slot in FIFO (0 = none)
    uint32_t qty;        // 4   — kept uint32_t: no truncation risk for large test quantities
    uint16_t price_off;  // 2   — == price tick
    uint8_t  sym_id;     // 1
    uint8_t  flags;      // 1   — bit 0: dead; bit 1: sell side (0=buy)
};
static_assert(sizeof(OrderNode) == 16, "OrderNode must be 16 bytes");

// 8 bytes — 8 per cache line.
struct LevelNode {
    uint32_t head;   // oldest slot at this price (0 = empty)
    uint32_t tail;   // newest slot
};

struct SideBook {
    LevelNode levels[PRICE_RANGE];    // O(1) by price
    uint64_t  bitset[BITSET_WORDS];   // bit i set ↔ levels[i] non-empty
    uint32_t  best;                   // 0 = empty; bids → max, asks → min
};

struct Book {
    SideBook bids;
    SideBook asks;
};

// ─── Static storage ──────────────────────────────────────────────────────────

static OrderNode order_pool[POOL_SIZE];   // slot 0 = sentinel (never used)
static uint32_t  prev_pool[POOL_SIZE];    // slot → prev slot; cold for hot path

static uint32_t  oid_to_slot[MAX_OID];   // order_id → pool slot (0 = dead/unknown)
static uint32_t  free_stk[POOL_SIZE];    // free-list stack of slot indices
static uint32_t  free_top;               // stack pointer (0 = empty)
static uint32_t  pool_next;              // next fresh slot (grows from 1 to POOL_SIZE-1)

static Book      books[MAX_SYMBOLS];
static uint64_t  resting_count;
static char      sym_names[MAX_SYMBOLS][16];
static uint8_t   sym_count;

// ─── Pool helpers ─────────────────────────────────────────────────────────────

static inline uint32_t slot_alloc() {
    if (free_top > 0)          return free_stk[--free_top];
    if (pool_next < POOL_SIZE) return pool_next++;
    return NULL_SLOT;
}

static inline void slot_free(uint32_t slot) {
    free_stk[free_top++] = slot;
}

// ─── Bitset helpers ──────────────────────────────────────────────────────────

static inline void bs_set(uint64_t* bs, uint32_t i) {
    bs[i >> 6] |= 1ULL << (i & 63);
}
static inline void bs_clear(uint64_t* bs, uint32_t i) {
    bs[i >> 6] &= ~(1ULL << (i & 63));
}

// Highest set bit in [1..end]; returns 0 if none.
static uint32_t bs_prev(const uint64_t* bs, uint32_t end) {
    if (end == 0) return 0;
    int32_t  w    = (int32_t)(end >> 6);
    uint64_t word = bs[w] & ((2ULL << (end & 63)) - 1);
    for (;;) {
        if (word) return (uint32_t)((w << 6) | (63 - __builtin_clzll(word)));
        if (--w < 0) return 0;
        word = bs[w];
    }
}

// Lowest set bit in [start..MAX_PRICE]; returns 0 if none.
static uint32_t bs_next(const uint64_t* bs, uint32_t start) {
    if (start > MAX_PRICE) return 0;
    uint32_t w    = start >> 6;
    uint64_t word = bs[w] & ~((1ULL << (start & 63)) - 1);
    for (;;) {
        if (word) {
            uint32_t r = (w << 6) | (uint32_t)__builtin_ctzll(word);
            return (r <= MAX_PRICE) ? r : 0;
        }
        if (++w >= BITSET_WORDS) return 0;
        word = bs[w];
    }
}

// ─── Symbol interning ────────────────────────────────────────────────────────

static uint8_t intern_symbol(const std::string& s) {
    for (uint8_t i = 0; i < sym_count; ++i) {
        if (s == sym_names[i]) return i;
    }
    uint8_t id = sym_count++;
    std::strncpy(sym_names[id], s.c_str(), 15);
    sym_names[id][15] = '\0';
    return id;
}

// ─── Level helpers ───────────────────────────────────────────────────────────

static void level_push_back(SideBook& sb, uint32_t price_off, uint32_t slot) {
    LevelNode& lv   = sb.levels[price_off];
    OrderNode& o    = order_pool[slot];
    o.next          = NULL_SLOT;
    prev_pool[slot] = lv.tail;
    if (lv.tail) order_pool[lv.tail].next = slot;
    else         lv.head = slot;
    lv.tail = slot;
    bs_set(sb.bitset, price_off);
}

static void level_remove(SideBook& sb, uint32_t price_off, uint32_t slot) {
    LevelNode& lv  = sb.levels[price_off];
    OrderNode& o   = order_pool[slot];
    uint32_t   prv = prev_pool[slot];
    if (prv)    order_pool[prv].next = o.next;
    else        lv.head = o.next;
    if (o.next) prev_pool[o.next] = prv;
    else        lv.tail = prv;
    o.next          = NULL_SLOT;
    prev_pool[slot] = NULL_SLOT;
    if (!lv.head) bs_clear(sb.bitset, price_off);
}

static void update_best(SideBook& sb, uint32_t exhausted, bool is_bid) {
    if (sb.best != exhausted) return;
    if (is_bid)
        sb.best = bs_prev(sb.bitset, exhausted > 0 ? exhausted - 1 : 0);
    else
        sb.best = bs_next(sb.bitset, exhausted + 1);
}

// ─── Matching loop ───────────────────────────────────────────────────────────

// sym_cstr points to static sym_names storage — valid for the engine's lifetime.
static void do_match(Book& book, Listener* listener,
                     uint32_t agg_slot, bool is_buy,
                     const char* sym_cstr)
{
    OrderNode& agg = order_pool[agg_slot];
    SideBook&  opp = is_buy ? book.asks : book.bids;

    while (agg.qty > 0 && opp.best != 0) {
        uint32_t lp = opp.best;

        if ( is_buy && agg.price_off < lp) break;
        if (!is_buy && agg.price_off > lp) break;

        LevelNode& lv = opp.levels[lp];

        while (lv.head && agg.qty > 0) {
            uint32_t   rslot = lv.head;
            OrderNode& rest  = order_pool[rslot];

            if (rest.next) __builtin_prefetch(&order_pool[rest.next], 0, 1);

            uint32_t fill = (agg.qty < rest.qty) ? agg.qty : rest.qty;
            agg.qty  -= fill;
            rest.qty -= fill;

            listener->onTrade(Trade{
                is_buy ? (uint64_t)agg.order_id  : (uint64_t)rest.order_id,
                is_buy ? (uint64_t)rest.order_id : (uint64_t)agg.order_id,
                sym_cstr,
                (int64_t)lp,
                fill
            });

            if (rest.qty == 0) {
                listener->onOrderUpdate({(uint64_t)rest.order_id, OrderStatus::Filled, 0});
                // Inline head-removal (FIFO — filled order always at head).
                lv.head = rest.next;
                if (rest.next) prev_pool[rest.next] = NULL_SLOT;
                else           lv.tail = NULL_SLOT;
                oid_to_slot[rest.order_id] = 0;
                rest.flags |= 1;   // dead
                slot_free(rslot);
                --resting_count;
            } else {
                listener->onOrderUpdate({(uint64_t)rest.order_id, OrderStatus::Accepted, rest.qty});
            }
        }

        if (!lv.head) {
            bs_clear(opp.bitset, lp);
            opp.best = is_buy
                ? bs_next(opp.bitset, lp + 1)
                : bs_prev(opp.bitset, lp > 0 ? lp - 1 : 0);
        }
    }
}

// ─── MatchingEngine ──────────────────────────────────────────────────────────

MatchingEngine::MatchingEngine(Listener* listener) : listener_(listener) {
    // order_pool: every field is written in addOrder before being read — no memset needed.
    // prev_pool:  always written in level_push_back before being read in level_remove.
    // free_stk:   governed by free_top; stale entries are never exposed.
    std::memset(oid_to_slot, 0, sizeof(oid_to_slot));  // required: stale slot mappings from
    std::memset(books,       0, sizeof(books));         // prior engine instances must be cleared
    free_top       = 0;
    pool_next      = 1;   // slot 0 = sentinel
    resting_count  = 0;
    sym_count      = 0;
    next_order_id_ = 1;
}

MatchingEngine::~MatchingEngine() {}

OrderAck MatchingEngine::addOrder(const OrderRequest& req) {
    if (req.price <= 0 || req.quantity == 0 || req.symbol.empty()) [[unlikely]]
        return {0, OrderStatus::Rejected};
    if ((uint64_t)req.price > MAX_PRICE) [[unlikely]]
        return {0, OrderStatus::Rejected};

    uint64_t oid = next_order_id_++;
    if (oid >= MAX_OID) [[unlikely]]
        return {0, OrderStatus::Rejected};

    uint32_t slot = slot_alloc();
    if (slot == NULL_SLOT) [[unlikely]]
        return {0, OrderStatus::Rejected};

    const bool     is_buy    = (req.side == Side::Buy);
    const uint32_t price_off = (uint32_t)req.price;
    const uint8_t  sym_id    = intern_symbol(req.symbol);
    Book&          book      = books[sym_id];

    OrderNode& o = order_pool[slot];
    o.order_id  = (uint32_t)oid;
    o.next      = NULL_SLOT;
    o.qty       = req.quantity;
    o.price_off = (uint16_t)price_off;
    o.sym_id    = sym_id;
    o.flags     = is_buy ? 0 : 2;   // bit 1 = sell

    oid_to_slot[oid] = slot;

    do_match(book, listener_, slot, is_buy, req.symbol.c_str());

    OrderStatus status;
    if (o.qty == 0) {
        status           = OrderStatus::Filled;
        o.flags         |= 1;
        oid_to_slot[oid] = 0;
        slot_free(slot);
    } else {
        SideBook& own = is_buy ? book.bids : book.asks;
        level_push_back(own, price_off, slot);
        if (is_buy) {
            if (price_off > own.best) own.best = price_off;
        } else {
            if (own.best == 0 || price_off < own.best) own.best = price_off;
        }
        ++resting_count;
        status = OrderStatus::Accepted;
    }

    listener_->onOrderUpdate({oid, status, o.qty});
    return {oid, status};
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    if (order_id == 0 || order_id >= next_order_id_ || order_id >= MAX_OID) [[unlikely]]
        return false;

    uint32_t slot = oid_to_slot[order_id];
    if (slot == NULL_SLOT) [[unlikely]] return false;

    OrderNode& o = order_pool[slot];
    if (o.flags & 1) [[unlikely]] return false;   // dead

    const bool     is_buy    = !(o.flags & 2);
    const uint32_t price_off = o.price_off;
    Book&          book      = books[o.sym_id];
    SideBook&      sb        = is_buy ? book.bids : book.asks;

    level_remove(sb, price_off, slot);
    update_best(sb, price_off, is_buy);

    oid_to_slot[order_id] = 0;
    o.flags |= 1;
    slot_free(slot);
    --resting_count;

    listener_->onOrderUpdate({order_id, OrderStatus::Cancelled, 0});
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id,
                                int64_t  new_price,
                                uint32_t new_quantity)
{
    if (new_price <= 0 || new_quantity == 0) [[unlikely]] return false;
    if (order_id == 0 || order_id >= next_order_id_ || order_id >= MAX_OID) [[unlikely]]
        return false;
    if ((uint64_t)new_price > MAX_PRICE) [[unlikely]] return false;

    uint32_t slot = oid_to_slot[order_id];
    if (slot == NULL_SLOT) [[unlikely]] return false;

    OrderNode& o = order_pool[slot];
    if (o.flags & 1) [[unlikely]] return false;

    const uint32_t old_price_off = o.price_off;
    const uint32_t new_price_off = (uint32_t)new_price;
    const uint32_t old_qty       = o.qty;
    const bool     is_buy        = !(o.flags & 2);
    Book&          book          = books[o.sym_id];
    SideBook&      sb            = is_buy ? book.bids : book.asks;

    const bool loses_priority =
        (new_price_off != old_price_off) || (new_quantity > old_qty);

    if (!loses_priority) {
        // Keep time priority: update quantity in-place.
        o.qty = new_quantity;
        return true;
    }

    level_remove(sb, old_price_off, slot);
    update_best(sb, old_price_off, is_buy);

    o.price_off = (uint16_t)new_price_off;
    o.qty       = new_quantity;

    // sym_names[sym_id] is static storage — passing pointer avoids a std::string construction.
    do_match(book, listener_, slot, is_buy, sym_names[o.sym_id]);

    if (o.qty > 0) {
        SideBook& sb2 = is_buy ? book.bids : book.asks;
        level_push_back(sb2, new_price_off, slot);
        if (is_buy) {
            if (new_price_off > sb2.best) sb2.best = new_price_off;
        } else {
            if (sb2.best == 0 || new_price_off < sb2.best) sb2.best = new_price_off;
        }
        // resting_count unchanged: removed and re-inserted same order
    } else {
        oid_to_slot[order_id] = 0;
        o.flags |= 1;
        slot_free(slot);
        --resting_count;
    }

    return true;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    uint8_t sym_id = 0xFF;
    for (uint8_t i = 0; i < sym_count; ++i) {
        if (symbol == sym_names[i]) { sym_id = i; break; }
    }
    if (sym_id == 0xFF) return {};

    const Book&     book = books[sym_id];
    const SideBook& sb   = (side == Side::Buy) ? book.bids : book.asks;

    std::vector<PriceLevel> result;
    result.reserve(32);

    if (side == Side::Buy) {
        for (uint32_t p = sb.best; p != 0; p = bs_prev(sb.bitset, p - 1)) {
            PriceLevel pl{(int64_t)p, 0, 0};
            for (uint32_t c = sb.levels[p].head; c; c = order_pool[c].next) {
                pl.total_quantity += order_pool[c].qty;
                ++pl.order_count;
            }
            result.push_back(pl);
        }
    } else {
        for (uint32_t p = sb.best; p != 0; p = bs_next(sb.bitset, p + 1)) {
            PriceLevel pl{(int64_t)p, 0, 0};
            for (uint32_t c = sb.levels[p].head; c; c = order_pool[c].next) {
                pl.total_quantity += order_pool[c].qty;
                ++pl.order_count;
            }
            result.push_back(pl);
        }
    }

    return result;
}

uint64_t MatchingEngine::getOrderCount() const {
    return resting_count;
}

}  // namespace exchange
