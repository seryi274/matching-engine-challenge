#pragma once

#include <iostream>
#include <list>
#include <map>

#include "types.h"
#include <vector>
#include <string>
#include <unordered_map>

namespace exchange {
    struct Order {
        uint64_t order_id;
        uint32_t quantity;
    };

    using price_map = std::map<int64_t, std::list<Order>>;

    struct OrderBook {
        // should we use a queue instead of a list? because we're treating a list as a fifo
        // container really. it would be the same. also, since queue uses a vector underneath,
        // maybe it has better cache locality??? who knows.
        // oh wait we can't use a vector because we need to have consistent iterators so we can
        // put something in order_lookup_
        // maybe we can store pointers instead? hmmm that would create another level of indirection
        // which can actually impact performance negatively.

        // bids are NEGATIVE.
        price_map bids;
        price_map asks;
    };

    /// ============================================================
    ///  MatchingEngine
    ///
    ///  Students implement ALL methods in src/matching_engine.cpp.
    ///  They may add any private members, helper classes, or internal
    ///  data structures they wish. The public interface is FIXED --
    ///  DO NOT MODIFY THIS FILE.
    ///
    ///  MATCHING RULES:
    ///    - Price-time priority (FIFO at each price level)
    ///    - A buy order matches against resting sells if buy.price >= sell.price
    ///    - A sell order matches against resting buys  if sell.price <= buy.price
    ///    - Execution price is always the RESTING (passive) order's price
    ///    - Orders match greedily: fill as much as possible immediately
    ///    - Any unmatched remainder rests on the book
    ///
    ///  THREAD SAFETY:
    ///    - The engine is single-threaded. No locking is required.
    ///
    /// ============================================================
    class MatchingEngine {
    public:
        /// Construct engine with a listener that receives trade/update callbacks.
        /// The listener pointer must remain valid for the engine's lifetime.
        explicit MatchingEngine(Listener* listener);

        ~MatchingEngine();

        // --------------------------------------------------------
        //  Order Entry
        // --------------------------------------------------------

        /// Submit a new limit order.
        ///
        /// The engine:
        ///   1. Assigns a unique order_id (monotonically increasing, starting at 1)
        ///   2. Attempts to match against resting orders on the opposite side
        ///   3. Calls listener->onTrade() for each execution
        ///   4. Calls listener->onOrderUpdate() for each affected resting order
        ///   5. Calls listener->onOrderUpdate() for the incoming order
        ///   6. If any quantity remains, rests the order on the book
        ///   7. Returns an OrderAck with the assigned order_id and status
        ///
        /// Reject if: price <= 0, quantity == 0, or symbol is empty.
        OrderAck addOrder(const OrderRequest& request);

        // --------------------------------------------------------
        //  Order Cancellation
        // --------------------------------------------------------

        /// Cancel a resting order by its order_id.
        ///
        /// If found and still resting, remove from book and call
        /// listener->onOrderUpdate() with status = Cancelled.
        ///
        /// Returns true if cancelled, false if order not found or already filled.
        bool cancelOrder(uint64_t order_id);

        // --------------------------------------------------------
        //  Order Amendment
        // --------------------------------------------------------

        /// Modify a resting order's price and/or quantity.
        ///
        /// Rules:
        ///   - Order must exist and be resting on the book
        ///   - Side and symbol cannot change
        ///   - If price changes: order LOSES time priority
        ///   - If only quantity decreases (price unchanged): KEEPS time priority
        ///   - If quantity increases (even if price unchanged): LOSES time priority
        ///   - new_quantity must be > 0, new_price must be > 0
        ///   - After amendment, the order MAY match against the opposite side
        ///
        /// Returns true if amended, false if order not found or invalid parameters.
        bool amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity);

        // --------------------------------------------------------
        //  Query (for testing / debugging -- NOT performance-critical)
        // --------------------------------------------------------

        /// Get a snapshot of one side of the book for a given symbol.
        /// Buy side:  highest price first (best bid at front)
        /// Sell side: lowest price first  (best ask at front)
        std::vector<PriceLevel> getBookSnapshot(const std::string& symbol, Side side) const;

        /// Get total number of live (resting) orders across all symbols.
        uint64_t getOrderCount() const;

    private:
        // ============================================================
        //  STUDENT: Add your data structures here.
        //
        //  Suggested starting point (naive but correct):
        //    - std::unordered_map<std::string, OrderBook> books_
        //      where OrderBook contains two std::map<int64_t, std::list<Order>>
        //      (one for bids sorted descending, one for asks sorted ascending)
        //    - std::unordered_map<uint64_t, pointer/iterator> order_lookup_
        //      for O(1) cancel/amend by order_id
        //
        //  Better approaches to explore:
        //    - Flat sorted arrays with binary search
        //    - Custom memory pools / arena allocators
        //    - Cache-aligned price level nodes
        //    - Intrusive linked lists
        //    - Pre-allocated order arrays with free lists
        // ============================================================

        // TODO: Add your internal data structures here.
        std::unordered_map<std::string, OrderBook> books_;

        struct OrderLookupInfo {
            // bids are NEGATIVE
            int64_t price;
            std::list<Order>::iterator order_it;
            std::list<Order>* list_p;
            decltype(books_)::iterator book_it;

            [[nodiscard]] price_map& getPriceMap() const {
                return price < 0 ? book_it->second.bids : book_it->second.asks;
            }

            [[nodiscard]] Side getSide() const {
                return price < 0 ? Side::Buy : Side::Sell;
            }

            [[nodiscard]] std::string getSymbol() const {
                return book_it->first;
            }
        };

        // We only store RESTING orders here
        std::unordered_map<uint64_t, OrderLookupInfo> order_lookup_;

        Listener* listener_;
        uint64_t next_order_id_ = 1;
        /**
         *
         * @param order_it
         * @param order_list
         * @return An iterator pointing to the next element in @code order_list@endcode, or @code end@endcode.
         */
        std::list<Order>::iterator removeOrder(std::list<Order>::iterator order_it, std::list<Order>& order_list) {
            this->order_lookup_.erase(order_it->order_id);
            return order_list.erase(order_it);
        }

        template<bool NegativePrice = false>
        std::vector<PriceLevel> getBookSideSnapshot(const std::map<int64_t, std::list<Order>>& book) const {
            std::vector<PriceLevel> prices(book.size());
            auto prices_it = prices.begin();
            for (const auto& [price, orders_list] : book) {
                // CONSTEXPR ALL THE THINGS.
                if constexpr (NegativePrice) prices_it->price = -price;
                else prices_it->price = price;

                prices_it->order_count = 0;
                for (const auto& [_, quantity] : orders_list) {
                    ++prices_it->order_count;
                    prices_it->total_quantity += quantity;
                }
                ++prices_it;
            }

            return prices;
        }

        struct OrderActiveMatch {
            std::string symbol;
            int64_t price;
            uint32_t quantity;
        };

        /**
         * @tparam side Side of the active order.
         */
        template<Side side>
        OrderAck match(uint64_t active_order_id, const OrderActiveMatch& request) {
            auto book_it = this->books_.try_emplace(request.symbol).first;

            price_map *book, *my_book;
            if constexpr (side == Side::Buy) {
                book = &book_it->second.asks;
                my_book = &book_it->second.bids;
            } else {
                book = &book_it->second.bids;
                my_book = &book_it->second.asks;
            }

            int64_t active_price;
            if constexpr (side == Side::Buy) active_price = -request.price;
            else active_price = request.price;

            auto active_quantity = request.quantity;

            auto current_price_it = book->begin();
            std::list<Order>* current_order_list_p;
            std::list<Order>::iterator current_order_it;
            while (active_quantity > 0 && current_price_it != book->end()) {
                current_order_list_p = &current_price_it->second;
                current_order_it = current_order_list_p->begin();

                // if it's a bid, it's negative.
                const int64_t current_price = current_price_it->first;

                if (0 < active_price + current_price) break;

                while (active_quantity > 0 && current_order_it != current_order_list_p->end()) {
                    auto& [resting_order_id, resting_quantity] = *current_order_it;
                    const uint32_t trade_quantity = std::min(resting_quantity, active_quantity);

                    // Execution price is always the RESTING (passive) order's price
                    const int64_t trade_price = std::abs(current_price);
                    if constexpr (side == Side::Buy) {
                        this->listener_->onTrade(Trade{
                            active_order_id, resting_order_id, request.symbol, trade_price, trade_quantity
                        });
                    } else {
                        this->listener_->onTrade(Trade{
                            resting_order_id, active_order_id, request.symbol, trade_price, trade_quantity
                        });
                    }

                    resting_quantity -= trade_quantity;
                    active_quantity -= trade_quantity;

                    if (resting_quantity == 0) {
                        this->listener_->onOrderUpdate(OrderUpdate{
                            resting_order_id, OrderStatus::Filled, 0
                        });
                        current_order_it = removeOrder(current_order_it, *current_order_list_p);
                    } else {
                        this->listener_->onOrderUpdate(OrderUpdate{
                            resting_order_id, OrderStatus::Accepted, resting_quantity
                        });
                    }

                    if (current_order_it == current_order_list_p->end()) break;
                    ++current_order_it;
                }
                if (current_order_list_p->empty()) {
                    current_price_it = book->erase(current_price_it);
                } else ++current_price_it;
            }

            if (active_quantity > 0) {
                auto& current_list = (*my_book)[active_price];
                current_list.emplace_back(active_order_id, active_quantity);

                this->order_lookup_.insert_or_assign(active_order_id,
                                                     OrderLookupInfo{
                                                         active_price, std::prev(current_list.end()), &current_list, book_it
                                                     }
                );

                this->listener_->onOrderUpdate(OrderUpdate{
                    active_order_id, OrderStatus::Accepted, active_quantity
                });
                return {active_order_id, OrderStatus::Accepted};
            }
            this->listener_->onOrderUpdate(OrderUpdate{
                active_order_id, OrderStatus::Filled, 0
            });

            return {active_order_id, OrderStatus::Filled};
        }
    };
} // namespace exchange
