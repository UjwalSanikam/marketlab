#include "marketlab/reference_strategy.hpp"
#include <algorithm>
#include <cstdlib>

namespace marketlab {

void ReferenceStrategy::on_start(SimTime, IStrategyContext&) {
    observed_inventory_ = 0;
    active_bid_id_ = std::nullopt;
    active_ask_id_ = std::nullopt;
    current_bid_price_ = 0;
    current_ask_price_ = 0;
    pending_bid_cancel_ = false;
    pending_ask_cancel_ = false;
    last_quote_time_ = 0;
    total_orders_placed_ = 0;
    total_trades_received_ = 0;
}

void ReferenceStrategy::on_market_data(SimTime now, const MarketObservation& obs, IStrategyContext& ctx) {
    if (!obs.mid_price) return;
    if (config_.quote_size <= 0) return;
    if (last_quote_time_ > 0 && now < last_quote_time_ + config_.quote_interval_ns) {
        return;
    }

    int64_t inv_delta = observed_inventory_ - config_.inventory_target;

    if (config_.mode == ReferenceStrategyMode::Taker) {
        if (inv_delta < 0 && obs.best_ask) {
            Quantity buy_qty = std::min<Quantity>(config_.quote_size, static_cast<Quantity>(-inv_delta));
            ctx.submit_order(Side::Buy, OrderType::Limit, *obs.best_ask, buy_qty, TimeInForce::IOC);
            ++total_orders_placed_;
            last_quote_time_ = now;
        } else if (inv_delta > 0 && obs.best_bid) {
            Quantity sell_qty = std::min<Quantity>(config_.quote_size, static_cast<Quantity>(inv_delta));
            ctx.submit_order(Side::Sell, OrderType::Limit, *obs.best_bid, sell_qty, TimeInForce::IOC);
            ++total_orders_placed_;
            last_quote_time_ = now;
        } else if (obs.last_trade_price) {
            // Signal-based taker entry
            if (*obs.last_trade_price > *obs.mid_price && obs.best_ask) {
                ctx.submit_order(Side::Buy, OrderType::Limit, *obs.best_ask, config_.quote_size, TimeInForce::IOC);
                ++total_orders_placed_;
                last_quote_time_ = now;
            } else if (*obs.last_trade_price < *obs.mid_price && obs.best_bid) {
                ctx.submit_order(Side::Sell, OrderType::Limit, *obs.best_bid, config_.quote_size, TimeInForce::IOC);
                ++total_orders_placed_;
                last_quote_time_ = now;
            }
        }
        return;
    }

    if (config_.mode == ReferenceStrategyMode::Hybrid) {
        if (std::abs(inv_delta) >= config_.rebalance_threshold) {
            // Cancel resting passive quotes first to avoid self-trades or unwanted fills
            if (active_bid_id_ && !pending_bid_cancel_) {
                ctx.cancel_order(*active_bid_id_);
                pending_bid_cancel_ = true;
            }
            if (active_ask_id_ && !pending_ask_cancel_) {
                ctx.cancel_order(*active_ask_id_);
                pending_ask_cancel_ = true;
            }

            Quantity rebal_qty = std::min<Quantity>(config_.quote_size, static_cast<Quantity>(std::abs(inv_delta)));
            if (inv_delta < 0 && obs.best_ask) {
                ctx.submit_order(Side::Buy, OrderType::Limit, *obs.best_ask, rebal_qty, TimeInForce::IOC);
                ++total_orders_placed_;
                last_quote_time_ = now;
                return;
            } else if (inv_delta > 0 && obs.best_bid) {
                ctx.submit_order(Side::Sell, OrderType::Limit, *obs.best_bid, rebal_qty, TimeInForce::IOC);
                ++total_orders_placed_;
                last_quote_time_ = now;
                return;
            }
        }
    }

    // MarketMaker mode (and normal quoting in Hybrid mode)
    Price res_price = *obs.mid_price - (inv_delta * config_.skew_per_lot);
    if (res_price <= 0) res_price = 1;

    Price target_bid = std::max<Price>(1, res_price - config_.half_spread);
    Price target_ask = res_price + config_.half_spread;
    if (target_ask <= target_bid) {
        target_ask = target_bid + 1;
    }

    // Manage Bid quote
    if (active_bid_id_) {
        if (current_bid_price_ != target_bid && !pending_bid_cancel_) {
            ctx.cancel_order(*active_bid_id_);
            pending_bid_cancel_ = true;
        }
    } else if (!pending_bid_cancel_) {
        ctx.submit_order(Side::Buy, OrderType::Limit, target_bid, config_.quote_size, TimeInForce::GTC);
        current_bid_price_ = target_bid;
        ++total_orders_placed_;
    }

    // Manage Ask quote
    if (active_ask_id_) {
        if (current_ask_price_ != target_ask && !pending_ask_cancel_) {
            ctx.cancel_order(*active_ask_id_);
            pending_ask_cancel_ = true;
        }
    } else if (!pending_ask_cancel_) {
        ctx.submit_order(Side::Sell, OrderType::Limit, target_ask, config_.quote_size, TimeInForce::GTC);
        current_ask_price_ = target_ask;
        ++total_orders_placed_;
    }

    last_quote_time_ = now;
}

void ReferenceStrategy::on_execution_report(SimTime, const ExecutionReport& report, IStrategyContext&) {
    switch (report.type) {
        case ExecutionReport::Type::Accepted:
            if (report.side == Side::Buy) {
                active_bid_id_ = report.order_id;
            } else {
                active_ask_id_ = report.order_id;
            }
            break;

        case ExecutionReport::Type::Canceled:
            if (active_bid_id_ && *active_bid_id_ == report.order_id) {
                active_bid_id_ = std::nullopt;
                pending_bid_cancel_ = false;
            }
            if (active_ask_id_ && *active_ask_id_ == report.order_id) {
                active_ask_id_ = std::nullopt;
                pending_ask_cancel_ = false;
            }
            break;

        case ExecutionReport::Type::Trade:
            ++total_trades_received_;
            if (report.side == Side::Buy) {
                observed_inventory_ += report.quantity;
            } else {
                observed_inventory_ -= report.quantity;
            }
            if (report.remaining_quantity == 0) {
                if (active_bid_id_ && *active_bid_id_ == report.order_id) {
                    active_bid_id_ = std::nullopt;
                }
                if (active_ask_id_ && *active_ask_id_ == report.order_id) {
                    active_ask_id_ = std::nullopt;
                }
            }
            break;

        case ExecutionReport::Type::Rejected:
            if (active_bid_id_ && *active_bid_id_ == report.order_id) {
                pending_bid_cancel_ = false;
                active_bid_id_ = std::nullopt;
            }
            if (active_ask_id_ && *active_ask_id_ == report.order_id) {
                pending_ask_cancel_ = false;
                active_ask_id_ = std::nullopt;
            }
            break;
    }
}

} // namespace marketlab
