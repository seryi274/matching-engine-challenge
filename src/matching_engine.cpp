#include "exchange/matching_engine.h"
#include <vector>
#include <string>
#include <iostream>
#include <array>

//test
namespace exchange {

namespace {

constexpr int64_t MAX_PRICE = 65536;

struct alignas(32) OrderNode {
    int64_t price;
    uint32_t qty;
    uint32_t prev;
    uint32_t next;
    uint16_t symbol_id;
    uint8_t side;
    uint8_t active;
};

struct PriceLevel_Data {
    uint32_t head = 0;
    uint32_t tail = 0;
    uint64_t total_qty = 0;
    uint32_t count = 0;
};

struct Book {
    PriceLevel_Data bids[MAX_PRICE];
    PriceLevel_Data asks[MAX_PRICE];
    int64_t best_bid = 0;
    int64_t best_ask = MAX_PRICE;
};

struct SymbolEntry {
    std::string symbol;
    uint32_t id;
};

std::vector<OrderNode> g_orders;
Book g_books[8];
uint32_t g_book_count = 0;

SymbolEntry g_symbol_entries[8];
uint32_t g_symbol_count = 0;

uint64_t g_order_count = 0;

inline uint32_t get_symbol_id(const std::string& symbol) {
    for (uint32_t i = 0; i < g_symbol_count; ++i) {
        if (g_symbol_entries[i].symbol == symbol) [[likely]] {
            return g_symbol_entries[i].id;
        }
    }
    uint32_t id = g_book_count++;
    g_symbol_entries[g_symbol_count++] = {symbol, id};
    return id;
}

inline void insert_node(Book& book, uint32_t order_idx) {
    OrderNode& order = g_orders[order_idx];
    PriceLevel_Data* level = nullptr;
    if (order.side == static_cast<uint8_t>(Side::Buy)) [[likely]] {
        level = &book.bids[order.price];
        if (order.price > book.best_bid) [[unlikely]] book.best_bid = order.price;
    } else {
        level = &book.asks[order.price];
        if (order.price < book.best_ask) [[unlikely]] book.best_ask = order.price;
    }
    
    order.prev = level->tail;
    order.next = 0;
    
    if (level->tail) [[likely]] {
        g_orders[level->tail].next = order_idx;
    } else {
        level->head = order_idx;
    }
    level->tail = order_idx;
    
    level->total_qty += order.qty;
    level->count++;
    g_order_count++;
}

inline void remove_node(Book& book, uint32_t order_idx) {
    OrderNode& order = g_orders[order_idx];
    order.active = 0;
    PriceLevel_Data* level = nullptr;
    if (order.side == static_cast<uint8_t>(Side::Buy)) {
        level = &book.bids[order.price];
    } else {
        level = &book.asks[order.price];
    }
    
    if (order.prev) [[likely]] g_orders[order.prev].next = order.next;
    else level->head = order.next;
    
    if (order.next) [[likely]] g_orders[order.next].prev = order.prev;
    else level->tail = order.prev;
    
    level->total_qty -= order.qty;
    level->count--;
    g_order_count--;
    
    if (level->count == 0) [[unlikely]] {
        if (order.side == static_cast<uint8_t>(Side::Buy) && order.price == book.best_bid) {
            while (book.best_bid > 0 && book.bids[book.best_bid].count == 0) {
                book.best_bid--;
            }
        } else if (order.side == static_cast<uint8_t>(Side::Sell) && order.price == book.best_ask) {
            while (book.best_ask < MAX_PRICE && book.asks[book.best_ask].count == 0) {
                book.best_ask++;
            }
        }
    }
}

} // namespace

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
    g_orders.clear();
    g_orders.reserve(16000000);
    g_orders.emplace_back(); // Dummy
    
    for (uint32_t i = 0; i < g_book_count; ++i) {
        g_books[i] = Book(); // reset
    }
    g_book_count = 0;
    g_symbol_count = 0;
    g_order_count = 0;
}

MatchingEngine::~MatchingEngine() {}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty() || request.price >= MAX_PRICE) [[unlikely]] {
        return OrderAck{0, OrderStatus::Rejected};
    }

    uint64_t order_id = next_order_id_++;
    uint32_t sym_id = get_symbol_id(request.symbol);
    Book& book = g_books[sym_id];

    uint32_t remaining = request.quantity;
    
    if (request.side == Side::Buy) [[likely]] {
        while (remaining > 0 && book.best_ask <= request.price && book.best_ask < MAX_PRICE) {
            PriceLevel_Data& level = book.asks[book.best_ask];
            while (level.head && remaining > 0) {
                uint32_t resting_idx = level.head;
                OrderNode& resting = g_orders[resting_idx];
                uint32_t trade_qty = std::min(remaining, resting.qty);
                
                remaining -= trade_qty;
                resting.qty -= trade_qty;
                level.total_qty -= trade_qty;
                
                listener_->onTrade(Trade{order_id, resting_idx, request.symbol, resting.price, trade_qty});
                
                if (resting.qty == 0) [[likely]] {
                    remove_node(book, resting_idx);
                    listener_->onOrderUpdate(OrderUpdate{resting_idx, OrderStatus::Filled, 0});
                } else {
                    listener_->onOrderUpdate(OrderUpdate{resting_idx, OrderStatus::Accepted, resting.qty});
                }
            }
        }
    } else {
        while (remaining > 0 && book.best_bid >= request.price && book.best_bid > 0) {
            PriceLevel_Data& level = book.bids[book.best_bid];
            while (level.head && remaining > 0) {
                uint32_t resting_idx = level.head;
                OrderNode& resting = g_orders[resting_idx];
                uint32_t trade_qty = std::min(remaining, resting.qty);
                
                remaining -= trade_qty;
                resting.qty -= trade_qty;
                level.total_qty -= trade_qty;
                
                listener_->onTrade(Trade{resting_idx, order_id, request.symbol, resting.price, trade_qty});
                
                if (resting.qty == 0) [[likely]] {
                    remove_node(book, resting_idx);
                    listener_->onOrderUpdate(OrderUpdate{resting_idx, OrderStatus::Filled, 0});
                } else {
                    listener_->onOrderUpdate(OrderUpdate{resting_idx, OrderStatus::Accepted, resting.qty});
                }
            }
        }
    }
    
    if (remaining > 0) [[likely]] {
        uint32_t order_idx = g_orders.size();
        g_orders.push_back({request.price, remaining, 0, 0, (uint16_t)sym_id, (uint8_t)request.side, 1});
        insert_node(book, order_idx);
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Accepted, remaining});
        return OrderAck{order_id, OrderStatus::Accepted};
    } else {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Filled, 0});
        g_orders.push_back({request.price, 0, 0, 0, (uint16_t)sym_id, (uint8_t)request.side, 0});
        return OrderAck{order_id, OrderStatus::Filled};
    }
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    if (order_id >= g_orders.size() || order_id == 0) [[unlikely]] return false;
    OrderNode& order = g_orders[order_id];
    if (!order.active) [[unlikely]] return false;
    
    Book& book = g_books[order.symbol_id];
    remove_node(book, order_id);
    listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    if (new_price <= 0 || new_quantity == 0 || new_price >= MAX_PRICE) [[unlikely]] return false;
    if (order_id >= g_orders.size() || order_id == 0) [[unlikely]] return false;
    
    OrderNode& order = g_orders[order_id];
    if (!order.active) [[unlikely]] return false;
    
    Book& book = g_books[order.symbol_id];
    
    if (new_price == order.price && new_quantity < order.qty) [[likely]] {
        uint32_t diff = order.qty - new_quantity;
        order.qty = new_quantity;
        if (order.side == static_cast<uint8_t>(Side::Buy)) {
            book.bids[order.price].total_qty -= diff;
        } else {
            book.asks[order.price].total_qty -= diff;
        }
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Accepted, new_quantity});
        return true;
    }
    
    remove_node(book, order_id);
    
    uint32_t remaining = new_quantity;
    std::string symbol = g_symbol_entries[order.symbol_id].symbol;
    
    if (order.side == static_cast<uint8_t>(Side::Buy)) {
        while (remaining > 0 && book.best_ask <= new_price && book.best_ask < MAX_PRICE) {
            PriceLevel_Data& level = book.asks[book.best_ask];
            while (level.head && remaining > 0) {
                uint32_t resting_idx = level.head;
                OrderNode& resting = g_orders[resting_idx];
                uint32_t trade_qty = std::min(remaining, resting.qty);
                
                remaining -= trade_qty;
                resting.qty -= trade_qty;
                level.total_qty -= trade_qty;
                
                listener_->onTrade(Trade{order_id, resting_idx, symbol, resting.price, trade_qty});
                
                if (resting.qty == 0) [[likely]] {
                    remove_node(book, resting_idx);
                    listener_->onOrderUpdate(OrderUpdate{resting_idx, OrderStatus::Filled, 0});
                } else {
                    listener_->onOrderUpdate(OrderUpdate{resting_idx, OrderStatus::Accepted, resting.qty});
                }
            }
        }
    } else {
         while (remaining > 0 && book.best_bid >= new_price && book.best_bid > 0) {
            PriceLevel_Data& level = book.bids[book.best_bid];
            while (level.head && remaining > 0) {
                uint32_t resting_idx = level.head;
                OrderNode& resting = g_orders[resting_idx];
                uint32_t trade_qty = std::min(remaining, resting.qty);
                
                remaining -= trade_qty;
                resting.qty -= trade_qty;
                level.total_qty -= trade_qty;
                
                listener_->onTrade(Trade{resting_idx, order_id, symbol, resting.price, trade_qty});
                
                if (resting.qty == 0) [[likely]] {
                    remove_node(book, resting_idx);
                    listener_->onOrderUpdate(OrderUpdate{resting_idx, OrderStatus::Filled, 0});
                } else {
                    listener_->onOrderUpdate(OrderUpdate{resting_idx, OrderStatus::Accepted, resting.qty});
                }
            }
        }
    }
    
    order.price = new_price;
    order.qty = remaining;
    
    if (remaining > 0) [[likely]] {
        order.active = 1;
        insert_node(book, order_id);
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Accepted, remaining});
    } else {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Filled, 0});
    }
    
    return true;
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    std::vector<PriceLevel> result;
    uint32_t sym_id = -1;
    for (uint32_t i = 0; i < g_symbol_count; ++i) {
        if (g_symbol_entries[i].symbol == symbol) {
            sym_id = g_symbol_entries[i].id;
            break;
        }
    }
    if (sym_id == (uint32_t)-1) return result;
    
    const Book& book = g_books[sym_id];
    
    if (side == Side::Buy) {
        for (int64_t p = book.best_bid; p > 0; p--) {
            if (book.bids[p].count > 0) {
                result.push_back({p, (uint32_t)book.bids[p].total_qty, book.bids[p].count});
            }
        }
    } else {
         for (int64_t p = book.best_ask; p < MAX_PRICE; p++) {
            if (book.asks[p].count > 0) {
                result.push_back({p, (uint32_t)book.asks[p].total_qty, book.asks[p].count});
            }
        }
    }
    return result;
}

uint64_t MatchingEngine::getOrderCount() const {
    return g_order_count;
}

}  // namespace exchange
