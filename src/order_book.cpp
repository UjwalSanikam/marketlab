#include "marketlab/order_book.hpp"
#include <algorithm>

namespace marketlab {

void OrderBook::process(const Command& cmd, std::vector<Event>& out_events) {
    std::visit([this, &out_events](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, NewOrderCommand>) {
            this->process_new_order(c, out_events);
        } else if constexpr (std::is_same_v<T, CancelOrderCommand>) {
            this->process_cancel(c, out_events);
        }
    }, cmd);
}

void OrderBook::process_new_order(const NewOrderCommand& cmd, std::vector<Event>& out_events) {
    // 1. Check duplicate order ID
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
        // Market orders carry no limit price (must be 0)
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

    // 5. Overflow Pre-check for resting residual:
    // Determine exact matchable quantity to check if resting remainder would overflow side or level totals.
    if (cmd.type == OrderType::Limit && cmd.tif == TimeInForce::GTC) {
        Quantity matchable = 0;
        if (cmd.side == Side::Buy) {
            for (const auto& [ask_price, level] : asks_) {
                if (ask_price > cmd.price) break;
                matchable += level.total_quantity;
                if (matchable >= cmd.quantity) break;
            }
            Quantity projected_remainder = (cmd.quantity > matchable) ? (cmd.quantity - matchable) : 0;
            if (projected_remainder > 0) {
                auto new_total_bid = CheckedMath::safe_add_quantity(total_bid_qty_, projected_remainder);
                if (!new_total_bid) {
                    out_events.push_back(OrderRejectedEvent{
                        cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::QuantityOverflow
                    });
                    return;
                }
                Quantity current_lvl_qty = 0;
                auto lvl_it = bids_.find(cmd.price);
                if (lvl_it != bids_.end()) {
                    current_lvl_qty = lvl_it->second.total_quantity;
                }
                auto new_lvl_qty = CheckedMath::safe_add_quantity(current_lvl_qty, projected_remainder);
                if (!new_lvl_qty) {
                    out_events.push_back(OrderRejectedEvent{
                        cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::QuantityOverflow
                    });
                    return;
                }
            }
        } else { // Side::Sell
            for (const auto& [bid_price, level] : bids_) {
                if (bid_price < cmd.price) break;
                matchable += level.total_quantity;
                if (matchable >= cmd.quantity) break;
            }
            Quantity projected_remainder = (cmd.quantity > matchable) ? (cmd.quantity - matchable) : 0;
            if (projected_remainder > 0) {
                auto new_total_ask = CheckedMath::safe_add_quantity(total_ask_qty_, projected_remainder);
                if (!new_total_ask) {
                    out_events.push_back(OrderRejectedEvent{
                        cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::QuantityOverflow
                    });
                    return;
                }
                Quantity current_lvl_qty = 0;
                auto lvl_it = asks_.find(cmd.price);
                if (lvl_it != asks_.end()) {
                    current_lvl_qty = lvl_it->second.total_quantity;
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
    }

    // 6. Order accepted
    order_status_[cmd.order_id] = StoredOrderStatus::Active;
    out_events.push_back(OrderAcceptedEvent{
        cmd.seq_num, next_event_id(), cmd.order_id, cmd.side, cmd.type, cmd.price, cmd.quantity, cmd.tif
    });

    Quantity remaining_qty = cmd.quantity;

    // 7. Matching logic
    if (cmd.side == Side::Buy) {
        while (remaining_qty > 0 && !asks_.empty()) {
            auto best_ask_it = asks_.begin();
            Price ask_price = best_ask_it->first;

            if (cmd.type == OrderType::Limit && cmd.price < ask_price) {
                break; // Limit price does not cross
            }

            auto& level = best_ask_it->second;
            auto order_it = level.orders.begin();

            while (remaining_qty > 0 && order_it != level.orders.end()) {
                auto& maker = *order_it;
                Quantity trade_qty = std::min(remaining_qty, maker.remaining_quantity);
                Price trade_price = maker.price; // Trade at resting order's price

                remaining_qty -= trade_qty;
                maker.remaining_quantity -= trade_qty;
                level.total_quantity -= trade_qty;
                total_ask_qty_ -= trade_qty;

                out_events.push_back(TradeEvent{
                    cmd.seq_num,
                    next_event_id(),
                    cmd.order_id,
                    maker.order_id,
                    trade_price,
                    trade_qty,
                    Side::Buy,
                    maker.remaining_quantity
                });

                if (maker.remaining_quantity == 0) {
                    order_status_[maker.order_id] = StoredOrderStatus::Filled;
                    order_index_.erase(maker.order_id);
                    order_it = level.orders.erase(order_it);
                } else {
                    ++order_it;
                }
            }

            if (level.orders.empty()) {
                asks_.erase(best_ask_it);
            }
        }
    } else { // Side::Sell
        while (remaining_qty > 0 && !bids_.empty()) {
            auto best_bid_it = bids_.begin();
            Price bid_price = best_bid_it->first;

            if (cmd.type == OrderType::Limit && cmd.price > bid_price) {
                break; // Limit price does not cross
            }

            auto& level = best_bid_it->second;
            auto order_it = level.orders.begin();

            while (remaining_qty > 0 && order_it != level.orders.end()) {
                auto& maker = *order_it;
                Quantity trade_qty = std::min(remaining_qty, maker.remaining_quantity);
                Price trade_price = maker.price; // Trade at resting order's price

                remaining_qty -= trade_qty;
                maker.remaining_quantity -= trade_qty;
                level.total_quantity -= trade_qty;
                total_bid_qty_ -= trade_qty;

                out_events.push_back(TradeEvent{
                    cmd.seq_num,
                    next_event_id(),
                    cmd.order_id,
                    maker.order_id,
                    trade_price,
                    trade_qty,
                    Side::Sell,
                    maker.remaining_quantity
                });

                if (maker.remaining_quantity == 0) {
                    order_status_[maker.order_id] = StoredOrderStatus::Filled;
                    order_index_.erase(maker.order_id);
                    order_it = level.orders.erase(order_it);
                } else {
                    ++order_it;
                }
            }

            if (level.orders.empty()) {
                bids_.erase(best_bid_it);
            }
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
            // GTC Limit order: rests on the book
            if (cmd.side == Side::Buy) {
                auto& level = bids_[cmd.price];
                level.price = cmd.price;
                level.total_quantity += remaining_qty;
                total_bid_qty_ += remaining_qty;

                level.orders.push_back(RestingOrder{
                    cmd.order_id,
                    cmd.seq_num,
                    cmd.side,
                    cmd.price,
                    cmd.quantity,
                    remaining_qty,
                    cmd.tif
                });

                auto it = std::prev(level.orders.end());
                order_index_[cmd.order_id] = IndexEntry{cmd.price, cmd.side, it};
            } else {
                auto& level = asks_[cmd.price];
                level.price = cmd.price;
                level.total_quantity += remaining_qty;
                total_ask_qty_ += remaining_qty;

                level.orders.push_back(RestingOrder{
                    cmd.order_id,
                    cmd.seq_num,
                    cmd.side,
                    cmd.price,
                    cmd.quantity,
                    remaining_qty,
                    cmd.tif
                });

                auto it = std::prev(level.orders.end());
                order_index_[cmd.order_id] = IndexEntry{cmd.price, cmd.side, it};
            }
        }
    }
}

void OrderBook::process_cancel(const CancelOrderCommand& cmd, std::vector<Event>& out_events) {
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

    auto idx_it = order_index_.find(cmd.order_id);
    if (idx_it == order_index_.end()) {
        out_events.push_back(OrderRejectedEvent{
            cmd.seq_num, next_event_id(), cmd.order_id, RejectReason::UnknownOrder
        });
        return;
    }

    const auto entry = idx_it->second;
    const Quantity canceled_qty = entry.iter->remaining_quantity;

    if (entry.side == Side::Buy) {
        auto level_it = bids_.find(entry.price);
        if (level_it != bids_.end()) {
            level_it->second.total_quantity -= canceled_qty;
            total_bid_qty_ -= canceled_qty;
            level_it->second.orders.erase(entry.iter);
            if (level_it->second.orders.empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(entry.price);
        if (level_it != asks_.end()) {
            level_it->second.total_quantity -= canceled_qty;
            total_ask_qty_ -= canceled_qty;
            level_it->second.orders.erase(entry.iter);
            if (level_it->second.orders.empty()) {
                asks_.erase(level_it);
            }
        }
    }

    order_index_.erase(idx_it);
    status_it->second = StoredOrderStatus::Canceled;

    out_events.push_back(OrderCanceledEvent{
        cmd.seq_num, next_event_id(), cmd.order_id, canceled_qty, CancelReason::UserRequested
    });
}

std::optional<Price> OrderBook::best_bid() const noexcept {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const noexcept {
    auto bb = best_bid();
    auto ba = best_ask();
    if (bb && ba) {
        return *ba - *bb;
    }
    return std::nullopt;
}

BookSnapshot OrderBook::get_snapshot(size_t max_depth) const {
    BookSnapshot snap;
    snap.total_bid_quantity = total_bid_qty_;
    snap.total_ask_quantity = total_ask_qty_;

    size_t count = 0;
    for (const auto& [price, level] : bids_) {
        if (max_depth > 0 && count >= max_depth) break;
        snap.bids.push_back(LevelInfo{price, level.total_quantity, level.orders.size()});
        ++count;
    }

    count = 0;
    for (const auto& [price, level] : asks_) {
        if (max_depth > 0 && count >= max_depth) break;
        snap.asks.push_back(LevelInfo{price, level.total_quantity, level.orders.size()});
        ++count;
    }

    return snap;
}

void OrderBook::reset() {
    bids_.clear();
    asks_.clear();
    total_bid_qty_ = 0;
    total_ask_qty_ = 0;
    order_index_.clear();
    order_status_.clear();
    next_event_id_ = 0;
}

} // namespace marketlab
