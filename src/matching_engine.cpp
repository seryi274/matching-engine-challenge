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

        OrderActiveMatch order_active_match = {request.symbol, request.price, request.quantity};
        if (request.side == Side::Sell) {
            return this->match<Side::Sell>(order_id, order_active_match);
        }
        return this->match<Side::Buy>(order_id, order_active_match);
        //auto book_it = this->books_.try_emplace(request.symbol).first;

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
        /*
                if (request.side == Side::Buy) {
                    auto& book = book_it->second.asks;
                    auto buy_price = request.price;
                    auto buy_quantity = request.quantity;

                    // asks is sorted ascending. we start with lowest asks
                    auto current_price_it = book.begin();
                    std::list<Order>* current_order_list_p;
                    std::list<Order>::iterator current_order_it; // NOLINT(*-use-auto)
                    while (buy_quantity > 0 && current_price_it != book.end()) {
                        current_order_list_p = &current_price_it->second;
                        current_order_it = current_order_list_p->begin();
                        const int64_t current_price = current_price_it->first;

                        if (current_price > buy_price) break;

                        // !current_order_list_p.empty() => current_order_it != current_order_list_p->end()
                        do {
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
                        } while (buy_quantity > 0);

                        if (current_order_list_p->empty()) {
                            // should we do this? is it worth it???
                            current_price_it = book.erase(current_price_it);
                        } else ++current_price_it;
                    }

                    if (buy_quantity > 0) {
                        // add current buy order to resting orders
                        auto& current_list = book_it->second.bids[-buy_price];
                        current_list.emplace_back(order_id, buy_quantity);
                        this->order_lookup_.emplace(order_id,
                                                    OrderLookupInfo{-buy_price, current_list.begin(), &current_list, book_it}
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

                auto& book = book_it->second.bids;
                auto current_price_it = book.begin();
                std::list<Order>* current_order_list_p;
                std::list<Order>::iterator current_order_it;
                while (sell_quantity > 0 && current_price_it != book.end()) {
                    current_order_list_p = &current_price_it->second;
                    current_order_it = current_order_list_p->begin();
                    const int64_t current_price = current_price_it->first;

                    // BIDS ARE NEGATIVE
                    if (0 < sell_price + current_price) break;

                    while (sell_quantity > 0 && current_order_it != current_order_list_p->end()) {
                        auto& [buy_order_id, buy_quantity] = *current_order_it;

                        const uint32_t trade_quantity = std::min(buy_quantity, sell_quantity);
                        this->listener_->onTrade(Trade{
                            buy_order_id, order_id, request.symbol, -current_price, trade_quantity
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
                        current_price_it = book.erase(current_price_it);
                    } else ++current_price_it;
                }

                if (sell_quantity > 0) {
                    auto& current_list = book_it->second.asks[sell_price];
                    current_list.emplace_back(order_id, sell_quantity);
                    this->order_lookup_.emplace(order_id,
                                                OrderLookupInfo{sell_price, current_list.begin(), &current_list, book_it}
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
        */

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
        if (res_it->second.list_p->empty()) res_it->second.getPriceMap().erase(res_it->second.price);

        this->order_lookup_.erase(res_it);

        this->listener_->onOrderUpdate(OrderUpdate{order_id, OrderStatus::Cancelled, 0});

        return true;
    }

    bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
        // Validate parameters.
        if (new_price <= 0 || new_quantity == 0) {
            return false;
        }

        auto lookup_it = this->order_lookup_.find(order_id);

        if (lookup_it == this->order_lookup_.end()) return false;

        auto& info_it = lookup_it->second;


        if (std::abs(info_it.price) == new_price) {
            if (info_it.order_it->quantity == new_quantity) return true;

            if (info_it.order_it->quantity > new_quantity) {
                info_it.order_it->quantity = new_quantity;
                listener_->onOrderUpdate(OrderUpdate{
                    order_id, OrderStatus::Accepted, new_quantity
                });
                return true;
            }

            info_it.list_p->splice(info_it.list_p->end(), *info_it.list_p, info_it.order_it);
            info_it.order_it = std::prev(info_it.list_p->end());
            info_it.order_it->quantity = new_quantity;
            listener_->onOrderUpdate(OrderUpdate{
                order_id, OrderStatus::Accepted, new_quantity
            });
            return true;
        }
        info_it.list_p->erase(info_it.order_it);
        if (info_it.list_p->empty()) info_it.getPriceMap().erase(info_it.price);


        new_price *= (info_it.price > 0);
        /*info_it.list_p = &dest_list;
        */

        const OrderActiveMatch order_active_match = {info_it.getSymbol(), new_price, new_quantity};
        OrderAck ack;
        if (info_it.price > 0) {
            ack = this->match<Side::Sell>(order_id, order_active_match);
        } else ack = this->match<Side::Buy>(order_id, order_active_match);

        if (ack.status == OrderStatus::Filled) {
            this->order_lookup_.erase(lookup_it);
        }
        return true;
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


        const bool is_buy = side == Side::Buy;
        const auto& book = is_buy ? books_it->second.bids : books_it->second.asks;

        std::vector<PriceLevel> prices(book.size());
        auto prices_it = prices.begin();
        for (const auto& [price, orders_list] : book) {
            // CONSTEXPR ALL THE THINGS.
            prices_it->price = (is_buy) ? -price : price;
            prices_it->order_count = 0;
            for (const auto& [_, quantity] : orders_list) {
                ++prices_it->order_count;
                prices_it->total_quantity += quantity;
            }
            ++prices_it;
        }

        return prices;
    }

    uint64_t MatchingEngine::getOrderCount() const {
        return this->order_lookup_.size();
    }
} // namespace exchange
