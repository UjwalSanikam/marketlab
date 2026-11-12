#include "marketlab/calibration_engine.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace marketlab {

CalibrationEngine::CalibrationEngine(CalibrationConfig config)
    : config_(std::move(config)) {}

void CalibrationEngine::prepare_data(const std::vector<NormalizedEvent>& all_events) {
    if (all_events.empty()) return;

    total_input_events_ = all_events.size();
    std::vector<NormalizedEvent> sorted_events = all_events;
    std::stable_sort(sorted_events.begin(), sorted_events.end(), [](const auto& a, const auto& b) {
        return get_event_time(a) < get_event_time(b);
    });

    SimTime t_start = get_event_time(sorted_events.front());
    SimTime t_end = get_event_time(sorted_events.back());

    if (t_end <= t_start) {
        split_time_ = t_start;
        calibration_events_ = sorted_events;
        holdout_events_ = sorted_events;
        return;
    }

    double ratio = std::clamp(config_.split_ratio, 0.05, 0.95);
    split_time_ = t_start + static_cast<SimTime>((t_end - t_start) * ratio);

    calibration_events_.clear();
    std::vector<NormalizedEvent> raw_holdout;

    // Track order book state through calibration period to synthesize warm-up snapshot for holdout
    ReconstructedOrderBook warm_book;

    for (const auto& ev : sorted_events) {
        SimTime t = get_event_time(ev);
        if (t < split_time_) {
            calibration_events_.push_back(ev);
            std::visit([&warm_book](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, NormalizedSnapshot>) {
                    warm_book.apply_snapshot(e.last_update_id, e.bids, e.asks);
                } else if constexpr (std::is_same_v<T, NormalizedDepthUpdate>) {
                    warm_book.apply_depth_update(e.first_update_id, e.final_update_id, e.bids, e.asks);
                } else if constexpr (std::is_same_v<T, NormalizedFeedStatus>) {
                    if (e.status == FeedStatus::Invalid) warm_book.mark_invalid(e.reason);
                }
            }, ev);
        } else {
            raw_holdout.push_back(ev);
        }
    }

    holdout_events_.clear();
    // Prepend snapshot if warm_book is valid to provide clean initial state for holdout
    if (warm_book.is_valid()) {
        NormalizedSnapshot snap;
        snap.local_receipt_time_ns = split_time_;
        snap.last_update_id = warm_book.last_update_id();
        for (const auto& [price, qty] : warm_book.bids()) {
            snap.bids.push_back(LevelUpdate{price, qty});
        }
        for (const auto& [price, qty] : warm_book.asks()) {
            snap.asks.push_back(LevelUpdate{price, qty});
        }
        holdout_events_.push_back(snap);
    }

    for (auto& ev : raw_holdout) {
        holdout_events_.push_back(std::move(ev));
    }
}

double CalibrationEngine::evaluate_objective(const BacktestReport& report, CalibrationObjective obj) const {
    switch (obj) {
        case CalibrationObjective::NetPnL:
            return static_cast<double>(report.net_pnl);
        case CalibrationObjective::GrossPnL:
            return static_cast<double>(report.gross_pnl);
        case CalibrationObjective::FillRate:
            return report.fill_rate_qty_pct;
        case CalibrationObjective::MinDrawdown:
            return -static_cast<double>(report.max_drawdown); // Higher (closer to 0) is better
    }
    return 0.0;
}

CalibrationReport CalibrationEngine::run() {
    CalibrationReport report;
    report.objective = config_.objective;
    report.split_ratio = config_.split_ratio;
    report.split_timestamp = split_time_;
    report.total_input_events = total_input_events_;
    report.calibration_event_count = calibration_events_.size();
    report.holdout_event_count = holdout_events_.size();

    if (config_.candidate_grid.empty()) {
        config_.candidate_grid.push_back(config_.baseline_candidate);
    }

    bool has_any_fills = false;

    // 1. Evaluate candidates on calibration period
    for (const auto& cand : config_.candidate_grid) {
        BacktestConfig b_cfg = config_.base_backtest_config;
        b_cfg.strategy_config = cand.strategy_config;
        b_cfg.execution_model = cand.execution_model;

        BacktestEngine engine(b_cfg);
        engine.load_events(calibration_events_);
        auto b_report = engine.run();

        if (b_report.total_trades > 0) {
            has_any_fills = true;
        }

        CandidateResult res;
        res.candidate = cand;
        res.calibration_report = b_report;
        res.score = evaluate_objective(b_report, config_.objective);
        report.calibration_rankings.push_back(res);
    }

    // Sort rankings descending by score
    std::stable_sort(report.calibration_rankings.begin(), report.calibration_rankings.end(),
                     [](const CandidateResult& a, const CandidateResult& b) {
                         return a.score > b.score;
                     });

    report.selected_candidate = report.calibration_rankings.front().candidate;

    // 2. Evaluate Baseline Candidate on Holdout Period
    {
        BacktestConfig base_cfg = config_.base_backtest_config;
        base_cfg.strategy_config = config_.baseline_candidate.strategy_config;
        base_cfg.execution_model = config_.baseline_candidate.execution_model;

        BacktestEngine engine(base_cfg);
        engine.load_events(holdout_events_);
        report.baseline_holdout_report = engine.run();
        if (report.baseline_holdout_report.total_trades > 0) {
            has_any_fills = true;
        }
    }

    // 3. Evaluate Selected Candidate on Holdout Period
    {
        BacktestConfig sel_cfg = config_.base_backtest_config;
        sel_cfg.strategy_config = report.selected_candidate.strategy_config;
        sel_cfg.execution_model = report.selected_candidate.execution_model;

        BacktestEngine engine(sel_cfg);
        engine.load_events(holdout_events_);
        report.selected_holdout_report = engine.run();
        if (report.selected_holdout_report.total_trades > 0) {
            has_any_fills = true;
        }
    }

    if (!has_any_fills) {
        report.is_sensitivity_analysis = true;
        report.report_title = "Execution Model Sensitivity Analysis";
    }

    return report;
}

std::string CalibrationReport::to_json() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"report_title\": \"" << report_title << "\",\n";
    ss << "  \"is_sensitivity_analysis\": " << (is_sensitivity_analysis ? "true" : "false") << ",\n";
    ss << "  \"objective\": \"" << ::marketlab::to_string(objective) << "\",\n";
    ss << "  \"split_ratio\": " << std::fixed << std::setprecision(2) << split_ratio << ",\n";
    ss << "  \"split_timestamp\": " << split_timestamp << ",\n";
    ss << "  \"total_input_events\": " << total_input_events << ",\n";
    ss << "  \"calibration_event_count\": " << calibration_event_count << ",\n";
    ss << "  \"holdout_event_count\": " << holdout_event_count << ",\n";
    ss << "  \"selected_candidate\": {\n";
    ss << "    \"label\": \"" << selected_candidate.label << "\",\n";
    ss << "    \"quote_size\": " << selected_candidate.strategy_config.quote_size << ",\n";
    ss << "    \"half_spread\": " << selected_candidate.strategy_config.half_spread << ",\n";
    ss << "    \"queue_model\": \"" << ::marketlab::to_string(selected_candidate.execution_model.queue_model) << "\",\n";
    ss << "    \"queue_pct\": " << selected_candidate.execution_model.queue_position_pct << "\n";
    ss << "  },\n";

    ss << "  \"calibration_rankings\": [\n";
    for (size_t i = 0; i < calibration_rankings.size(); ++i) {
        const auto& r = calibration_rankings[i];
        ss << "    {\n";
        ss << "      \"rank\": " << (i + 1) << ",\n";
        ss << "      \"label\": \"" << r.candidate.label << "\",\n";
        ss << "      \"score\": " << std::fixed << std::setprecision(4) << r.score << ",\n";
        ss << "      \"net_pnl\": " << r.calibration_report.net_pnl << ",\n";
        ss << "      \"trades\": " << r.calibration_report.total_trades << ",\n";
        ss << "      \"fill_rate_pct\": " << std::fixed << std::setprecision(2) << r.calibration_report.fill_rate_qty_pct << ",\n";
        ss << "      \"max_drawdown\": " << r.calibration_report.max_drawdown << "\n";
        ss << "    }" << (i + 1 < calibration_rankings.size() ? ",\n" : "\n");
    }
    ss << "  ],\n";

    ss << "  \"holdout_validation\": {\n";
    ss << "    \"baseline\": {\n";
    ss << "      \"net_pnl\": " << baseline_holdout_report.net_pnl << ",\n";
    ss << "      \"gross_pnl\": " << baseline_holdout_report.gross_pnl << ",\n";
    ss << "      \"fees\": " << baseline_holdout_report.total_fees_paid << ",\n";
    ss << "      \"trades\": " << baseline_holdout_report.total_trades << ",\n";
    ss << "      \"filled_quantity\": " << baseline_holdout_report.filled_quantity << ",\n";
    ss << "      \"fill_rate_qty_pct\": " << std::fixed << std::setprecision(2) << baseline_holdout_report.fill_rate_qty_pct << ",\n";
    ss << "      \"max_drawdown\": " << baseline_holdout_report.max_drawdown << "\n";
    ss << "    },\n";
    ss << "    \"selected\": {\n";
    ss << "      \"net_pnl\": " << selected_holdout_report.net_pnl << ",\n";
    ss << "      \"gross_pnl\": " << selected_holdout_report.gross_pnl << ",\n";
    ss << "      \"fees\": " << selected_holdout_report.total_fees_paid << ",\n";
    ss << "      \"trades\": " << selected_holdout_report.total_trades << ",\n";
    ss << "      \"filled_quantity\": " << selected_holdout_report.filled_quantity << ",\n";
    ss << "      \"fill_rate_qty_pct\": " << std::fixed << std::setprecision(2) << selected_holdout_report.fill_rate_qty_pct << ",\n";
    ss << "      \"max_drawdown\": " << selected_holdout_report.max_drawdown << "\n";
    ss << "    }\n";
    ss << "  }\n";
    ss << "}\n";
    return ss.str();
}

std::string CalibrationReport::to_string() const {
    std::ostringstream ss;
    ss << "================================================================================\n";
    ss << "  " << report_title << "\n";
    ss << "================================================================================\n";
    ss << "Objective:              " << ::marketlab::to_string(objective) << "\n";
    ss << "Chronological Split:    " << std::fixed << std::setprecision(1) << (split_ratio * 100.0)
       << "% Train / " << ((1.0 - split_ratio) * 100.0) << "% Holdout\n";
    ss << "Split Timestamp (ns):   " << split_timestamp << "\n";
    ss << "Events:                 Total: " << total_input_events
       << " | Calibration: " << calibration_event_count
       << " | Holdout: " << holdout_event_count << "\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "  Calibration Period Rankings (In-Sample)\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << std::left << std::setw(6) << "Rank"
       << std::setw(48) << "Candidate"
       << std::setw(16) << "Score"
       << std::setw(16) << "Net PnL"
       << std::setw(10) << "Trades"
       << std::setw(12) << "Fill Rate"
       << std::setw(12) << "Max DD" << "\n";

    for (size_t i = 0; i < calibration_rankings.size(); ++i) {
        const auto& r = calibration_rankings[i];
        ss << std::left << std::setw(6) << (std::to_string(i + 1) + ".")
           << std::setw(48) << r.candidate.label
           << std::setw(16) << (std::to_string(static_cast<int64_t>(r.score)))
           << std::setw(16) << r.calibration_report.net_pnl
           << std::setw(10) << r.calibration_report.total_trades
           << std::setw(12) << (std::to_string(static_cast<int>(r.calibration_report.fill_rate_qty_pct)) + "%")
           << std::setw(12) << r.calibration_report.max_drawdown << "\n";
    }

    ss << "--------------------------------------------------------------------------------\n";
    ss << "Selected Parameter Set: " << selected_candidate.label << "\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << "  Out-of-Sample Holdout Performance Comparison\n";
    ss << "--------------------------------------------------------------------------------\n";
    ss << std::left << std::setw(28) << "Metric"
       << std::setw(24) << "Baseline"
       << std::setw(24) << "Selected Winner" << "\n";
    ss << std::left << std::setw(28) << "Net P&L (cents):"
       << std::setw(24) << baseline_holdout_report.net_pnl
       << std::setw(24) << selected_holdout_report.net_pnl << "\n";
    ss << std::left << std::setw(28) << "Gross P&L (cents):"
       << std::setw(24) << baseline_holdout_report.gross_pnl
       << std::setw(24) << selected_holdout_report.gross_pnl << "\n";
    ss << std::left << std::setw(28) << "Fees Paid (cents):"
       << std::setw(24) << baseline_holdout_report.total_fees_paid
       << std::setw(24) << selected_holdout_report.total_fees_paid << "\n";
    ss << std::left << std::setw(28) << "Trades Executed:"
       << std::setw(24) << baseline_holdout_report.total_trades
       << std::setw(24) << selected_holdout_report.total_trades << "\n";
    ss << std::left << std::setw(28) << "Volume Traded (lots):"
       << std::setw(24) << baseline_holdout_report.filled_quantity
       << std::setw(24) << selected_holdout_report.filled_quantity << "\n";
    ss << std::left << std::setw(28) << "Fill Rate (Qty %):"
       << std::setw(24) << (std::to_string(static_cast<int>(baseline_holdout_report.fill_rate_qty_pct)) + "%")
       << std::setw(24) << (std::to_string(static_cast<int>(selected_holdout_report.fill_rate_qty_pct)) + "%") << "\n";
    ss << std::left << std::setw(28) << "Max Drawdown (cents):"
       << std::setw(24) << baseline_holdout_report.max_drawdown
       << std::setw(24) << selected_holdout_report.max_drawdown << "\n";
    ss << "================================================================================\n";
    return ss.str();
}

} // namespace marketlab
