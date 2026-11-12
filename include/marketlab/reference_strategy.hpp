#pragma once

#include <optional>
#include <string>
#include <cstdint>

#include "types.hpp"
#include "sim_types.hpp"
#include "strategy.hpp"

namespace marketlab {

enum class ReferenceStrategyMode : uint8_t {
    MarketMaker = 0,
    Taker = 1,
    Hybrid = 2
};

[[nodiscard]] constexpr std::string_view to_string(ReferenceStrategyMode m) noexcept {
    switch (m) {
        case ReferenceStrategyMode::MarketMaker: return "MARKET_MAKER";
        case ReferenceStrategyMode::Taker: return "TAKER";
        case ReferenceStrategyMode::Hybrid: return "HYBRID";
    }
    return "UNKNOWN_MODE";
}

struct ReferenceStrategyConfig {
    ReferenceStrategyMode mode{ReferenceStrategyMode::Hybrid};
    Quantity quote_size{10000};             // Quote lot size (e.g. 0.1 BTC = 10,000 lots)
    Price half_spread{200};                 // 2.00 USDT half-spread (200 ticks)
    Quantity inventory_target{0};           // Target position
    int64_t skew_per_lot{1};                // Reservation price shift per lot deviation
    Quantity rebalance_threshold{30000};    // Inventory deviation triggering aggressive rebalance
    SimTime quote_interval_ns{100000000ULL};// 100ms min requote interval
};

class ReferenceStrategy : public IStrategy {
public:
    explicit ReferenceStrategy(ReferenceStrategyConfig config = {}) : config_(config) {}

    void on_start(SimTime now, IStrategyContext& ctx) override;
    void on_market_data(SimTime now, const MarketObservation& obs, IStrategyContext& ctx) override;
    void on_execution_report(SimTime now, const ExecutionReport& report, IStrategyContext& ctx) override;

    [[nodiscard]] Quantity observed_inventory() const noexcept { return observed_inventory_; }
    [[nodiscard]] size_t total_orders_placed() const noexcept { return total_orders_placed_; }
    [[nodiscard]] size_t total_trades_received() const noexcept { return total_trades_received_; }
    [[nodiscard]] const ReferenceStrategyConfig& config() const noexcept { return config_; }

private:
    ReferenceStrategyConfig config_;
    Quantity observed_inventory_{0};

    std::optional<OrderId> active_bid_id_;
    std::optional<OrderId> active_ask_id_;
    Price current_bid_price_{0};
    Price current_ask_price_{0};

    bool pending_bid_cancel_{false};
    bool pending_ask_cancel_{false};

    SimTime last_quote_time_{0};
    size_t total_orders_placed_{0};
    size_t total_trades_received_{0};
};

} // namespace marketlab
