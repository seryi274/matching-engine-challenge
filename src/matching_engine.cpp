// ReSharper disable CppJoinDeclarationAndAssignment
#include "exchange/matching_engine.h"

#include <algorithm>

namespace exchange {
    MatchingEngine::MatchingEngine(Listener* listener)
        : listener_(listener) {
        // TODO: Initialize your data structures here.
    }

    MatchingEngine::~MatchingEngine() {
        // TODO: Clean up if necessary.
    }


    OrderAck MatchingEngine::addOrder(const OrderRequest& request) {
        // Step 1: Validate the request.
        if (request.price <= 0 || request.quantity == 0 || request.symbol.empty()) {
            return OrderAck{0, OrderStatus::Rejected};
        }

        // Step 2: Assign an order ID.
        uint64_t order_id = next_order_id_++;

        // TODO: Step 3: Look up (or create) the order book for request.symbol.
        //
        //   Hint: You need a per-symbol data structure that maintains
        //   buy orders (bids) and sell orders (asks) separately.
        //   ---> OrderBook

        auto book = this->books_[request.symbol];

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

        if (request.side == Side::Buy) {
            auto& buy_price = request.price;
            auto buy_quantity = request.quantity;

            // asks is sorted ascending. we start with lowest asks
            auto current_price_it = book.asks.begin();
            std::list<Order>* current_order_list_p;
            std::list<Order>::iterator current_order_it; // NOLINT(*-use-auto)
            while (buy_quantity > 0 && current_price_it != book.asks.end()) {
                current_order_list_p = &current_price_it->second;
                current_order_it = current_order_list_p->begin();
                const int64_t current_price = current_price_it->first;

                if (current_price > buy_price) break;

                while (buy_quantity > 0 && current_order_it != current_order_list_p->end()) {
                    auto& [sell_order_id, sell_quantity] = *current_order_it;

                    const uint32_t trade_quantity = std::min(sell_quantity, buy_quantity);
                    this->listener_->onTrade(Trade{
                        order_id, sell_order_id, request.symbol, current_price, trade_quantity
                    });
                    sell_quantity -= trade_quantity;
                    buy_quantity -= trade_quantity;

                    if (sell_quantity == 0) {
                        this->listener_->onOrderUpdate(OrderUpdate{
                            sell_order_id, OrderStatus::Filled, 0
                        });
                        current_order_it = removeOrder(current_order_it, *current_order_list_p);
                    } else {
                        this->listener_->onOrderUpdate(OrderUpdate{
                            sell_order_id, OrderStatus::Accepted, sell_quantity
                        });
                    }


                    if (current_order_it == current_order_list_p->end()) break;
                    ++current_order_it;
                }
                if (current_order_list_p->empty()) {
                    // should we do this? is it worth it???
                    current_price_it = book.asks.erase(current_price_it);
                } else ++current_price_it;
            }

            if (buy_quantity > 0) {
                // add current buy order to resting orders
                auto& current_list = book.bids[- buy_price];
                current_list.emplace_front(order_id, buy_quantity);
                this->order_lookup_.emplace(order_id,
                                            OrderLookupInfo{- buy_price, current_list.begin(), &current_list, &book.bids}
                );

                this->listener_->onOrderUpdate(OrderUpdate{
                    order_id, OrderStatus::Accepted, buy_quantity
                });
                return {order_id, OrderStatus::Accepted};
            }
            this->listener_->onOrderUpdate(OrderUpdate{
                order_id, OrderStatus::Filled, 0
            });
            return {order_id, OrderStatus::Filled};
        }

        auto& sell_price = request.price;
        // bids is sorted descending AND THEY'RE NEGATIVE. we'll start with the biggest bid and go down from there
        auto sell_quantity = request.quantity;

        auto current_price_it = book.bids.begin();
        std::list<Order>* current_order_list_p;
        std::list<Order>::iterator current_order_it;
        while (sell_quantity > 0 && current_price_it != book.asks.end()) {
            current_order_list_p = &current_price_it->second;
            current_order_it = current_order_list_p->begin();
            const int64_t current_price = current_price_it->first;

            // BIDS ARE NEGATIVE
            if (0 < sell_price + current_price) break;

            while (sell_quantity > 0 && current_order_it != current_order_list_p->end()) {
                auto& [buy_order_id, buy_quantity] = *current_order_it;

                const uint32_t trade_quantity = std::min(buy_quantity, sell_quantity);
                this->listener_->onTrade(Trade{
                    buy_order_id, order_id, request.symbol, - current_price, trade_quantity
                });

                sell_quantity -= trade_quantity;
                buy_quantity -= trade_quantity;

                if (buy_quantity == 0) {
                    this->listener_->onOrderUpdate(OrderUpdate{
                        buy_order_id, OrderStatus::Filled, 0
                    });
                    current_order_it = removeOrder(current_order_it, *current_order_list_p);
                } else {
                    this->listener_->onOrderUpdate(OrderUpdate{
                        buy_order_id, OrderStatus::Accepted, sell_quantity
                    });
                }

                if (current_order_it == current_order_list_p->end()) break;
                ++current_order_it;
            }
            if (current_order_list_p->empty()) {
                current_price_it = book.bids.erase(current_price_it);
            } else ++current_price_it;
        }

        if (sell_quantity > 0) {
            auto& current_list = book.asks[sell_price];
            current_list.emplace_front(order_id, sell_quantity);
            this->order_lookup_.emplace(order_id,
                                        OrderLookupInfo{sell_price, current_list.begin(), &current_list, &book.asks}
            );

            this->listener_->onOrderUpdate(OrderUpdate{
                order_id, OrderStatus::Accepted, sell_quantity
            });
            return {order_id, OrderStatus::Accepted};
        }
        this->listener_->onOrderUpdate(OrderUpdate{
            order_id, OrderStatus::Filled, 0
        });

        return {order_id, OrderStatus::Filled};


        // TODO: Step 5: If quantity remains, insert into the book.

        // TODO: Step 6: Call listener_->onOrderUpdate() for the incoming order.
        //   - status = Filled     if fully matched (remaining == 0)
        //   - status = Accepted   if resting on the book (remaining > 0)

        // TODO: Step 7: Return the ack.
        //   - status = Filled   if fully matched
        //   - status = Accepted if resting (including partial fills)
        return OrderAck{order_id, OrderStatus::Rejected}; // placeholder -- replace this
    }


    bool MatchingEngine::cancelOrder(uint64_t order_id) {
        const auto& res_it = this->order_lookup_.find(order_id);
        if (res_it == this->order_lookup_.end()) return false;

        res_it->second.list_p->erase(res_it->second.order_it);
        this->order_lookup_.erase(res_it);

        return true;
    }

    bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
        // Validate parameters.
        if (new_price <= 0 || new_quantity == 0) {
            return false;
        }

        auto lookup_it = this->order_lookup_.find(order_id);

        if (lookup_it == this->order_lookup_.end()) return false;

        auto info_it = lookup_it->second;
        auto order_it = info_it.order_it;

        bool price_changed = info_it.price != new_price;


        if (!price_changed && info_it.order_it->quantity < new_quantity) {
            // keeps priority
            info_it.order_it->quantity = new_quantity;
        } else {
            // loses priority
            // we use std::list<Order>::splice !!!

            // this code moves the list, but works fine if the price doesn't change. we do need the splice anyways.
            std::list<Order>& dest_list = info_it.order_book->operator[](new_price);
            dest_list.splice(dest_list.end(), *info_it.list_p, info_it.order_it);
            info_it.order_it = std::prev(dest_list.end());
            info_it.price = new_price;
            info_it.list_p = &dest_list;
            dest_list.back().quantity = new_quantity;
            // If the new price could cause a match (e.g., a buy order's price
            // is raised above the best ask), run matching logic.!!!!!
        }

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


        return true; // placeholder
    }

    std::vector<PriceLevel> MatchingEngine::getBookSnapshot(const std::string& symbol, Side side) const {
        // TODO: UPDATE THE FUNCTION, BIDS ARE NOW NEGATIVE. ALSO, MAPS ARE THE SAME TYPE
        // SO WE DON'T NEED TEMPLATE. I'M WRITING IN CAPS SO YOU CAN DISTINGUISH MY TODOS
        // FROM THE REST OF THEIR TODOS. OK BYE.
        // ^ THIS IS DONE.
        //
        // Sort best-to-worst:
        //   Buy side:  highest price first
        //   Sell side: lowest price first

        const auto& books_it = books_.find(symbol);
        if (books_it == books_.end()) return {};

        return side == Side::Buy
                   ? getBookSideSnapshot<true>(books_it->second.bids)
                   : getBookSideSnapshot<false>(books_it->second.asks);
    }

    uint64_t MatchingEngine::getOrderCount() const {
        return this->order_lookup_.size();
    }
} // namespace exchange
