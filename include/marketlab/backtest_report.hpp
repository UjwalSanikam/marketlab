#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include "types.hpp"
#include "sim_types.hpp"
#include "market_data_types.hpp"
#include "execution_model.hpp"

namespace marketlab {

enum class MarkoutStatus : uint8_t {
    Completed = 0,   // Target timestamp reached and valid mid mark price available
    Unfinished = 1,  // Target timestamp extends beyond end of session
    MissingMark = 2  // Target timestamp was reached, but no valid mid price (empty book/invalid feed)
};

[[nodiscard]] constexpr std::string_view to_string(MarkoutStatus s) noexcept {
    switch (s) {
        case MarkoutStatus::Completed: return "COMPLETED";
        case MarkoutStatus::Unfinished: return "UNFINISHED";
        case MarkoutStatus::MissingMark: return "MISSING_MARK";
    }
    return "UNKNOWN_MARKOUT_STATUS";
}

struct MarkoutPoint {
    SimTime horizon_ns{0};
    SimTime target_time{0};
    MarkoutStatus status{MarkoutStatus::Unfinished};
    std::optional<Price> mark_price;
    std::optional<Price> markout_ticks;
    std::optional<double> markout_bps;
};

struct TradeRecord {
    uint64_t trade_id{0};
    SimTime execution_time{0};
    OrderId order_id{0};
    Side side{Side::Buy};
    Price price{0};
    Quantity quantity{0};
    int64_t notional{0};
    int64_t fee{0};
    bool is_aggressive{false};
    std::vector<MarkoutPoint> markouts;
};

struct HorizonMarkoutSummary {
    SimTime horizon_ns{0};
    std::string horizon_label; // e.g. "100ms", "1s", "5s"
    size_t total_trades{0};
    size_t completed_count{0};
    size_t unfinished_count{0};
    size_t missing_mark_count{0};
    double mean_markout_ticks{0.0};
    double mean_markout_bps{0.0};
    double volume_weighted_markout_ticks{0.0};
    double volume_weighted_markout_bps{0.0};
};

struct EquityPoint {
    SimTime time{0};
    int64_t cash{0};
    Quantity inventory{0};
    std::optional<Price> mark_price;
    int64_t equity{0};
    int64_t drawdown{0};
};

struct BacktestReport {
    // Identity & Meta
    std::string strategy_name{"ReferenceStrategy"};
    AccountId strategy_account_id{"MM"};
    InstrumentMetadata instrument;
    ExecutionModelConfig execution_model;

    SimTime start_time_ns{0};
    SimTime end_time_ns{0};
    SimTime duration_ns{0};
    uint64_t total_events_processed{0};
    FeedStatus final_feed_status{FeedStatus::Valid};

    // Financial Performance
    int64_t initial_cash{0};
    int64_t final_cash{0};
    Quantity initial_inventory{0};
    Quantity final_inventory{0};
    int64_t starting_equity{0};
    int64_t ending_equity{0};
    int64_t gross_pnl{0};
    int64_t net_pnl{0};
    int64_t realized_pnl{0};
    int64_t unrealized_pnl{0};
    int64_t total_fees_paid{0};
    int64_t turnover{0}; // total notional traded

    // Orders & Trades
    size_t orders_submitted{0};
    size_t orders_accepted{0};
    size_t orders_rejected{0};
    size_t orders_canceled{0};
    size_t total_trades{0};
    size_t aggressive_trades{0};
    size_t passive_trades{0};
    Quantity total_order_qty_submitted{0};
    Quantity filled_quantity{0};
    double fill_rate_qty_pct{0.0};
    double fill_rate_orders_pct{0.0};

    // Inventory & Risk Profile
    Quantity min_inventory{0};
    Quantity max_inventory{0};
    Quantity peak_absolute_inventory{0};

    // Drawdown
    int64_t peak_equity{0};
    int64_t max_drawdown{0};
    double max_drawdown_pct{0.0};

    // Conservation Auditing
    bool cash_conserved{false};
    bool inventory_conserved{false};
    int64_t initial_system_cash{0};
    int64_t final_system_cash{0};
    Quantity initial_system_inventory{0};
    Quantity final_system_inventory{0};

    // Post-fill Markouts
    std::vector<HorizonMarkoutSummary> markout_summaries;
    std::vector<TradeRecord> trades;

    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::string to_string() const;
};

} // namespace marketlab
