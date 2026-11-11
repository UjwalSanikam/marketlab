#include "marketlab/strategy.hpp"
#include <algorithm>

namespace marketlab {

void InventoryAwareMarketMaker::on_start(SimTime, IStrategyContext&) {
    observed_inventory_ = 0;
    active_bid_id_ = std::nullopt;
    active_ask_id_ = std::nullopt;
    pending_bid_cancel_ = false;
    pending_ask_cancel_ = false;
    last_quote_time_ = 0;
    total_orders_placed_ = 0;
    total_trades_received_ = 0;
}

void InventoryAwareMarketMaker::on_market_data(SimTime now, const MarketObservation& obs, IStrategyContext& ctx) {
    if (!obs.mid_price) return;
    if (config_.quote_size <= 0) return;
    if (last_quote_time_ > 0 && now < last_quote_time_ + config_.quote_interval_ns) {
        return;
    }

    int64_t inv_delta = observed_inventory_ - config_.inventory_target;
    Price res_price = *obs.mid_price - (inv_delta * config_.skew_per_lot);
    if (res_price <= 0) res_price = 1;

    Price target_bid = std::max<Price>(1, res_price - config_.half_spread);
    Price target_ask = res_price + config_.half_spread;

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

void InventoryAwareMarketMaker::on_execution_report(SimTime, const ExecutionReport& report, IStrategyContext&) {
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
            }
            if (active_ask_id_ && *active_ask_id_ == report.order_id) {
                pending_ask_cancel_ = false;
            }
            break;
    }
}

} // namespace marketlab
