#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <cstdint>

#include "types.hpp"
#include "sim_types.hpp"
#include "scheduler.hpp"
#include "order_book.hpp"
#include "portfolio.hpp"
#include "risk_engine.hpp"
#include "strategy.hpp"
#include "sha256.hpp"

namespace marketlab {

struct SimulationConfig {
    InstrumentConfig instrument;
    FeeConfig fees;
    LatencyConfig latencies;
    RiskLimits risk;
    MarketMakerConfig strategy_config;

    SimTime run_horizon_ns{10000000000ULL}; // 10 seconds default
    size_t max_events{500000};
    uint64_t seed{42};

    AccountId strategy_account_id{"MM"};
    int64_t strategy_starting_cash{1000000};       // 1,000,000 cash units
    Quantity strategy_starting_inventory{100};     // 100 lots

    AccountId background_account_id{"BACKGROUND"};
    int64_t background_starting_cash{10000000};    // 10,000,000 cash units
    Quantity background_starting_inventory{10000}; // 10,000 lots

    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] static SimulationConfig from_json(const std::string& json_str);
};

struct SimulationSummary {
    TerminationReason termination_reason{TerminationReason::None};
    SimTime simulated_time_ns{0};
    uint64_t total_events_processed{0};

    // Accounting checks
    bool cash_conserved{false};
    bool inventory_conserved{false};
    int64_t initial_total_cash{0};
    int64_t final_total_cash{0};
    Quantity initial_total_inventory{0};
    Quantity final_total_inventory{0};

    // Strategy results
    int64_t strategy_final_cash{0};
    Quantity strategy_final_inventory{0};
    int64_t strategy_realized_pnl{0};
    std::optional<int64_t> strategy_unrealized_pnl;
    std::optional<int64_t> strategy_total_pnl;
    int64_t strategy_fees_paid{0};
    size_t strategy_orders_submitted{0};
    size_t strategy_orders_accepted{0};
    size_t strategy_risk_rejections{0};
    size_t strategy_trades{0};

    // Exchange stats
    int64_t total_fees_collected{0};
    size_t total_trades{0};
    int64_t total_volume_traded{0};
    int64_t total_notional_traded{0};
};

class SimulationEngine : public IStrategyContext {
public:
    explicit SimulationEngine(SimulationConfig config);

    // Run the complete simulation session until horizon, event limit, or halt
    SimulationSummary run();

    // IStrategyContext implementations (called by strategy)
    void submit_order(Side side, OrderType type, Price price, Quantity qty, TimeInForce tif) override;
    void cancel_order(OrderId order_id) override;

    // External background orders injection
    void submit_background_order(const AccountId& account_id,
                                 Side side,
                                 OrderType type,
                                 Price price,
                                 Quantity qty,
                                 TimeInForce tif,
                                 SimTime arrival_time);

    void submit_background_cancel(const AccountId& account_id,
                                  OrderId order_id,
                                  SimTime arrival_time);

    // Schedule market observation arrival
    void emit_market_observation(SimTime time,
                                 std::optional<Price> best_bid,
                                 std::optional<Price> best_ask,
                                 std::optional<Price> mid_price,
                                 std::optional<Price> last_trade_price,
                                 Quantity last_trade_qty);

    // Populate scheduler with deterministic synthetic market activity
    void setup_synthetic_scenario();

    // Export canonical logs
    [[nodiscard]] std::string get_events_content() const;
    [[nodiscard]] std::string get_fills_content() const;
    [[nodiscard]] std::string get_state_content() const;
    [[nodiscard]] std::string get_manifest_content(const std::string& config_content) const;

    bool export_events_binary(const std::string& path) const;
    bool export_fills_binary(const std::string& path) const;
    bool export_state_binary(const std::string& path) const;
    bool export_manifest_binary(const std::string& path, const std::string& config_content) const;

    [[nodiscard]] const Ledger& ledger() const noexcept { return ledger_; }
    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const SimulationConfig& config() const noexcept { return config_; }

private:
    void process_order_admission(const AccountId& account_id,
                                 NewOrderCommand cmd,
                                 Price protection_price);

    void process_cancel_admission(const AccountId& account_id,
                                  CancelOrderCommand cmd);

    OrderId allocate_order_id() noexcept { return ++next_order_id_; }
    uint64_t allocate_trade_id() noexcept { return ++next_trade_id_; }

    SimulationConfig config_;
    VirtualScheduler scheduler_;
    OrderBook book_;
    Ledger ledger_;
    RiskEngine risk_engine_;
    std::unique_ptr<IStrategy> strategy_;

    OrderId next_order_id_{10000};
    uint64_t next_trade_id_{0};
    SeqNum next_cmd_seq_{0};

    SimTime last_market_data_time_{0};
    std::optional<Price> last_mark_price_;

    int64_t initial_total_cash_{0};
    Quantity initial_total_inventory_{0};

    // Tracking
    size_t strategy_orders_submitted_{0};
    size_t strategy_orders_accepted_{0};
    size_t strategy_risk_rejections_{0};
    size_t strategy_trades_{0};
    size_t total_trades_{0};
    int64_t total_volume_traded_{0};
    int64_t total_notional_traded_{0};

    std::vector<std::string> event_log_lines_;
    std::vector<TradeSettlement> fill_log_;
};

} // namespace marketlab
