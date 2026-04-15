#include "exchange/matching_engine.h"
#include <vector>
#include <string>
#include <iostream>

namespace exchange {

namespace {

constexpr int64_t MAX_PRICE = 65536;

struct OrderNode {
    uint64_t id;
    int64_t price;
    uint32_t qty;
    uint32_t symbol_id;
    Side side;
    bool active;
    
    uint32_t prev;
    uint32_t next;
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
std::vector<Book> g_books;
std::vector<SymbolEntry> g_symbol_entries;
uint64_t g_order_count = 0;

uint32_t get_symbol_id(const std::string& symbol) {
    for (const auto& entry : g_symbol_entries) {
        if (entry.symbol == symbol) {
            return entry.id;
        }
    }
    uint32_t id = g_books.size();
    g_symbol_entries.push_back({symbol, id});
    g_books.emplace_back();
    return id;
}

void insert_node(Book& book, uint32_t order_idx) {
    OrderNode& order = g_orders[order_idx];
    PriceLevel_Data* level = nullptr;
    if (order.side == Side::Buy) {
        level = &book.bids[order.price];
        if (order.price > book.best_bid) book.best_bid = order.price;
    } else {
        level = &book.asks[order.price];
        if (order.price < book.best_ask) book.best_ask = order.price;
    }
    
    order.prev = level->tail;
    order.next = 0;
    
    if (level->tail) {
        g_orders[level->tail].next = order_idx;
    } else {
        level->head = order_idx;
    }
    level->tail = order_idx;
    
    level->total_qty += order.qty;
    level->count++;
    g_order_count++;
}

void remove_node(Book& book, uint32_t order_idx) {
    OrderNode& order = g_orders[order_idx];
    order.active = false;
    PriceLevel_Data* level = nullptr;
    if (order.side == Side::Buy) {
        level = &book.bids[order.price];
    } else {
        level = &book.asks[order.price];
    }
    
    if (order.prev) g_orders[order.prev].next = order.next;
    else level->head = order.next;
    
    if (order.next) g_orders[order.next].prev = order.prev;
    else level->tail = order.prev;
    
    level->total_qty -= order.qty;
    level->count--;
    g_order_count--;
    
    if (level->count == 0) {
        if (order.side == Side::Buy && order.price == book.best_bid) {
            while (book.best_bid > 0 && book.bids[book.best_bid].count == 0) {
                book.best_bid--;
            }
        } else if (order.side == Side::Sell && order.price == book.best_ask) {
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
    g_books.clear();
    g_symbol_entries.clear();
    g_order_count = 0;
}

MatchingEngine::~MatchingEngine() {}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
    if (request.price <= 0 || request.quantity == 0 || request.symbol.empty() || request.price >= MAX_PRICE) {
        return OrderAck{0, OrderStatus::Rejected};
    }

    uint64_t order_id = next_order_id_++;
    uint32_t sym_id = get_symbol_id(request.symbol);
    Book& book = g_books[sym_id];

    uint32_t remaining = request.quantity;
    
    // Match
    if (request.side == Side::Buy) {
        while (remaining > 0 && book.best_ask <= request.price && book.best_ask < MAX_PRICE) {
            PriceLevel_Data& level = book.asks[book.best_ask];
            while (level.head && remaining > 0) {
                uint32_t resting_idx = level.head;
                OrderNode& resting = g_orders[resting_idx];
                uint32_t trade_qty = std::min(remaining, resting.qty);
                
                remaining -= trade_qty;
                resting.qty -= trade_qty;
                level.total_qty -= trade_qty;
                
                listener_->onTrade(Trade{order_id, resting.id, request.symbol, resting.price, trade_qty});
                
                if (resting.qty == 0) {
                    remove_node(book, resting_idx);
                    listener_->onOrderUpdate(OrderUpdate{resting.id, OrderStatus::Filled, 0});
                } else {
                    listener_->onOrderUpdate(OrderUpdate{resting.id, OrderStatus::Accepted, resting.qty});
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
                
                listener_->onTrade(Trade{resting.id, order_id, request.symbol, resting.price, trade_qty});
                
                if (resting.qty == 0) {
                    remove_node(book, resting_idx);
                    listener_->onOrderUpdate(OrderUpdate{resting.id, OrderStatus::Filled, 0});
                } else {
                    listener_->onOrderUpdate(OrderUpdate{resting.id, OrderStatus::Accepted, resting.qty});
                }
            }
        }
    }
    
    if (remaining > 0) {
        uint32_t order_idx = g_orders.size();
        g_orders.push_back({order_id, request.price, remaining, sym_id, request.side, true, 0, 0});
        insert_node(book, order_idx);
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Accepted, remaining});
        return OrderAck{order_id, OrderStatus::Accepted};
    } else {
        listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Filled, 0});
        g_orders.push_back({order_id, request.price, 0, sym_id, request.side, false, 0, 0}); // keep ID valid
        return OrderAck{order_id, OrderStatus::Filled};
    }
}

bool MatchingEngine::cancelOrder(uint64_t order_id) {
    if (order_id >= g_orders.size() || order_id == 0) return false;
    OrderNode& order = g_orders[order_id];
    if (!order.active) return false;
    
    Book& book = g_books[order.symbol_id];
    remove_node(book, order_id);
    listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});
    return true;
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    if (new_price <= 0 || new_quantity == 0 || new_price >= MAX_PRICE) return false;
    if (order_id >= g_orders.size() || order_id == 0) return false;
    
    OrderNode& order = g_orders[order_id];
    if (!order.active) return false;
    
    Book& book = g_books[order.symbol_id];
    
    bool keep_priority = false;
    if (new_price == order.price && new_quantity < order.qty) {
        keep_priority = true;
    }
    
    if (keep_priority) {
        uint32_t diff = order.qty - new_quantity;
        order.qty = new_quantity;
        if (order.side == Side::Buy) {
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
    
    if (order.side == Side::Buy) {
        while (remaining > 0 && book.best_ask <= new_price && book.best_ask < MAX_PRICE) {
            PriceLevel_Data& level = book.asks[book.best_ask];
            while (level.head && remaining > 0) {
                uint32_t resting_idx = level.head;
                OrderNode& resting = g_orders[resting_idx];
                uint32_t trade_qty = std::min(remaining, resting.qty);
                
                remaining -= trade_qty;
                resting.qty -= trade_qty;
                level.total_qty -= trade_qty;
                
                listener_->onTrade(Trade{order_id, resting.id, symbol, resting.price, trade_qty});
                
                if (resting.qty == 0) {
                    remove_node(book, resting_idx);
                    listener_->onOrderUpdate(OrderUpdate{resting.id, OrderStatus::Filled, 0});
                } else {
                    listener_->onOrderUpdate(OrderUpdate{resting.id, OrderStatus::Accepted, resting.qty});
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
                
                listener_->onTrade(Trade{resting.id, order_id, symbol, resting.price, trade_qty});
                
                if (resting.qty == 0) {
                    remove_node(book, resting_idx);
                    listener_->onOrderUpdate(OrderUpdate{resting.id, OrderStatus::Filled, 0});
                } else {
                    listener_->onOrderUpdate(OrderUpdate{resting.id, OrderStatus::Accepted, resting.qty});
                }
            }
        }
    }
    
    order.price = new_price;
    order.qty = remaining;
    
    if (remaining > 0) {
        order.active = true;
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
    for (const auto& entry : g_symbol_entries) {
        if (entry.symbol == symbol) {
            sym_id = entry.id;
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
