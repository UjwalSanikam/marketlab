#pragma once

#include <vector>
#include <string>
#include <memory>
#include <optional>
#include <sstream>

#include "types.hpp"
#include "sim_types.hpp"
#include "scheduler.hpp"
#include "portfolio.hpp"
#include "risk_engine.hpp"
#include "strategy.hpp"
#include "market_data_types.hpp"
#include "market_data_normalizer.hpp"
#include "reconstructed_book.hpp"
#include "execution_model.hpp"
#include "historical_execution_adapter.hpp"

namespace marketlab {

struct HistoricalReplayConfig {
    InstrumentMetadata instrument;
    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    LatencyConfig latencies;
    RiskLimits risk;
    MarketMakerConfig strategy_config;
    AccountId strategy_account_id{"MM"};
    int64_t strategy_starting_cash{100000000LL}; // In quote cents (e.g. 1M USDT = 100M cents)
    Quantity strategy_starting_inventory{100000}; // In lots (e.g. 1 BTC = 100,000 lots)
    SimTime run_horizon_ns{0}; // 0 = run until all events consumed
    ExecutionModelConfig execution_model;
};

struct HistoricalReplaySummary {
    SimTime simulated_time_ns{0};
    TerminationReason termination_reason{TerminationReason::AllEventsProcessed};
    uint64_t total_events_processed{0};

    // Reconstructed Book Diagnostics
    FeedStatus final_feed_status{FeedStatus::Valid};
    uint64_t final_update_id{0};
    size_t bid_depth_levels{0};
    size_t ask_depth_levels{0};
    std::optional<Price> best_bid;
    std::optional<Price> best_ask;

    // Strategy Performance
    size_t strategy_orders_submitted{0};
    size_t strategy_orders_accepted{0};
    size_t strategy_risk_rejections{0};
    size_t strategy_orders_canceled{0};
    size_t strategy_aggressive_trades{0};
    size_t strategy_passive_trades{0};
    size_t strategy_total_trades{0};
    Quantity strategy_volume_traded{0};
    int64_t strategy_notional_traded{0};
    int64_t strategy_fees_paid{0};

    int64_t strategy_final_cash{0};
    Quantity strategy_final_inventory{0};
    int64_t strategy_realized_pnl{0};
    std::optional<int64_t> strategy_unrealized_pnl;
    std::optional<int64_t> strategy_total_pnl;

    // Double-entry Ledger Conservation
    bool cash_conserved{false};
    bool inventory_conserved{false};
    int64_t initial_total_cash{0};
    int64_t final_total_cash{0};
    Quantity initial_total_inventory{0};
    Quantity final_total_inventory{0};
};

class HistoricalReplayEngine : public IStrategyContext {
public:
    explicit HistoricalReplayEngine(HistoricalReplayConfig config);

    // Load normalized historical events
    void load_events(std::vector<NormalizedEvent> events);

    // Run replay to completion
    HistoricalReplaySummary run();

    // IStrategyContext implementations
    void submit_order(Side side, OrderType type, Price price, Quantity qty, TimeInForce tif) override;
    void cancel_order(OrderId order_id) override;

    // Set custom strategy
    void set_strategy(std::unique_ptr<IStrategy> strat) { strategy_ = std::move(strat); }

    // Accessors
    [[nodiscard]] const ReconstructedOrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const Ledger& ledger() const noexcept { return ledger_; }
    [[nodiscard]] const HistoricalExecutionAdapter& execution_adapter() const noexcept { return execution_adapter_; }

    // Export canonical logs
    [[nodiscard]] std::string get_events_content() const;
    [[nodiscard]] std::string get_fills_content() const;
    [[nodiscard]] std::string get_state_content() const;
    [[nodiscard]] std::string get_manifest_content(const std::string& raw_input_hash) const;

    bool export_events_binary(const std::string& path) const;
    bool export_fills_binary(const std::string& path) const;
    bool export_state_binary(const std::string& path) const;
    bool export_manifest_binary(const std::string& path, const std::string& raw_input_hash) const;

private:
    void process_strategy_order_admission(NewOrderCommand cmd, Price protection_price);
    void process_strategy_cancel_admission(CancelOrderCommand cmd);
    void schedule_observation_delivery(SimTime time,
                                       std::optional<Price> best_bid,
                                       std::optional<Price> best_ask,
                                       std::optional<Price> mid_price,
                                       std::optional<Price> last_trade_price,
                                       Quantity last_trade_qty);

    HistoricalReplayConfig config_;
    VirtualScheduler scheduler_;
    ReconstructedOrderBook book_;
    Ledger ledger_;
    RiskEngine risk_engine_;
    HistoricalExecutionAdapter execution_adapter_;
    std::unique_ptr<IStrategy> strategy_;

    std::vector<NormalizedEvent> loaded_events_;
    std::vector<std::string> event_log_lines_;
    std::vector<std::string> fill_log_lines_;

    OrderId next_order_id_{20000};
    SeqNum next_cmd_seq_{0};
    SimTime first_event_time_{0};
    SimTime last_event_time_{0};
};

} // namespace marketlab
