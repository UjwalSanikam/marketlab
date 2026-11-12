#include "marketlab/backtest_report.hpp"
#include <iomanip>
#include <sstream>

namespace marketlab {

std::string BacktestReport::to_json() const {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4);
    ss << "{\n"
       << "  \"schema_version\": \"marketlab.backtest.report.v1\",\n"
       << "  \"strategy\": {\n"
       << "    \"name\": \"" << strategy_name << "\",\n"
       << "    \"account_id\": \"" << strategy_account_id << "\"\n"
       << "  },\n"
       << "  \"instrument\": {\n"
       << "    \"symbol\": \"" << instrument.symbol << "\",\n"
       << "    \"price_scale\": " << instrument.price_scale << ",\n"
       << "    \"qty_scale\": " << instrument.qty_scale << ",\n"
       << "    \"tick_size\": " << instrument.tick_size << ",\n"
       << "    \"lot_size\": " << instrument.lot_size << "\n"
       << "  },\n"
       << "  \"execution_model\": {\n"
       << "    \"queue_model\": \"" << ::marketlab::to_string(execution_model.queue_model) << "\",\n"
       << "    \"queue_position_pct\": " << execution_model.queue_position_pct << ",\n"
       << "    \"impact_model\": \"" << ::marketlab::to_string(execution_model.impact_model) << "\",\n"
       << "    \"fixed_slippage_ticks\": " << execution_model.fixed_slippage_ticks << ",\n"
       << "    \"impact_ticks_per_lot\": " << execution_model.impact_ticks_per_lot << "\n"
       << "  },\n"
       << "  \"session\": {\n"
       << "    \"start_time_ns\": " << start_time_ns << ",\n"
       << "    \"end_time_ns\": " << end_time_ns << ",\n"
       << "    \"duration_sec\": " << (duration_ns / 1e9) << ",\n"
       << "    \"total_events_processed\": " << total_events_processed << ",\n"
       << "    \"final_feed_status\": \"" << ::marketlab::to_string(final_feed_status) << "\"\n"
       << "  },\n"
       << "  \"performance\": {\n"
       << "    \"initial_cash\": " << initial_cash << ",\n"
       << "    \"final_cash\": " << final_cash << ",\n"
       << "    \"initial_inventory\": " << initial_inventory << ",\n"
       << "    \"final_inventory\": " << final_inventory << ",\n"
       << "    \"starting_equity\": " << starting_equity << ",\n"
       << "    \"ending_equity\": " << ending_equity << ",\n"
       << "    \"gross_pnl\": " << gross_pnl << ",\n"
       << "    \"net_pnl\": " << net_pnl << ",\n"
       << "    \"realized_pnl\": " << realized_pnl << ",\n"
       << "    \"unrealized_pnl\": " << unrealized_pnl << ",\n"
       << "    \"total_fees_paid\": " << total_fees_paid << ",\n"
       << "    \"turnover\": " << turnover << "\n"
       << "  },\n"
       << "  \"orders_and_fills\": {\n"
       << "    \"orders_submitted\": " << orders_submitted << ",\n"
       << "    \"orders_accepted\": " << orders_accepted << ",\n"
       << "    \"orders_rejected\": " << orders_rejected << ",\n"
       << "    \"orders_canceled\": " << orders_canceled << ",\n"
       << "    \"total_trades\": " << total_trades << ",\n"
       << "    \"aggressive_trades\": " << aggressive_trades << ",\n"
       << "    \"passive_trades\": " << passive_trades << ",\n"
       << "    \"total_order_qty_submitted\": " << total_order_qty_submitted << ",\n"
       << "    \"filled_quantity\": " << filled_quantity << ",\n"
       << "    \"fill_rate_qty_pct\": " << fill_rate_qty_pct << ",\n"
       << "    \"fill_rate_orders_pct\": " << fill_rate_orders_pct << "\n"
       << "  },\n"
       << "  \"inventory_profile\": {\n"
       << "    \"min_inventory\": " << min_inventory << ",\n"
       << "    \"max_inventory\": " << max_inventory << ",\n"
       << "    \"peak_absolute_inventory\": " << peak_absolute_inventory << "\n"
       << "  },\n"
       << "  \"drawdown\": {\n"
       << "    \"peak_equity\": " << peak_equity << ",\n"
       << "    \"max_drawdown\": " << max_drawdown << ",\n"
       << "    \"max_drawdown_pct\": " << max_drawdown_pct << "\n"
       << "  },\n"
       << "  \"conservation\": {\n"
       << "    \"cash_conserved\": " << (cash_conserved ? "true" : "false") << ",\n"
       << "    \"inventory_conserved\": " << (inventory_conserved ? "true" : "false") << ",\n"
       << "    \"initial_system_cash\": " << initial_system_cash << ",\n"
       << "    \"final_system_cash\": " << final_system_cash << ",\n"
       << "    \"initial_system_inventory\": " << initial_system_inventory << ",\n"
       << "    \"final_system_inventory\": " << final_system_inventory << "\n"
       << "  },\n"
       << "  \"markouts\": [\n";

    for (size_t i = 0; i < markout_summaries.size(); ++i) {
        const auto& m = markout_summaries[i];
        if (i > 0) ss << ",\n";
        ss << "    {\n"
           << "      \"horizon_ns\": " << m.horizon_ns << ",\n"
           << "      \"label\": \"" << m.horizon_label << "\",\n"
           << "      \"total_trades\": " << m.total_trades << ",\n"
           << "      \"completed_count\": " << m.completed_count << ",\n"
           << "      \"unfinished_count\": " << m.unfinished_count << ",\n"
           << "      \"missing_mark_count\": " << m.missing_mark_count << ",\n"
           << "      \"mean_markout_ticks\": " << m.mean_markout_ticks << ",\n"
           << "      \"mean_markout_bps\": " << m.mean_markout_bps << ",\n"
           << "      \"volume_weighted_markout_ticks\": " << m.volume_weighted_markout_ticks << ",\n"
           << "      \"volume_weighted_markout_bps\": " << m.volume_weighted_markout_bps << "\n"
           << "    }";
    }

    ss << "\n  ]\n}\n";
    return ss.str();
}

std::string BacktestReport::to_string() const {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "=========================================================\n";
    ss << "  MarketLab Event-Driven Backtest Summary Report\n";
    ss << "=========================================================\n";
    ss << "Strategy:             " << strategy_name << " (Account: " << strategy_account_id << ")\n";
    ss << "Instrument:           " << instrument.symbol << " (tick=" << instrument.tick_size
       << ", lot=" << instrument.lot_size << ")\n";
    ss << "Execution Model:      Queue=" << ::marketlab::to_string(execution_model.queue_model)
       << " (" << execution_model.queue_position_pct << "%) | Impact=" << ::marketlab::to_string(execution_model.impact_model);
    if (execution_model.fixed_slippage_ticks > 0) {
        ss << " (slip=" << execution_model.fixed_slippage_ticks << " ticks)";
    }
    ss << "\n";
    ss << "Simulated Duration:   " << (duration_ns / 1e9) << " s (" << total_events_processed << " events processed)\n";
    ss << "Feed Final Status:    " << ::marketlab::to_string(final_feed_status) << "\n\n";

    ss << "--- Financial Performance ---\n";
    ss << "Starting Equity:      " << starting_equity << "\n";
    ss << "Ending Equity:        " << ending_equity << "\n";
    ss << "Gross P&L:            " << gross_pnl << " ticks\n";
    ss << "Net P&L:              " << net_pnl << " ticks\n";
    ss << "Realized P&L:         " << realized_pnl << "\n";
    ss << "Unrealized P&L:       " << unrealized_pnl << "\n";
    ss << "Trading Fees Paid:    " << total_fees_paid << "\n";
    ss << "Traded Turnover:      " << turnover << " notional\n\n";

    ss << "--- Order & Execution Activity ---\n";
    ss << "Orders Submitted:     " << orders_submitted << "\n";
    ss << "Orders Accepted:      " << orders_accepted << "\n";
    ss << "Orders Rejected:      " << orders_rejected << "\n";
    ss << "Orders Canceled:      " << orders_canceled << "\n";
    ss << "Total Trades:         " << total_trades << " (Aggressive=" << aggressive_trades
       << ", Passive=" << passive_trades << ")\n";
    ss << "Total Lots Filled:    " << filled_quantity << " lots / " << total_order_qty_submitted << " submitted\n";
    ss << "Quantity Fill Rate:   " << fill_rate_qty_pct << " %\n";
    ss << "Order Fill Rate:      " << fill_rate_orders_pct << " %\n\n";

    ss << "--- Risk & Drawdown Profile ---\n";
    ss << "Inventory Range:      Min=" << min_inventory << " | Max=" << max_inventory
       << " | Peak Exposure=" << peak_absolute_inventory << " lots\n";
    ss << "Final Inventory:      " << final_inventory << " lots\n";
    ss << "Peak Equity:          " << peak_equity << "\n";
    ss << "Max Drawdown:         " << max_drawdown << " ticks (" << max_drawdown_pct << " %)\n\n";

    ss << "--- Double-Entry Ledger Conservation ---\n";
    ss << "Cash Conservation:      " << (cash_conserved ? "[PASS]" : "[FAIL]")
       << " (Initial: " << initial_system_cash << ", Final: " << final_system_cash << ")\n";
    ss << "Inventory Conservation: " << (inventory_conserved ? "[PASS]" : "[FAIL]")
       << " (Initial: " << initial_system_inventory << ", Final: " << final_system_inventory << ")\n\n";

    ss << "--- Post-Fill Markout Analysis ---\n";
    if (markout_summaries.empty()) {
        ss << "  (No markout horizons configured or no fills occurred)\n";
    } else {
        ss << std::left
           << std::setw(10) << "Horizon"
           << std::setw(12) << "Completed"
           << std::setw(12) << "Unfinished"
           << std::setw(12) << "MissingMark"
           << std::setw(14) << "Mean (ticks)"
           << std::setw(12) << "Mean (bps)"
           << std::setw(14) << "VWAP (ticks)"
           << std::setw(12) << "VWAP (bps)" << "\n";
        ss << std::string(88, '-') << "\n";
        for (const auto& m : markout_summaries) {
            ss << std::left
               << std::setw(10) << m.horizon_label
               << std::setw(12) << m.completed_count
               << std::setw(12) << m.unfinished_count
               << std::setw(12) << m.missing_mark_count
               << std::setw(14) << m.mean_markout_ticks
               << std::setw(12) << m.mean_markout_bps
               << std::setw(14) << m.volume_weighted_markout_ticks
               << std::setw(12) << m.volume_weighted_markout_bps << "\n";
        }
    }
    ss << "=========================================================\n";
    return ss.str();
}

} // namespace marketlab
