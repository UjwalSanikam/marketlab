#include "marketlab/workload_generator.hpp"
#include <algorithm>

namespace marketlab {

std::vector<Command> WorkloadGenerator::generate() {
    std::vector<Command> commands;
    commands.reserve(config_.count);

    std::vector<OrderId> active_order_ids;
    std::vector<OrderId> historical_order_ids;

    SeqNum seq_num = 1;
    OrderId next_id = 1000;
    Price current_mid = config_.mid_price;

    std::uniform_int_distribution<int> pct_dist(0, 99);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> offset_dist(1, config_.max_price_offset_ticks);
    std::uniform_int_distribution<int> drift_dist(-config_.max_random_walk_step, config_.max_random_walk_step);
    std::uniform_int_distribution<int64_t> qty_dist(config_.min_quantity, config_.max_quantity);

    for (size_t i = 0; i < config_.count; ++i) {
        // Random walk the mid price
        current_mid += drift_dist(rng_);
        if (current_mid < 100) current_mid = 100;

        // Anomaly injection for testing robustness
        if (config_.inject_anomalies && pct_dist(rng_) < 4) {
            int anomaly_type = pct_dist(rng_) % 4;
            if (anomaly_type == 0 && !historical_order_ids.empty()) {
                // Duplicate order ID
                std::uniform_int_distribution<size_t> hist_idx(0, historical_order_ids.size() - 1);
                OrderId dup_id = historical_order_ids[hist_idx(rng_)];
                commands.push_back(NewOrderCommand{
                    seq_num++, dup_id, Side::Buy, OrderType::Limit, current_mid, 10, TimeInForce::GTC
                });
            } else if (anomaly_type == 1) {
                // Invalid price (zero or negative)
                commands.push_back(NewOrderCommand{
                    seq_num++, next_id++, Side::Buy, OrderType::Limit, 0, 10, TimeInForce::GTC
                });
            } else if (anomaly_type == 2) {
                // Invalid quantity (zero)
                commands.push_back(NewOrderCommand{
                    seq_num++, next_id++, Side::Sell, OrderType::Limit, current_mid + 5, 0, TimeInForce::GTC
                });
            } else {
                // Cancel unknown order
                commands.push_back(CancelOrderCommand{
                    seq_num++, 99999999
                });
            }
            continue;
        }

        int roll = pct_dist(rng_);

        // Cancel command
        if (roll < config_.cancel_pct && !active_order_ids.empty()) {
            std::uniform_int_distribution<size_t> cancel_idx_dist(0, active_order_ids.size() - 1);
            size_t idx = cancel_idx_dist(rng_);
            OrderId target_id = active_order_ids[idx];

            // Remove from active list
            active_order_ids[idx] = active_order_ids.back();
            active_order_ids.pop_back();

            commands.push_back(CancelOrderCommand{
                seq_num++,
                target_id
            });
            continue;
        }

        // Market order
        if (roll < (config_.cancel_pct + config_.market_order_pct)) {
            Side side = (side_dist(rng_) == 0) ? Side::Buy : Side::Sell;
            Quantity qty = qty_dist(rng_) * config_.lot_size;
            OrderId id = next_id++;
            historical_order_ids.push_back(id);

            commands.push_back(NewOrderCommand{
                seq_num++,
                id,
                side,
                OrderType::Market,
                0, // Market orders carry price 0
                qty,
                TimeInForce::IOC
            });
            continue;
        }

        // Limit order
        Side side = (side_dist(rng_) == 0) ? Side::Buy : Side::Sell;
        int offset = offset_dist(rng_);
        // Some orders are placed aggressively (crossing inside spread) to generate trades
        bool aggressive = (pct_dist(rng_) < 35);

        Price price = 0;
        if (side == Side::Buy) {
            price = aggressive ? (current_mid + offset / 2) : (current_mid - offset);
        } else {
            price = aggressive ? (current_mid - offset / 2) : (current_mid + offset);
        }

        // Ensure price is positive and aligned
        if (price <= 0) price = config_.tick_size;
        price = (price / config_.tick_size) * config_.tick_size;

        Quantity qty = qty_dist(rng_) * config_.lot_size;
        bool is_ioc = (pct_dist(rng_) < config_.ioc_limit_pct);
        TimeInForce tif = is_ioc ? TimeInForce::IOC : TimeInForce::GTC;

        OrderId id = next_id++;
        historical_order_ids.push_back(id);
        if (tif == TimeInForce::GTC) {
            active_order_ids.push_back(id);
            // Cap active list to keep cancel pool reasonable
            if (active_order_ids.size() > 5000) {
                active_order_ids.erase(active_order_ids.begin(), active_order_ids.begin() + 1000);
            }
        }

        commands.push_back(NewOrderCommand{
            seq_num++,
            id,
            side,
            OrderType::Limit,
            price,
            qty,
            tif
        });
    }

    return commands;
}

} // namespace marketlab
