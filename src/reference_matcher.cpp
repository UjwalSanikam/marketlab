#include "marketlab/reference_matcher.hpp"
#include <map>

namespace marketlab {

void ReferenceMatcher::process(const Command& cmd, std::vector<Event>& out_events) {
    std::visit([this, &out_events](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, NewOrderCommand>) {
            this->process_new_order(c, out_events);
        } else if constexpr (std::is_same_v<T, CancelOrderCommand>) {
            this->process_cancel(c, out_events);
        }
    }, cmd);
}

void ReferenceMatcher::process_new_order(const NewOrderCommand& cmd, std::vector<Event>& out_events) {
    // 1. Check duplicate ID
    if (order_status_.find(cmd.order_id) != order_status_.end()) {
        out_events.push_back(OrderRejectedEvent{
            cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::DuplicateOrderId
        });
        return;
    }

    // 2. Validate quantity
    if (!CheckedMath::is_valid_quantity(cmd.quantity, config_.lot_size)) {
        out_events.push_back(OrderRejectedEvent{
            cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::InvalidQuantity
        });
        return;
    }

    // 3. Validate price
    if (cmd.type == OrderType::Limit) {
        if (!CheckedMath::is_valid_price(cmd.price, config_.tick_size)) {
            out_events.push_back(OrderRejectedEvent{
                cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::InvalidPrice
            });
            return;
        }
    } else if (cmd.type == OrderType::Market) {
        if (cmd.price != 0) {
            out_events.push_back(OrderRejectedEvent{
                cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::InvalidPrice
            });
            return;
        }
    }

    // 4. Validate TimeInForce
    if (cmd.tif != TimeInForce::GTC && cmd.tif != TimeInForce::IOC) {
        out_events.push_back(OrderRejectedEvent{
            cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::InvalidTimeInForce
        });
        return;
    }

    // 5. Overflow Pre-check for resting remainder (GTC limit orders)
    if (cmd.type == OrderType::Limit && cmd.tif == TimeInForce::GTC) {
        Quantity matchable = 0;
        Quantity total_side_qty = 0;
        Quantity current_lvl_qty = 0;

        for (const auto& o : resting_orders_) {
            if (cmd.side == Side::Buy) {
                if (o.side == Side::Sell && o.price <= cmd.price) {
                    matchable += o.remaining_quantity;
                }
                if (o.side == Side::Buy) {
                    total_side_qty += o.remaining_quantity;
                    if (o.price == cmd.price) {
                        current_lvl_qty += o.remaining_quantity;
                    }
                }
            } else {
                if (o.side == Side::Buy && o.price >= cmd.price) {
                    matchable += o.remaining_quantity;
                }
                if (o.side == Side::Sell) {
                    total_side_qty += o.remaining_quantity;
                    if (o.price == cmd.price) {
                        current_lvl_qty += o.remaining_quantity;
                    }
                }
            }
        }

        Quantity projected_remainder = (cmd.quantity > matchable) ? (cmd.quantity - matchable) : 0;
        if (projected_remainder > 0) {
            auto new_side_qty = CheckedMath::safe_add_quantity(total_side_qty, projected_remainder);
            if (!new_side_qty) {
                out_events.push_back(OrderRejectedEvent{
                    cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::QuantityOverflow
                });
                return;
            }
            auto new_lvl_qty = CheckedMath::safe_add_quantity(current_lvl_qty, projected_remainder);
            if (!new_lvl_qty) {
                out_events.push_back(OrderRejectedEvent{
                    cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::QuantityOverflow
                });
                return;
            }
        }
    }

    // 6. Accept
    order_status_[cmd.order_id] = StoredOrderStatus::Active;
    out_events.push_back(OrderAcceptedEvent{
        cmd.seq_num, next_event_id(), cmd.order_id, cmd.side, cmd.type, cmd.price, cmd.quantity, cmd.tif
    });

    Quantity remaining_qty = cmd.quantity;

    // 7. Brute-force price-time matching loop
    while (remaining_qty > 0) {
        size_t best_idx = size_t(-1);

        if (cmd.side == Side::Buy) {
            // Find lowest price sell order, earliest arrival
            for (size_t i = 0; i < resting_orders_.size(); ++i) {
                const auto& candidate = resting_orders_[i];
                if (candidate.side != Side::Sell) continue;
                if (cmd.type == OrderType::Limit && candidate.price > cmd.price) continue;

                if (best_idx == size_t(-1)) {
                    best_idx = i;
                } else {
                    const auto& current_best = resting_orders_[best_idx];
                    if (candidate.price < current_best.price) {
                        best_idx = i;
                    } else if (candidate.price == current_best.price && candidate.arrival_order < current_best.arrival_order) {
                        best_idx = i;
                    }
                }
            }
        } else { // Side::Sell
            // Find highest price buy order, earliest arrival
            for (size_t i = 0; i < resting_orders_.size(); ++i) {
                const auto& candidate = resting_orders_[i];
                if (candidate.side != Side::Buy) continue;
                if (cmd.type == OrderType::Limit && candidate.price < cmd.price) continue;

                if (best_idx == size_t(-1)) {
                    best_idx = i;
                } else {
                    const auto& current_best = resting_orders_[best_idx];
                    if (candidate.price > current_best.price) {
                        best_idx = i;
                    } else if (candidate.price == current_best.price && candidate.arrival_order < current_best.arrival_order) {
                        best_idx = i;
                    }
                }
            }
        }

        if (best_idx == size_t(-1)) {
            break; // No matching resting order available
        }

        auto& maker = resting_orders_[best_idx];
        Quantity trade_qty = std::min(remaining_qty, maker.remaining_quantity);
        Price trade_price = maker.price;

        remaining_qty -= trade_qty;
        maker.remaining_quantity -= trade_qty;

        out_events.push_back(TradeEvent{
            cmd.seq_num,
            next_event_id(),
            cmd.order_id,
            maker.order_id,
            trade_price,
            trade_qty,
            cmd.side,
            maker.remaining_quantity
        });

        if (maker.remaining_quantity == 0) {
            order_status_[maker.order_id] = StoredOrderStatus::Filled;
            resting_orders_.erase(resting_orders_.begin() + best_idx);
        }
    }

    // 8. Post-match handling
    if (remaining_qty == 0) {
        order_status_[cmd.order_id] = StoredOrderStatus::Filled;
    } else {
        if (cmd.type == OrderType::Market) {
            out_events.push_back(OrderCanceledEvent{
                cmd.seq_num, next_event_id(), cmd.order_id, remaining_qty, CancelReason::MarketRemainder
            });
            order_status_[cmd.order_id] = (remaining_qty == cmd.quantity)
                ? StoredOrderStatus::Canceled
                : StoredOrderStatus::Filled;
        } else if (cmd.tif == TimeInForce::IOC) {
            out_events.push_back(OrderCanceledEvent{
                cmd.seq_num, next_event_id(), cmd.order_id, remaining_qty, CancelReason::IocRemainder
            });
            order_status_[cmd.order_id] = (remaining_qty == cmd.quantity)
                ? StoredOrderStatus::Canceled
                : StoredOrderStatus::Filled;
        } else {
            // GTC limit rests in the book
            resting_orders_.push_back(ReferenceRestingOrder{
                cmd.order_id,
                cmd.seq_num,
                cmd.side,
                cmd.price,
                cmd.quantity,
                remaining_qty,
                cmd.tif,
                ++arrival_counter_
            });
        }
    }
}

void ReferenceMatcher::process_cancel(const CancelOrderCommand& cmd, std::vector<Event>& out_events) {
    auto status_it = order_status_.find(cmd.order_id);
    if (status_it == order_status_.end()) {
        out_events.push_back(OrderRejectedEvent{
            cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::UnknownOrder
        });
        return;
    }

    if (status_it->second != StoredOrderStatus::Active) {
        out_events.push_back(OrderRejectedEvent{
            cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::OrderAlreadyFilledOrCancelled
        });
        return;
    }

    auto it = std::find_if(resting_orders_.begin(), resting_orders_.end(), [&](const auto& o) {
        return o.order_id == cmd.order_id;
    });

    if (it == resting_orders_.end()) {
        out_events.push_back(OrderRejectedEvent{
            cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::UnknownOrder
        });
        return;
    }

    Quantity canceled_qty = it->remaining_quantity;
    resting_orders_.erase(it);
    status_it->second = StoredOrderStatus::Canceled;

    out_events.push_back(OrderCanceledEvent{
        cmd.seq_num, next_event_id(), cmd.order_id, canceled_qty, CancelReason::UserRequested
    });
}

BookSnapshot ReferenceMatcher::get_snapshot(size_t max_depth) const {
    BookSnapshot snap;

    std::map<Price, std::pair<Quantity, size_t>, std::greater<Price>> bid_map;
    std::map<Price, std::pair<Quantity, size_t>, std::less<Price>> ask_map;

    for (const auto& o : resting_orders_) {
        if (o.side == Side::Buy) {
            bid_map[o.price].first += o.remaining_quantity;
            bid_map[o.price].second += 1;
            snap.total_bid_quantity += o.remaining_quantity;
        } else {
            ask_map[o.price].first += o.remaining_quantity;
            ask_map[o.price].second += 1;
            snap.total_ask_quantity += o.remaining_quantity;
        }
    }

    size_t count = 0;
    for (const auto& [price, pair] : bid_map) {
        if (max_depth > 0 && count >= max_depth) break;
        snap.bids.push_back(LevelInfo{price, pair.first, pair.second});
        ++count;
    }

    count = 0;
    for (const auto& [price, pair] : ask_map) {
        if (max_depth > 0 && count >= max_depth) break;
        snap.asks.push_back(LevelInfo{price, pair.first, pair.second});
        ++count;
    }

    return snap;
}

void ReferenceMatcher::reset() {
    resting_orders_.clear();
    order_status_.clear();
    next_event_id_ = 0;
    arrival_counter_ = 0;
}

} // namespace marketlab
