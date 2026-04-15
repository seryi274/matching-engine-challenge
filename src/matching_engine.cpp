#include "exchange/matching_engine.h"
#include <iostream>
#include <cassert>
namespace exchange {

const int MAX_STOCK = 300000;
const int MAX_ORDERS = 10000000;

MatchingEngine::MatchingEngine(Listener* listener)
   : listener_(listener)
{
   stockToId.reserve(MAX_STOCK);
   existingOrders.reserve(MAX_ORDERS);
}


MatchingEngine::~MatchingEngine() {
   existingOrders.clear();
   stockToId.clear();
   book[0].clear();
   book[1].clear();
}


// Match an order with the rest of the book.
OrderStatus MatchingEngine::matchOrder(const Order& request) {

    const uint8_t bookSide = (request.side == Side::Buy ? 0 : 1);
    const uint32_t symbol_id = stockToId[request.symbol];
    uint32_t remaining_quantity = request.quantity;
    const uint8_t opposite_side = (bookSide ^ 1);
    const int8_t multiplier = (request.side == Side::Buy ? 1 : -1); // 1 to sell orders

    std::set<Order, OrderCmp>& orders_to_match = book[opposite_side][symbol_id];
    while (remaining_quantity > 0 && !orders_to_match.empty() && 
           orders_to_match.begin()->price * multiplier <= request.price * multiplier) {
        // We have a trade.
        const int64_t current_price = orders_to_match.begin()->price;
        const uint32_t volume = std::min(remaining_quantity, orders_to_match.begin()->quantity);
        uint64_t buy_side = request.order_id;
        uint64_t sell_side = orders_to_match.begin()->order_id;
        if (request.side == Side::Sell) std::swap(buy_side, sell_side);
        listener_->onTrade(Trade{buy_side, sell_side, request.symbol, current_price, volume});

        if (volume >= orders_to_match.begin()->quantity) {
            listener_->onOrderUpdate(OrderUpdate{orders_to_match.begin()->order_id, OrderStatus::Filled, 0});
            existingOrders.erase(orders_to_match.begin()->order_id);
            orders_to_match.erase(orders_to_match.begin());
        } else {
            Order toModify = existingOrders[orders_to_match.begin()->order_id];
            toModify.quantity -= volume;
            existingOrders[orders_to_match.begin()->order_id] = toModify;
            orders_to_match.erase(orders_to_match.begin());
            orders_to_match.insert(toModify);
        }
        remaining_quantity -= volume;
    }
    
    if (remaining_quantity == 0) {
        listener_->onOrderUpdate(OrderUpdate{request.order_id, OrderStatus::Filled, 0});
        return OrderStatus::Filled;
    }
    else {
        existingOrders[request.order_id] = request;
        book[bookSide][symbol_id].insert(request);
    }
    return OrderStatus::Accepted;
}

OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
   // Step 1: Validate the request.
   if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) {
       return OrderAck{0, OrderStatus::Rejected};
   }
   // Step 2: Assign an order ID.
   const uint64_t order_id = next_order_id_++;
   const uint8_t bookSide = (request.side == Side::Buy ? 0 : 1);

    if (stockToId.find(request.symbol) == stockToId.end()) {
	    const uint32_t symbol_id = next_symbol_id_++;
        stockToId[request.symbol] = symbol_id;
        std::set<Order, OrderCmp> toAdd;
        book[bookSide].push_back(toAdd);
        book[(bookSide^1)].push_back(toAdd);
    }

    Order new_order;
    new_order.quantity = request.quantity;
    new_order.price = request.price;
    new_order.symbol = request.symbol;
    new_order.side = request.side;
    new_order.order_id = order_id;
    new_order.timestamp = next_timestamp_id_++;
    OrderStatus status = matchOrder(new_order);

   return OrderAck{order_id, status};
}


bool MatchingEngine::cancelOrder(uint64_t order_id) {
    if (existingOrders.find(order_id) == existingOrders.end()) {
        return false;
    }
    const Order toErase = existingOrders[order_id];
    const uint8_t bookSide = (toErase.side == Side::Buy ? 0 : 1);
    existingOrders.erase(order_id);
    book[bookSide][stockToId[toErase.symbol]].erase(toErase);
    listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});
   return true;
}


bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
   if (new_price <= 0 || new_quantity == 0 || existingOrders.find(order_id) == existingOrders.end()) {
       return false;
   }
   Order toModify = existingOrders[order_id];
   const uint8_t bookSide = (toModify.side == Side::Buy ? 0 : 1);
   book[bookSide][stockToId[toModify.symbol]].erase(toModify);
   if (new_price != toModify.price || toModify.quantity < new_quantity) {
        toModify.price = new_price;
        toModify.quantity = new_quantity;
        toModify.timestamp = next_timestamp_id_++;
        book[bookSide][stockToId[toModify.symbol]].insert(toModify);
        matchOrder(toModify);
    } else {
        toModify.quantity = new_quantity;
        book[bookSide][stockToId[toModify.symbol]].insert(toModify);
    }
   existingOrders[order_id] = toModify;
   return true;
}


std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
   const std::string& symbol, Side side) const
{
	if(stockToId.find(symbol) == stockToId.end()) 
        return std::vector<PriceLevel>();

   	const uint32_t id = stockToId.find(symbol)->second;
    const uint8_t bookSide = (side == Side::Buy ? 0 : 1);
    if (book[bookSide][id].empty()) {
        return std::vector<PriceLevel>();
    }
    std::vector<PriceLevel> priceLevel;
    PriceLevel* last = new PriceLevel();
    last->price = book[bookSide][id].cbegin()->price;
    last->total_quantity = 0;
    last->order_count = 0;

    for (auto it = book[bookSide][id].cbegin(); it != book[bookSide][id].cend(); ++it){
        if (last->price != it->price){
            priceLevel.push_back(*last);
            last->price = it->price;
            last->order_count = 0;
            last->total_quantity = 0;
        }
        last->order_count++;
        last->total_quantity += it->quantity;
    }
    priceLevel.push_back(*last);
    
    return priceLevel;
}


uint64_t MatchingEngine::getOrderCount() const {
   return existingOrders.size();
}

}  // namespace exchange
