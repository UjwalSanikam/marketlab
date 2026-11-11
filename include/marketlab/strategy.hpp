#pragma once

#include <optional>
#include <string>
#include <vector>
#include <cstdint>

#include "types.hpp"
#include "sim_types.hpp"

namespace marketlab {

struct MarketObservation {
    SimTime timestamp{0};
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;
    std::optional<Price> mid_price;
    std::optional<Price> last_trade_price;
    Quantity last_trade_qty{0};
};

struct ExecutionReport {
    enum class Type {
        Accepted,
        Rejected,
        Trade,
        Canceled
    } type{Type::Accepted};

    SimTime delivery_time{0};
    OrderId order_id{0};
    Side side{Side::Buy};
    Price price{0};
    Quantity quantity{0};
    Quantity remaining_quantity{0};
    std::string reason;
};

class IStrategyContext {
public:
    virtual ~IStrategyContext() = default;
    virtual void submit_order(Side side, OrderType type, Price price, Quantity qty, TimeInForce tif) = 0;
    virtual void cancel_order(OrderId order_id) = 0;
};

class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual void on_start(SimTime now, IStrategyContext& ctx) = 0;
    virtual void on_market_data(SimTime now, const MarketObservation& obs, IStrategyContext& ctx) = 0;
    virtual void on_execution_report(SimTime now, const ExecutionReport& report, IStrategyContext& ctx) = 0;
};

struct MarketMakerConfig {
    Quantity quote_size{5};
    Price half_spread{2};           // Distance from reservation price
    Quantity inventory_target{0};
    int64_t skew_per_lot{1};        // Price shift in ticks per unit inventory deviation
    SimTime quote_interval_ns{1000000000ULL}; // 1 second min requote interval
};

class InventoryAwareMarketMaker : public IStrategy {
public:
    explicit InventoryAwareMarketMaker(MarketMakerConfig config = {}) : config_(config) {}

    void on_start(SimTime now, IStrategyContext& ctx) override;
    void on_market_data(SimTime now, const MarketObservation& obs, IStrategyContext& ctx) override;
    void on_execution_report(SimTime now, const ExecutionReport& report, IStrategyContext& ctx) override;

    [[nodiscard]] Quantity observed_inventory() const noexcept { return observed_inventory_; }
    [[nodiscard]] size_t total_orders_placed() const noexcept { return total_orders_placed_; }
    [[nodiscard]] size_t total_trades_received() const noexcept { return total_trades_received_; }

private:
    MarketMakerConfig config_;
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
