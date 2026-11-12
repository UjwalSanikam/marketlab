#pragma once

#include <vector>
#include <string>
#include <optional>
#include <functional>
#include <memory>
#include <sstream>
#include <iomanip>

#include "types.hpp"
#include "sim_types.hpp"
#include "market_data_types.hpp"
#include "execution_model.hpp"
#include "reference_strategy.hpp"
#include "backtest_report.hpp"
#include "backtest_engine.hpp"

namespace marketlab {

enum class CalibrationObjective : uint8_t {
    NetPnL,
    GrossPnL,
    FillRate,
    MinDrawdown
};

[[nodiscard]] constexpr std::string_view to_string(CalibrationObjective obj) noexcept {
    switch (obj) {
        case CalibrationObjective::NetPnL: return "NET_PNL";
        case CalibrationObjective::GrossPnL: return "GROSS_PNL";
        case CalibrationObjective::FillRate: return "FILL_RATE";
        case CalibrationObjective::MinDrawdown: return "MIN_DRAWDOWN";
    }
    return "UNKNOWN_OBJECTIVE";
}

[[nodiscard]] inline std::optional<CalibrationObjective> parse_calibration_objective(std::string_view s) noexcept {
    if (s == "net_pnl" || s == "NET_PNL") return CalibrationObjective::NetPnL;
    if (s == "gross_pnl" || s == "GROSS_PNL") return CalibrationObjective::GrossPnL;
    if (s == "fill_rate" || s == "FILL_RATE") return CalibrationObjective::FillRate;
    if (s == "min_drawdown" || s == "MIN_DRAWDOWN") return CalibrationObjective::MinDrawdown;
    return std::nullopt;
}

struct ParameterCandidate {
    std::string label;
    ReferenceStrategyConfig strategy_config;
    ExecutionModelConfig execution_model;
};

struct CandidateResult {
    ParameterCandidate candidate;
    BacktestReport calibration_report;
    double score{0.0};
};

struct CalibrationConfig {
    double split_ratio{0.6}; // 60% calibration, 40% holdout
    CalibrationObjective objective{CalibrationObjective::NetPnL};
    ParameterCandidate baseline_candidate;
    std::vector<ParameterCandidate> candidate_grid;
    BacktestConfig base_backtest_config;
};

struct CalibrationReport {
    std::string report_title{"Calibration & Holdout Validation"};
    bool is_sensitivity_analysis{false};
    CalibrationObjective objective{CalibrationObjective::NetPnL};
    double split_ratio{0.6};
    SimTime split_timestamp{0};
    size_t total_input_events{0};
    size_t calibration_event_count{0};
    size_t holdout_event_count{0};

    // Candidate ranking on calibration period
    std::vector<CandidateResult> calibration_rankings;
    ParameterCandidate selected_candidate;

    // Out-of-sample holdout comparison
    BacktestReport baseline_holdout_report;
    BacktestReport selected_holdout_report;

    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] std::string to_string() const;
};

class CalibrationEngine {
public:
    explicit CalibrationEngine(CalibrationConfig config);

    // Split input events chronologically into calibration and holdout sets
    void prepare_data(const std::vector<NormalizedEvent>& all_events);

    // Run grid search on calibration period and validate winner vs baseline on holdout
    [[nodiscard]] CalibrationReport run();

    [[nodiscard]] const std::vector<NormalizedEvent>& calibration_events() const noexcept {
        return calibration_events_;
    }
    [[nodiscard]] const std::vector<NormalizedEvent>& holdout_events() const noexcept {
        return holdout_events_;
    }
    [[nodiscard]] SimTime split_time() const noexcept { return split_time_; }

private:
    double evaluate_objective(const BacktestReport& report, CalibrationObjective obj) const;

    CalibrationConfig config_;
    std::vector<NormalizedEvent> calibration_events_;
    std::vector<NormalizedEvent> holdout_events_;
    SimTime split_time_{0};
    size_t total_input_events_{0};
};

} // namespace marketlab
