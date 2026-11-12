#pragma once

#include <vector>
#include <string>
#include <memory>
#include <optional>
#include <algorithm>
#include <sstream>
#include <cstdint>

#include "types.hpp"
#include "sim_types.hpp"
#include "scheduler.hpp"
#include "portfolio.hpp"
#include "risk_engine.hpp"
#include "strategy.hpp"
#include "reference_strategy.hpp"
#include "market_data_types.hpp"
#include "market_data_normalizer.hpp"
#include "reconstructed_book.hpp"
#include "execution_model.hpp"
#include "historical_execution_adapter.hpp"
#include "backtest_report.hpp"

namespace marketlab {

struct BacktestConfig {
    InstrumentMetadata instrument;
    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    LatencyConfig latencies;
    RiskLimits risk{1000000, 1000000000000LL, 5000000};
    ReferenceStrategyConfig strategy_config;
    ExecutionModelConfig execution_model;
    AccountId strategy_account_id{"MM"};
    int64_t strategy_starting_cash{10000000000LL};    // In quote cents (e.g. 100M USDT)
    Quantity strategy_starting_inventory{100000};     // In lots (e.g. 1 BTC = 100k lots)
    SimTime run_horizon_ns{0};                       // 0 = all events
    std::vector<SimTime> markout_horizons{
        100000000ULL,   // 100ms
        1000000000ULL,  // 1s
        5000000000ULL   // 5s
    };
};

class BacktestEngine : public IStrategyContext {
public:
    explicit BacktestEngine(BacktestConfig config);

    // Load normalized historical events
    void load_events(std::vector<NormalizedEvent> events);

    // Set custom strategy implementation (defaults to ReferenceStrategy)
    void set_strategy(std::unique_ptr<IStrategy> strat) { strategy_ = std::move(strat); }

    // Run backtest to completion and return comprehensive report
    BacktestReport run();

    // IStrategyContext implementations
    void submit_order(Side side, OrderType type, Price price, Quantity qty, TimeInForce tif) override;
    void cancel_order(OrderId order_id) override;

    // Accessors
    [[nodiscard]] const ReconstructedOrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const Ledger& ledger() const noexcept { return ledger_; }
    [[nodiscard]] const HistoricalExecutionAdapter& execution_adapter() const noexcept { return execution_adapter_; }
    [[nodiscard]] const std::vector<TradeRecord>& recorded_trades() const noexcept { return recorded_trades_; }

    // Canonical Export Generation
    [[nodiscard]] std::string get_events_content() const;
    [[nodiscard]] std::string get_fills_content() const;
    [[nodiscard]] std::string get_state_content() const;
    [[nodiscard]] std::string get_manifest_content(const BacktestReport& report,
                                                   const std::string& events_hash,
                                                   const std::string& fills_hash,
                                                   const std::string& state_hash,
                                                   const std::string& report_hash) const;

private:
    void schedule_observation_delivery(SimTime time,
                                       std::optional<Price> best_bid,
                                       std::optional<Price> best_ask,
                                       std::optional<Price> mid_price,
                                       std::optional<Price> last_trade_price,
                                       Quantity last_trade_qty);

    void record_mid_price(SimTime time);
    void evaluate_equity(SimTime time);
    void record_trade(SimTime time, OrderId order_id, Side side, Price price, Quantity qty, int64_t fee, bool is_aggressive);
    void compute_markouts(SimTime session_end_time, BacktestReport& report);

    BacktestConfig config_;
    VirtualScheduler scheduler_;
    ReconstructedOrderBook book_;
    Ledger ledger_;
    RiskEngine risk_engine_;
    HistoricalExecutionAdapter execution_adapter_;
    std::unique_ptr<IStrategy> strategy_;

    std::vector<NormalizedEvent> loaded_events_;
    SimTime first_event_time_{0};
    SimTime last_event_time_{0};

    std::vector<std::pair<SimTime, std::optional<Price>>> mid_history_;
    std::vector<TradeRecord> recorded_trades_;
    uint64_t next_trade_id_{0};

    // Tracking & Metrics
    OrderId next_order_id_{0};
    SeqNum next_cmd_seq_{0};
    size_t orders_submitted_{0};
    Quantity total_order_qty_submitted_{0};

    int64_t init_system_cash_{0};
    Quantity init_system_inv_{0};
    int64_t starting_equity_{0};

    Quantity min_inventory_{0};
    Quantity max_inventory_{0};
    Quantity peak_abs_inventory_{0};
    int64_t peak_equity_{0};
    int64_t max_drawdown_{0};

    std::vector<std::string> event_log_lines_;
    std::vector<std::string> fill_log_lines_;
};

} // namespace marketlab
