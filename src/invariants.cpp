#include "marketlab/invariants.hpp"
#include <sstream>

namespace marketlab {

InvariantResult InvariantChecker::check_book_invariants(const OrderBook& book) {
    if (book.total_bid_quantity() < 0) {
        return InvariantResult::fail("Total bid quantity is negative: " + std::to_string(book.total_bid_quantity()));
    }
    if (book.total_ask_quantity() < 0) {
        return InvariantResult::fail("Total ask quantity is negative: " + std::to_string(book.total_ask_quantity()));
    }

    Quantity calculated_bid_total = 0;
    std::optional<Price> prev_bid_price;

    for (const auto& [price, level] : book.bids()) {
        if (price <= 0) {
            return InvariantResult::fail("Bid price must be positive, found: " + std::to_string(price));
        }
        if (price % book.config().tick_size != 0) {
            return InvariantResult::fail("Bid price off-tick: " + std::to_string(price));
        }
        if (prev_bid_price && price >= *prev_bid_price) {
            return InvariantResult::fail("Bid levels not strictly descending");
        }
        prev_bid_price = price;

        if (level.orders.empty()) {
            return InvariantResult::fail("Empty bid price level preserved in book at price: " + std::to_string(price));
        }
        if (level.total_quantity < 0) {
            return InvariantResult::fail("Negative total quantity in bid level at price " + std::to_string(price));
        }

        Quantity level_sum = 0;
        for (const auto& order : level.orders) {
            if (order.remaining_quantity <= 0) {
                return InvariantResult::fail("Non-positive remaining quantity in bid order " + std::to_string(order.order_id));
            }
            if (order.side != Side::Buy) {
                return InvariantResult::fail("Non-buy order found in bid level: " + std::to_string(order.order_id));
            }
            if (order.price != price) {
                return InvariantResult::fail("Order price mismatch in bid level: " + std::to_string(order.order_id));
            }
            auto next_sum = CheckedMath::safe_add_quantity(level_sum, order.remaining_quantity);
            if (!next_sum) {
                return InvariantResult::fail("Quantity overflow while accumulating bid level orders at price " + std::to_string(price));
            }
            level_sum = *next_sum;
        }

        if (level_sum != level.total_quantity) {
            return InvariantResult::fail("Bid level total mismatch at price " + std::to_string(price) +
                ": sum=" + std::to_string(level_sum) + " recorded=" + std::to_string(level.total_quantity));
        }

        auto next_bid_total = CheckedMath::safe_add_quantity(calculated_bid_total, level_sum);
        if (!next_bid_total) {
            return InvariantResult::fail("Quantity overflow while accumulating total bid quantity");
        }
        calculated_bid_total = *next_bid_total;
    }

    if (calculated_bid_total != book.total_bid_quantity()) {
        return InvariantResult::fail("Total bid quantity mismatch: calculated=" +
            std::to_string(calculated_bid_total) + " recorded=" + std::to_string(book.total_bid_quantity()));
    }

    Quantity calculated_ask_total = 0;
    std::optional<Price> prev_ask_price;

    for (const auto& [price, level] : book.asks()) {
        if (price <= 0) {
            return InvariantResult::fail("Ask price must be positive, found: " + std::to_string(price));
        }
        if (price % book.config().tick_size != 0) {
            return InvariantResult::fail("Ask price off-tick: " + std::to_string(price));
        }
        if (prev_ask_price && price <= *prev_ask_price) {
            return InvariantResult::fail("Ask levels not strictly ascending");
        }
        prev_ask_price = price;

        if (level.orders.empty()) {
            return InvariantResult::fail("Empty ask price level preserved in book at price: " + std::to_string(price));
        }
        if (level.total_quantity < 0) {
            return InvariantResult::fail("Negative total quantity in ask level at price " + std::to_string(price));
        }

        Quantity level_sum = 0;
        for (const auto& order : level.orders) {
            if (order.remaining_quantity <= 0) {
                return InvariantResult::fail("Non-positive remaining quantity in ask order " + std::to_string(order.order_id));
            }
            if (order.side != Side::Sell) {
                return InvariantResult::fail("Non-sell order found in ask level: " + std::to_string(order.order_id));
            }
            if (order.price != price) {
                return InvariantResult::fail("Order price mismatch in ask level: " + std::to_string(order.order_id));
            }
            auto next_sum = CheckedMath::safe_add_quantity(level_sum, order.remaining_quantity);
            if (!next_sum) {
                return InvariantResult::fail("Quantity overflow while accumulating ask level orders at price " + std::to_string(price));
            }
            level_sum = *next_sum;
        }

        if (level_sum != level.total_quantity) {
            return InvariantResult::fail("Ask level total mismatch at price " + std::to_string(price) +
                ": sum=" + std::to_string(level_sum) + " recorded=" + std::to_string(level.total_quantity));
        }

        auto next_ask_total = CheckedMath::safe_add_quantity(calculated_ask_total, level_sum);
        if (!next_ask_total) {
            return InvariantResult::fail("Quantity overflow while accumulating total ask quantity");
        }
        calculated_ask_total = *next_ask_total;
    }

    if (calculated_ask_total != book.total_ask_quantity()) {
        return InvariantResult::fail("Total ask quantity mismatch: calculated=" +
            std::to_string(calculated_ask_total) + " recorded=" + std::to_string(book.total_ask_quantity()));
    }

    // Crossed book invariant
    auto best_bid = book.best_bid();
    auto best_ask = book.best_ask();
    if (best_bid && best_ask) {
        if (*best_bid >= *best_ask) {
            return InvariantResult::fail("Crossed book invariant violated: best_bid=" +
                std::to_string(*best_bid) + " >= best_ask=" + std::to_string(*best_ask));
        }
    }

    return InvariantResult::pass();
}

void InvariantChecker::LifecycleAuditor::record_event(const Event& ev) {
    std::visit([this](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, OrderAcceptedEvent>) {
            auto& audit = audits_[e.order_id];
            audit.accepted = true;
            audit.accepted_qty = e.quantity;
        } else if constexpr (std::is_same_v<T, TradeEvent>) {
            auto t_opt = CheckedMath::safe_add_quantity(audits_[e.taker_order_id].filled_qty, e.quantity);
            audits_[e.taker_order_id].filled_qty = t_opt ? *t_opt : std::numeric_limits<Quantity>::max();

            auto m_opt = CheckedMath::safe_add_quantity(audits_[e.maker_order_id].filled_qty, e.quantity);
            audits_[e.maker_order_id].filled_qty = m_opt ? *m_opt : std::numeric_limits<Quantity>::max();
        } else if constexpr (std::is_same_v<T, OrderCanceledEvent>) {
            auto c_opt = CheckedMath::safe_add_quantity(audits_[e.order_id].canceled_qty, e.canceled_quantity);
            audits_[e.order_id].canceled_qty = c_opt ? *c_opt : std::numeric_limits<Quantity>::max();
        }
    }, ev);
}

InvariantResult InvariantChecker::LifecycleAuditor::verify_reconciliation(const OrderBook& book) const {
    std::unordered_map<OrderId, Quantity> book_remaining;
    for (const auto& [price, level] : book.bids()) {
        for (const auto& order : level.orders) {
            auto cur = book_remaining[order.order_id];
            auto sum = CheckedMath::safe_add_quantity(cur, order.remaining_quantity);
            book_remaining[order.order_id] = sum ? *sum : std::numeric_limits<Quantity>::max();
        }
    }
    for (const auto& [price, level] : book.asks()) {
        for (const auto& order : level.orders) {
            auto cur = book_remaining[order.order_id];
            auto sum = CheckedMath::safe_add_quantity(cur, order.remaining_quantity);
            book_remaining[order.order_id] = sum ? *sum : std::numeric_limits<Quantity>::max();
        }
    }

    for (const auto& [order_id, audit] : audits_) {
        if (!audit.accepted) continue;

        Quantity remaining = 0;
        auto it = book_remaining.find(order_id);
        if (it != book_remaining.end()) {
            remaining = it->second;
        }

        auto part1 = CheckedMath::safe_add_quantity(audit.filled_qty, audit.canceled_qty);
        auto total_accounted = part1 ? CheckedMath::safe_add_quantity(*part1, remaining) : std::nullopt;

        if (!total_accounted || *total_accounted != audit.accepted_qty) {
            std::ostringstream ss;
            ss << "Order " << order_id << " lifecycle reconciliation failed: accepted="
               << audit.accepted_qty << ", filled=" << audit.filled_qty
               << ", canceled=" << audit.canceled_qty << ", remaining_in_book=" << remaining;
            return InvariantResult::fail(ss.str());
        }
    }

    return InvariantResult::pass();
}

void InvariantChecker::LifecycleAuditor::reset() {
    audits_.clear();
}

} // namespace marketlab
