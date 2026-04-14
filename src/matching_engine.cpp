#include "exchange/matching_engine.h"

namespace exchange {

MatchingEngine::MatchingEngine(Listener* listener)
    : listener_(listener)
{
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

    // TODO: Step 5: If quantity remains, insert into the book.

    // TODO: Step 6: Call listener_->onOrderUpdate() for the incoming order.
    //   - status = Filled     if fully matched (remaining == 0)
    //   - status = Accepted   if resting on the book (remaining > 0)

    // TODO: Step 7: Return the ack.
    //   - status = Filled   if fully matched
    //   - status = Accepted if resting (including partial fills)
    return OrderAck{order_id, OrderStatus::Rejected};  // placeholder -- replace this
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

    return false;  // placeholder
}

bool MatchingEngine::amendOrder(uint64_t order_id, int64_t new_price, uint32_t new_quantity) {
    // Validate parameters.
    if (new_price <= 0 || new_quantity == 0) {
        return false;
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

    return false;  // placeholder
}

std::vector<PriceLevel> MatchingEngine::getBookSnapshot(
    const std::string& symbol, Side side) const
{
    // TODO: Build a vector of PriceLevel for the requested side.
    //
    // Sort best-to-worst:
    //   Buy side:  highest price first
    //   Sell side: lowest price first

    return {};  // placeholder
}

uint64_t MatchingEngine::getOrderCount() const {
    // TODO: Return total number of resting orders across all symbols.

    return 0;  // placeholder
}

}  // namespace exchange
