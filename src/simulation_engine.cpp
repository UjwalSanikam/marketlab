#include "marketlab/simulation_engine.hpp"
#include "marketlab/serializer.hpp"
#include "marketlab/json.hpp"
#include <sstream>
#include <iomanip>
#include <random>
#include <unordered_set>

namespace marketlab {

#ifndef MARKETLAB_BUILD_REVISION
#define MARKETLAB_BUILD_REVISION "unavailable"
#endif

namespace {

std::string get_build_environment() {
    std::string env;
#if defined(_WIN32)
    env += "Windows x86_64";
#elif defined(__linux__)
    env += "Linux x86_64";
#elif defined(__APPLE__)
    env += "macOS";
#else
    env += "Unknown OS";
#endif
    env += ", ";
#if defined(__clang__)
    env += "Clang ";
    env += __clang_version__;
#elif defined(__GNUC__)
    env += "GCC ";
    env += __VERSION__;
#elif defined(_MSC_VER)
    env += "MSVC ";
    env += std::to_string(_MSC_VER);
#else
    env += "Unknown Compiler";
#endif
    env += " C++20";
    return env;
}

} // namespace

SimulationConfig::ValidationResult SimulationConfig::validate() const {
    if (run_horizon_ns == 0) return {false, "run_horizon_ns must be greater than 0"};
    if (max_events == 0) return {false, "max_events must be greater than 0"};
    if (instrument.symbol.empty()) return {false, "instrument.symbol cannot be empty"};
    if (instrument.tick_size <= 0) return {false, "instrument.tick_size must be greater than 0"};
    if (instrument.lot_size <= 0) return {false, "instrument.lot_size must be greater than 0"};
    if (fees.maker_fee_bps < 0 || fees.maker_fee_bps > 10000) return {false, "fees.maker_fee_bps must be between 0 and 10000"};
    if (fees.taker_fee_bps < 0 || fees.taker_fee_bps > 10000) return {false, "fees.taker_fee_bps must be between 0 and 10000"};
    if (fees.fee_account_id.empty()) return {false, "fees.fee_account_id cannot be empty"};
    if (strategy_account_id.empty()) return {false, "strategy_account_id cannot be empty"};
    if (background_account_id.empty()) return {false, "background_account_id cannot be empty"};
    if (strategy_account_id == background_account_id) return {false, "strategy_account_id and background_account_id cannot be identical"};
    if (strategy_account_id == fees.fee_account_id) return {false, "strategy_account_id and fee_account_id cannot be identical"};
    if (background_account_id == fees.fee_account_id) return {false, "background_account_id and fee_account_id cannot be identical"};
    if (risk.max_order_qty <= 0) return {false, "risk.max_order_qty must be greater than 0"};
    if (risk.max_order_notional <= 0) return {false, "risk.max_order_notional must be greater than 0"};
    if (risk.max_inventory <= 0) return {false, "risk.max_inventory must be greater than 0"};
    if (strategy_config.quote_size <= 0) return {false, "strategy_config.quote_size must be greater than 0"};
    if (strategy_config.half_spread <= 0) return {false, "strategy_config.half_spread must be greater than 0"};
    if (strategy_starting_cash < 0) return {false, "strategy_starting_cash cannot be negative"};
    if (strategy_starting_inventory < 0) return {false, "strategy_starting_inventory cannot be negative"};
    if (background_starting_cash < 0) return {false, "background_starting_cash cannot be negative"};
    if (background_starting_inventory < 0) return {false, "background_starting_inventory cannot be negative"};
    return {true, ""};
}

std::string SimulationConfig::to_json() const {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"seed\": " << seed << ",\n"
       << "  \"run_horizon_ns\": " << run_horizon_ns << ",\n"
       << "  \"max_events\": " << max_events << ",\n"
       << "  \"instrument\": {\n"
       << "    \"symbol\": \"" << instrument.symbol << "\",\n"
       << "    \"tick_size\": " << instrument.tick_size << ",\n"
       << "    \"lot_size\": " << instrument.lot_size << "\n"
       << "  },\n"
       << "  \"fees\": {\n"
       << "    \"maker_fee_bps\": " << fees.maker_fee_bps << ",\n"
       << "    \"taker_fee_bps\": " << fees.taker_fee_bps << ",\n"
       << "    \"fee_account_id\": \"" << fees.fee_account_id << "\"\n"
       << "  },\n"
       << "  \"latencies\": {\n"
       << "    \"market_data_delay_ns\": " << latencies.market_data_delay << ",\n"
       << "    \"strategy_delay_ns\": " << latencies.strategy_delay << ",\n"
       << "    \"order_entry_delay_ns\": " << latencies.order_entry_delay << ",\n"
       << "    \"execution_report_delay_ns\": " << latencies.execution_report_delay << "\n"
       << "  },\n"
       << "  \"risk\": {\n"
       << "    \"max_order_qty\": " << risk.max_order_qty << ",\n"
       << "    \"max_order_notional\": " << risk.max_order_notional << ",\n"
       << "    \"max_inventory\": " << risk.max_inventory << ",\n"
       << "    \"max_data_age_ns\": " << risk.max_data_age_ns << ",\n"
       << "    \"allow_self_trade\": " << (risk.allow_self_trade ? "true" : "false") << "\n"
       << "  },\n"
       << "  \"strategy\": {\n"
       << "    \"quote_size\": " << strategy_config.quote_size << ",\n"
       << "    \"half_spread\": " << strategy_config.half_spread << ",\n"
       << "    \"inventory_target\": " << strategy_config.inventory_target << ",\n"
       << "    \"skew_per_lot\": " << strategy_config.skew_per_lot << ",\n"
       << "    \"quote_interval_ns\": " << strategy_config.quote_interval_ns << "\n"
       << "  },\n"
       << "  \"strategy_account_id\": \"" << strategy_account_id << "\",\n"
       << "  \"strategy_starting_cash\": " << strategy_starting_cash << ",\n"
       << "  \"strategy_starting_inventory\": " << strategy_starting_inventory << ",\n"
       << "  \"background_account_id\": \"" << background_account_id << "\",\n"
       << "  \"background_starting_cash\": " << background_starting_cash << ",\n"
       << "  \"background_starting_inventory\": " << background_starting_inventory << "\n"
       << "}\n";
    return ss.str();
}

std::pair<std::optional<SimulationConfig>, std::string> SimulationConfig::from_json_strict(const std::string& json_str) {
    auto parse_res = JsonParser::parse(json_str);
    if (!parse_res.ok) {
        return {std::nullopt, "JSON parse error: " + parse_res.error};
    }

    if (!parse_res.value.is_object()) {
        return {std::nullopt, "JSON root must be an object, got " + parse_res.value.type_name()};
    }

    SimulationConfig cfg;

    const std::unordered_set<std::string> allowed_root_keys = {
        "seed",
        "run_horizon_ns",
        "max_events",
        "instrument",
        "fees",
        "latencies",
        "risk",
        "strategy",
        "strategy_account_id",
        "strategy_starting_cash",
        "strategy_starting_inventory",
        "background_account_id",
        "background_starting_cash",
        "background_starting_inventory"
    };

    // 1. Check for unknown root keys
    for (const auto& [k, v] : parse_res.value.as_object()) {
        if (allowed_root_keys.find(k) == allowed_root_keys.end()) {
            return {std::nullopt, "Unknown configuration key: '" + k + "'"};
        }
    }

    // 2. Validate and extract top-level fields
    if (const auto* val = parse_res.value.find("seed")) {
        if (!val->is_number() || val->is_floating_point()) {
            return {std::nullopt, "Field 'seed' must be an integer, got " + val->type_name()};
        }
        auto i = val->as_uint64();
        if (!i) return {std::nullopt, "Field 'seed' out of valid range for uint64: " + val->raw_number()};
        cfg.seed = *i;
    }

    if (const auto* val = parse_res.value.find("run_horizon_ns")) {
        if (!val->is_number() || val->is_floating_point()) {
            return {std::nullopt, "Field 'run_horizon_ns' must be an integer, got " + val->type_name()};
        }
        auto i = val->as_uint64();
        if (!i) return {std::nullopt, "Field 'run_horizon_ns' out of valid range for uint64: " + val->raw_number()};
        cfg.run_horizon_ns = *i;
    }

    if (const auto* val = parse_res.value.find("max_events")) {
        if (!val->is_number() || val->is_floating_point()) {
            return {std::nullopt, "Field 'max_events' must be an integer, got " + val->type_name()};
        }
        auto i = val->as_uint64();
        if (!i) return {std::nullopt, "Field 'max_events' out of valid range for uint64: " + val->raw_number()};
        cfg.max_events = static_cast<size_t>(*i);
    }

    // 3. Instrument object
    if (const auto* inst_val = parse_res.value.find("instrument")) {
        if (!inst_val->is_object()) {
            return {std::nullopt, "Field 'instrument' must be an object, got " + inst_val->type_name()};
        }
        const std::unordered_set<std::string> allowed_inst_keys = {"symbol", "tick_size", "lot_size"};
        for (const auto& [k, v] : inst_val->as_object()) {
            if (allowed_inst_keys.find(k) == allowed_inst_keys.end()) {
                return {std::nullopt, "Unknown configuration key: 'instrument." + k + "'"};
            }
        }
        if (const auto* v = inst_val->find("symbol")) {
            if (!v->is_string()) return {std::nullopt, "Field 'instrument.symbol' must be a string, got " + v->type_name()};
            cfg.instrument.symbol = v->as_string();
        }
        if (const auto* v = inst_val->find("tick_size")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'instrument.tick_size' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'instrument.tick_size' out of range for int64: " + v->raw_number()};
            cfg.instrument.tick_size = *i;
        }
        if (const auto* v = inst_val->find("lot_size")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'instrument.lot_size' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'instrument.lot_size' out of range for int64: " + v->raw_number()};
            cfg.instrument.lot_size = *i;
        }
    }

    // 4. Fees object
    if (const auto* fees_val = parse_res.value.find("fees")) {
        if (!fees_val->is_object()) {
            return {std::nullopt, "Field 'fees' must be an object, got " + fees_val->type_name()};
        }
        const std::unordered_set<std::string> allowed_fees_keys = {"maker_fee_bps", "taker_fee_bps", "fee_account_id"};
        for (const auto& [k, v] : fees_val->as_object()) {
            if (allowed_fees_keys.find(k) == allowed_fees_keys.end()) {
                return {std::nullopt, "Unknown configuration key: 'fees." + k + "'"};
            }
        }
        if (const auto* v = fees_val->find("maker_fee_bps")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'fees.maker_fee_bps' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'fees.maker_fee_bps' out of range for int64: " + v->raw_number()};
            cfg.fees.maker_fee_bps = *i;
        }
        if (const auto* v = fees_val->find("taker_fee_bps")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'fees.taker_fee_bps' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'fees.taker_fee_bps' out of range for int64: " + v->raw_number()};
            cfg.fees.taker_fee_bps = *i;
        }
        if (const auto* v = fees_val->find("fee_account_id")) {
            if (!v->is_string()) return {std::nullopt, "Field 'fees.fee_account_id' must be a string, got " + v->type_name()};
            cfg.fees.fee_account_id = v->as_string();
        }
    }

    // 5. Latencies object
    if (const auto* lat_val = parse_res.value.find("latencies")) {
        if (!lat_val->is_object()) {
            return {std::nullopt, "Field 'latencies' must be an object, got " + lat_val->type_name()};
        }
        const std::unordered_set<std::string> allowed_lat_keys = {
            "market_data_delay_ns", "strategy_delay_ns", "order_entry_delay_ns", "execution_report_delay_ns"
        };
        for (const auto& [k, v] : lat_val->as_object()) {
            if (allowed_lat_keys.find(k) == allowed_lat_keys.end()) {
                return {std::nullopt, "Unknown configuration key: 'latencies." + k + "'"};
            }
        }
        if (const auto* v = lat_val->find("market_data_delay_ns")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'latencies.market_data_delay_ns' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_uint64();
            if (!i) return {std::nullopt, "Field 'latencies.market_data_delay_ns' out of range for uint64: " + v->raw_number()};
            cfg.latencies.market_data_delay = *i;
        }
        if (const auto* v = lat_val->find("strategy_delay_ns")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'latencies.strategy_delay_ns' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_uint64();
            if (!i) return {std::nullopt, "Field 'latencies.strategy_delay_ns' out of range for uint64: " + v->raw_number()};
            cfg.latencies.strategy_delay = *i;
        }
        if (const auto* v = lat_val->find("order_entry_delay_ns")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'latencies.order_entry_delay_ns' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_uint64();
            if (!i) return {std::nullopt, "Field 'latencies.order_entry_delay_ns' out of range for uint64: " + v->raw_number()};
            cfg.latencies.order_entry_delay = *i;
        }
        if (const auto* v = lat_val->find("execution_report_delay_ns")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'latencies.execution_report_delay_ns' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_uint64();
            if (!i) return {std::nullopt, "Field 'latencies.execution_report_delay_ns' out of range for uint64: " + v->raw_number()};
            cfg.latencies.execution_report_delay = *i;
        }
    }

    // 6. Risk object
    if (const auto* risk_val = parse_res.value.find("risk")) {
        if (!risk_val->is_object()) {
            return {std::nullopt, "Field 'risk' must be an object, got " + risk_val->type_name()};
        }
        const std::unordered_set<std::string> allowed_risk_keys = {
            "max_order_qty", "max_order_notional", "max_inventory", "max_data_age_ns", "allow_self_trade"
        };
        for (const auto& [k, v] : risk_val->as_object()) {
            if (allowed_risk_keys.find(k) == allowed_risk_keys.end()) {
                return {std::nullopt, "Unknown configuration key: 'risk." + k + "'"};
            }
        }
        if (const auto* v = risk_val->find("max_order_qty")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'risk.max_order_qty' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'risk.max_order_qty' out of range for int64: " + v->raw_number()};
            cfg.risk.max_order_qty = *i;
        }
        if (const auto* v = risk_val->find("max_order_notional")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'risk.max_order_notional' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'risk.max_order_notional' out of range for int64: " + v->raw_number()};
            cfg.risk.max_order_notional = *i;
        }
        if (const auto* v = risk_val->find("max_inventory")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'risk.max_inventory' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'risk.max_inventory' out of range for int64: " + v->raw_number()};
            cfg.risk.max_inventory = *i;
        }
        if (const auto* v = risk_val->find("max_data_age_ns")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'risk.max_data_age_ns' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_uint64();
            if (!i) return {std::nullopt, "Field 'risk.max_data_age_ns' out of range for uint64: " + v->raw_number()};
            cfg.risk.max_data_age_ns = *i;
        }
        if (const auto* v = risk_val->find("allow_self_trade")) {
            if (!v->is_bool()) {
                return {std::nullopt, "Field 'risk.allow_self_trade' must be a boolean, got " + v->type_name()};
            }
            cfg.risk.allow_self_trade = v->as_bool();
        }
    }

    // 7. Strategy object
    if (const auto* strat_val = parse_res.value.find("strategy")) {
        if (!strat_val->is_object()) {
            return {std::nullopt, "Field 'strategy' must be an object, got " + strat_val->type_name()};
        }
        const std::unordered_set<std::string> allowed_strat_keys = {
            "quote_size", "half_spread", "inventory_target", "skew_per_lot", "quote_interval_ns"
        };
        for (const auto& [k, v] : strat_val->as_object()) {
            if (allowed_strat_keys.find(k) == allowed_strat_keys.end()) {
                return {std::nullopt, "Unknown configuration key: 'strategy." + k + "'"};
            }
        }
        if (const auto* v = strat_val->find("quote_size")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'strategy.quote_size' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'strategy.quote_size' out of range for int64: " + v->raw_number()};
            cfg.strategy_config.quote_size = *i;
        }
        if (const auto* v = strat_val->find("half_spread")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'strategy.half_spread' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'strategy.half_spread' out of range for int64: " + v->raw_number()};
            cfg.strategy_config.half_spread = *i;
        }
        if (const auto* v = strat_val->find("inventory_target")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'strategy.inventory_target' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'strategy.inventory_target' out of range for int64: " + v->raw_number()};
            cfg.strategy_config.inventory_target = *i;
        }
        if (const auto* v = strat_val->find("skew_per_lot")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'strategy.skew_per_lot' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_int64();
            if (!i) return {std::nullopt, "Field 'strategy.skew_per_lot' out of range for int64: " + v->raw_number()};
            cfg.strategy_config.skew_per_lot = *i;
        }
        if (const auto* v = strat_val->find("quote_interval_ns")) {
            if (!v->is_number() || v->is_floating_point()) {
                return {std::nullopt, "Field 'strategy.quote_interval_ns' must be an integer, got " + v->type_name()};
            }
            auto i = v->as_uint64();
            if (!i) return {std::nullopt, "Field 'strategy.quote_interval_ns' out of range for uint64: " + v->raw_number()};
            cfg.strategy_config.quote_interval_ns = *i;
        }
    }

    // 8. Strategy & Background accounts
    if (const auto* val = parse_res.value.find("strategy_account_id")) {
        if (!val->is_string()) return {std::nullopt, "Field 'strategy_account_id' must be a string, got " + val->type_name()};
        cfg.strategy_account_id = val->as_string();
    }
    if (const auto* val = parse_res.value.find("strategy_starting_cash")) {
        if (!val->is_number() || val->is_floating_point()) {
            return {std::nullopt, "Field 'strategy_starting_cash' must be an integer, got " + val->type_name()};
        }
        auto i = val->as_int64();
        if (!i) return {std::nullopt, "Field 'strategy_starting_cash' out of range for int64: " + val->raw_number()};
        cfg.strategy_starting_cash = *i;
    }
    if (const auto* val = parse_res.value.find("strategy_starting_inventory")) {
        if (!val->is_number() || val->is_floating_point()) {
            return {std::nullopt, "Field 'strategy_starting_inventory' must be an integer, got " + val->type_name()};
        }
        auto i = val->as_int64();
        if (!i) return {std::nullopt, "Field 'strategy_starting_inventory' out of range for int64: " + val->raw_number()};
        cfg.strategy_starting_inventory = *i;
    }

    if (const auto* val = parse_res.value.find("background_account_id")) {
        if (!val->is_string()) return {std::nullopt, "Field 'background_account_id' must be a string, got " + val->type_name()};
        cfg.background_account_id = val->as_string();
    }
    if (const auto* val = parse_res.value.find("background_starting_cash")) {
        if (!val->is_number() || val->is_floating_point()) {
            return {std::nullopt, "Field 'background_starting_cash' must be an integer, got " + val->type_name()};
        }
        auto i = val->as_int64();
        if (!i) return {std::nullopt, "Field 'background_starting_cash' out of range for int64: " + val->raw_number()};
        cfg.background_starting_cash = *i;
    }
    if (const auto* val = parse_res.value.find("background_starting_inventory")) {
        if (!val->is_number() || val->is_floating_point()) {
            return {std::nullopt, "Field 'background_starting_inventory' must be an integer, got " + val->type_name()};
        }
        auto i = val->as_int64();
        if (!i) return {std::nullopt, "Field 'background_starting_inventory' out of range for int64: " + val->raw_number()};
        cfg.background_starting_inventory = *i;
    }

    // 9. Semantic validation
    auto val_res = cfg.validate();
    if (!val_res.valid) {
        return {std::nullopt, "Configuration semantic error: " + val_res.error};
    }

    return {cfg, ""};
}

SimulationConfig SimulationConfig::from_json(const std::string& json_str) {
    auto [cfg_opt, _] = from_json_strict(json_str);
    if (cfg_opt) return *cfg_opt;
    return SimulationConfig{};
}

SimulationEngine::SimulationEngine(SimulationConfig config)
    : config_(std::move(config)),
      book_(config_.instrument),
      ledger_(config_.fees),
      risk_engine_(config_.risk),
      strategy_(std::make_unique<InventoryAwareMarketMaker>(config_.strategy_config)) {

    // Fund strategy account
    ledger_.fund_account(config_.strategy_account_id,
                         config_.strategy_starting_cash,
                         config_.strategy_starting_inventory);

    // Fund background account
    ledger_.fund_account(config_.background_account_id,
                         config_.background_starting_cash,
                         config_.background_starting_inventory);

    initial_total_cash_ = ledger_.get_total_cash();
    initial_total_inventory_ = ledger_.get_total_inventory();

    // Schedule strategy start at virtual time 0
    scheduler_.schedule(0, EventPriority::StrategyWakeup, [this](SimTime t) {
        strategy_->on_start(t, *this);
    });
}

void SimulationEngine::submit_order(Side side, OrderType type, Price price, Quantity qty, TimeInForce tif) {
    ++strategy_orders_submitted_;
    OrderId id = allocate_order_id();
    SeqNum seq = ++next_cmd_seq_;
    NewOrderCommand cmd{seq, id, side, type, price, qty, tif};

    // Simulated latency: strategy decision time + network order entry delay
    auto delay_opt = safe_add_time(config_.latencies.strategy_delay, config_.latencies.order_entry_delay);
    if (!delay_opt) {
        ++strategy_risk_rejections_;
        ExecutionReport rpt{ExecutionReport::Type::Rejected, scheduler_.now(), id, side, price, qty, qty, "TIME_ARITHMETIC_OVERFLOW"};
        if (strategy_) strategy_->on_execution_report(scheduler_.now(), rpt, *this);
        return;
    }
    auto arr_opt = safe_add_time(scheduler_.now(), *delay_opt);
    if (!arr_opt) {
        ++strategy_risk_rejections_;
        ExecutionReport rpt{ExecutionReport::Type::Rejected, scheduler_.now(), id, side, price, qty, qty, "TIME_ARITHMETIC_OVERFLOW"};
        if (strategy_) strategy_->on_execution_report(scheduler_.now(), rpt, *this);
        return;
    }
    SimTime arrival_time = *arr_opt;

    Price protection_price = price;
    if (type == OrderType::Market && side == Side::Buy) {
        if (price > 0) {
            protection_price = price;
        } else if (last_mark_price_.has_value()) {
            protection_price = *last_mark_price_ + 10;
        } else {
            protection_price = 0; // Explicit cap required; risk engine will reject
        }
    }

    scheduler_.schedule(arrival_time, EventPriority::OrderAdmission, [this, cmd, protection_price](SimTime) {
        this->process_order_admission(config_.strategy_account_id, cmd, protection_price);
    });
}

void SimulationEngine::cancel_order(OrderId order_id) {
    SeqNum seq = ++next_cmd_seq_;
    CancelOrderCommand cmd{seq, order_id};

    auto delay_opt = safe_add_time(config_.latencies.strategy_delay, config_.latencies.order_entry_delay);
    if (!delay_opt) {
        ExecutionReport rpt{ExecutionReport::Type::Rejected, scheduler_.now(), order_id, Side::Buy, 0, 0, 0, "TIME_ARITHMETIC_OVERFLOW"};
        if (strategy_) strategy_->on_execution_report(scheduler_.now(), rpt, *this);
        return;
    }
    auto arr_opt = safe_add_time(scheduler_.now(), *delay_opt);
    if (!arr_opt) {
        ExecutionReport rpt{ExecutionReport::Type::Rejected, scheduler_.now(), order_id, Side::Buy, 0, 0, 0, "TIME_ARITHMETIC_OVERFLOW"};
        if (strategy_) strategy_->on_execution_report(scheduler_.now(), rpt, *this);
        return;
    }
    SimTime arrival_time = *arr_opt;

    scheduler_.schedule(arrival_time, EventPriority::OrderAdmission, [this, cmd](SimTime) {
        this->process_cancel_admission(config_.strategy_account_id, cmd);
    });
}

void SimulationEngine::submit_background_order(const AccountId& account_id,
                                               Side side,
                                               OrderType type,
                                               Price price,
                                               Quantity qty,
                                               TimeInForce tif,
                                               SimTime arrival_time) {
    OrderId id = allocate_order_id();
    SeqNum seq = ++next_cmd_seq_;
    NewOrderCommand cmd{seq, id, side, type, price, qty, tif};

    Price protection_price = price;
    if (type == OrderType::Market && side == Side::Buy) {
        if (price > 0) {
            protection_price = price;
        } else if (last_mark_price_.has_value()) {
            protection_price = *last_mark_price_ + 10;
        } else {
            protection_price = 0;
        }
    }

    scheduler_.schedule(arrival_time, EventPriority::BackgroundArrival, [this, account_id, cmd, protection_price](SimTime) {
        this->process_order_admission(account_id, cmd, protection_price);
    });
}

void SimulationEngine::submit_background_cancel(const AccountId& account_id,
                                                OrderId order_id,
                                                SimTime arrival_time) {
    SeqNum seq = ++next_cmd_seq_;
    CancelOrderCommand cmd{seq, order_id};

    scheduler_.schedule(arrival_time, EventPriority::BackgroundArrival, [this, account_id, cmd](SimTime) {
        this->process_cancel_admission(account_id, cmd);
    });
}

void SimulationEngine::emit_market_observation(SimTime time,
                                               std::optional<Price> best_bid,
                                               std::optional<Price> best_ask,
                                               std::optional<Price> mid_price,
                                               std::optional<Price> last_trade_price,
                                               Quantity last_trade_qty) {
    MarketObservation obs{time, best_bid, best_ask, mid_price, last_trade_price, last_trade_qty};

    // 1. Authoritative runtime state update executes only at virtual timestamp 'time'
    scheduler_.schedule(time, EventPriority::MarketDataAuthoritative, [this, mid_price, last_trade_price, time](SimTime) {
        if (mid_price) {
            this->last_mark_price_ = mid_price;
        } else if (last_trade_price) {
            this->last_mark_price_ = last_trade_price;
        }
        this->last_market_data_time_ = time;
    });

    // 2. Schedule strategy observation delivery at time + market_data_delay
    auto del_opt = safe_add_time(time, config_.latencies.market_data_delay);
    if (!del_opt) return; // Time overflow
    SimTime delivery_time = *del_opt;

    scheduler_.schedule(delivery_time, EventPriority::MarketDataDelivery, [this, obs](SimTime t) {
        if (strategy_) {
            strategy_->on_market_data(t, obs, *this);
        }
    });
}

void SimulationEngine::process_order_admission(const AccountId& account_id,
                                               NewOrderCommand cmd,
                                               Price protection_price) {
    SimTime now = scheduler_.now();

    // 1. Pre-trade Risk Evaluation & Atomic Reservation
    auto risk_res = risk_engine_.evaluate_and_reserve(
        account_id, cmd, protection_price, now, last_market_data_time_, book_, ledger_
    );

    if (!risk_res.approved) {
        if (account_id == config_.strategy_account_id) {
            ++strategy_risk_rejections_;
            // Schedule rejected report delivery to strategy
            auto rpt_time = safe_add_time(now, config_.latencies.execution_report_delay);
            if (rpt_time) {
                ExecutionReport rpt{ExecutionReport::Type::Rejected, *rpt_time, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity, std::string(to_string(risk_res.reject_reason))};
                scheduler_.schedule(*rpt_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                    if (strategy_) strategy_->on_execution_report(t, rpt, *this);
                });
            }
        }

        std::ostringstream ss;
        ss << "{\"event\":\"RISK_REJECTED\",\"time\":" << now
           << ",\"id\":" << cmd.order_id
           << ",\"account\":\"" << account_id
           << "\",\"reason\":\"" << to_string(risk_res.reject_reason) << "\"}";
        event_log_lines_.push_back(ss.str());
        return;
    }

    if (account_id == config_.strategy_account_id) {
        ++strategy_orders_accepted_;
    }

    // 2. Submit to Matching Engine with protected IOC execution for market buys
    std::vector<Event> matcher_events;
    NewOrderCommand matcher_cmd = cmd;
    if (cmd.type == OrderType::Market && cmd.side == Side::Buy) {
        matcher_cmd.type = OrderType::Limit;
        matcher_cmd.price = protection_price;
        matcher_cmd.tif = TimeInForce::IOC;
    }
    book_.process(matcher_cmd, matcher_events);

    // 3. Process Engine Events & Settle
    for (const auto& ev : matcher_events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, OrderAcceptedEvent>) {
                std::ostringstream ss;
                ss << "{\"event\":\"ACCEPTED\",\"time\":" << now
                   << ",\"id\":" << e.order_id
                   << ",\"account\":\"" << account_id
                   << "\",\"side\":\"" << to_string(e.side)
                   << "\",\"price\":" << e.price
                   << ",\"qty\":" << e.quantity << "}";
                event_log_lines_.push_back(ss.str());

                if (account_id == config_.strategy_account_id) {
                    auto rpt_time = safe_add_time(now, config_.latencies.execution_report_delay);
                    SimTime del_time = rpt_time ? *rpt_time : now;
                    ExecutionReport rpt{ExecutionReport::Type::Accepted, del_time, e.order_id, e.side, e.price, e.quantity, e.quantity, ""};
                    scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                        if (strategy_) strategy_->on_execution_report(t, rpt, *this);
                    });
                }
            } else if constexpr (std::is_same_v<T, TradeEvent>) {
                auto maker_owner_opt = ledger_.get_order_owner(e.maker_order_id);
                AccountId maker_acc = maker_owner_opt ? *maker_owner_opt : "UNKNOWN";

                int64_t notional = e.price * e.quantity;
                int64_t taker_fee = config_.fees.calculate_taker_fee(notional);
                int64_t maker_fee = config_.fees.calculate_maker_fee(notional);

                uint64_t tid = allocate_trade_id();
                TradeSettlement settlement{
                    tid, now, e.taker_order_id, e.maker_order_id,
                    account_id, maker_acc, e.taker_side,
                    e.price, e.quantity, notional, taker_fee, maker_fee
                };

                // Authoritative settlement at execution time
                ledger_.settle_trade(settlement);
                fill_log_.push_back(settlement);

                ++total_trades_;
                total_volume_traded_ += e.quantity;
                total_notional_traded_ += notional;

                if (account_id == config_.strategy_account_id || maker_acc == config_.strategy_account_id) {
                    ++strategy_trades_;
                }

                last_mark_price_ = e.price;

                std::ostringstream ss;
                ss << "{\"event\":\"TRADE\",\"time\":" << now
                   << ",\"trade_id\":" << tid
                   << ",\"taker_id\":" << e.taker_order_id
                   << ",\"maker_id\":" << e.maker_order_id
                   << ",\"taker_acc\":\"" << account_id
                   << "\",\"maker_acc\":\"" << maker_acc
                   << "\",\"price\":" << e.price
                   << ",\"qty\":" << e.quantity
                   << ",\"notional\":" << notional
                   << ",\"taker_fee\":" << taker_fee
                   << ",\"maker_fee\":" << maker_fee << "}";
                event_log_lines_.push_back(ss.str());

                // Deliver reports to strategy if involved
                auto rpt_time = safe_add_time(now, config_.latencies.execution_report_delay);
                SimTime del_time = rpt_time ? *rpt_time : now;

                if (account_id == config_.strategy_account_id) {
                    ExecutionReport rpt{ExecutionReport::Type::Trade, del_time, e.taker_order_id, e.taker_side, e.price, e.quantity, 0, ""};
                    scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                        if (strategy_) strategy_->on_execution_report(t, rpt, *this);
                    });
                }
                if (maker_acc == config_.strategy_account_id) {
                    Side maker_side = (e.taker_side == Side::Buy) ? Side::Sell : Side::Buy;
                    ExecutionReport rpt{ExecutionReport::Type::Trade, del_time, e.maker_order_id, maker_side, e.price, e.quantity, e.maker_remaining_qty, ""};
                    scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                        if (strategy_) strategy_->on_execution_report(t, rpt, *this);
                    });
                }
            } else if constexpr (std::is_same_v<T, OrderCanceledEvent>) {
                ledger_.release_reservation_on_cancel(e.order_id, e.canceled_quantity);

                std::ostringstream ss;
                ss << "{\"event\":\"CANCELED\",\"time\":" << now
                   << ",\"id\":" << e.order_id
                   << ",\"canceled_qty\":" << e.canceled_quantity
                   << ",\"reason\":\"" << to_string(e.reason) << "\"}";
                event_log_lines_.push_back(ss.str());

                if (account_id == config_.strategy_account_id) {
                    auto rpt_time = safe_add_time(now, config_.latencies.execution_report_delay);
                    SimTime del_time = rpt_time ? *rpt_time : now;
                    ExecutionReport rpt{ExecutionReport::Type::Canceled, del_time, e.order_id, Side::Buy, 0, e.canceled_quantity, 0, std::string(to_string(e.reason))};
                    scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                        if (strategy_) strategy_->on_execution_report(t, rpt, *this);
                    });
                }
            } else if constexpr (std::is_same_v<T, OrderRejectedEvent>) {
                ledger_.release_reservation_on_reject(e.order_id);

                std::ostringstream ss;
                ss << "{\"event\":\"MATCHER_REJECTED\",\"time\":" << now
                   << ",\"id\":" << e.order_id
                   << ",\"reason\":\"" << to_string(e.reason) << "\"}";
                event_log_lines_.push_back(ss.str());

                if (account_id == config_.strategy_account_id) {
                    auto rpt_time = safe_add_time(now, config_.latencies.execution_report_delay);
                    SimTime del_time = rpt_time ? *rpt_time : now;
                    ExecutionReport rpt{ExecutionReport::Type::Rejected, del_time, e.order_id, Side::Buy, 0, 0, 0, std::string(to_string(e.reason))};
                    scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                        if (strategy_) strategy_->on_execution_report(t, rpt, *this);
                    });
                }
            }
        }, ev);
    }

    // Emit updated market observation after order execution
    auto best_bid = book_.best_bid();
    auto best_ask = book_.best_ask();
    std::optional<Price> mid;
    if (best_bid && best_ask) {
        mid = (*best_bid + *best_ask) / 2;
    }
    emit_market_observation(now, best_bid, best_ask, mid, last_mark_price_, 0);
}

void SimulationEngine::process_cancel_admission(const AccountId& account_id,
                                                CancelOrderCommand cmd) {
    SimTime now = scheduler_.now();

    // Check ownership before canceling
    if (!risk_engine_.can_cancel_order(account_id, cmd.order_id, ledger_)) {
        if (account_id == config_.strategy_account_id) {
            auto rpt_time = safe_add_time(now, config_.latencies.execution_report_delay);
            SimTime del_time = rpt_time ? *rpt_time : now;
            ExecutionReport rpt{ExecutionReport::Type::Rejected, del_time, cmd.order_id, Side::Buy, 0, 0, 0, "UNAUTHORIZED_ORDER_CANCELLATION"};
            scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                if (strategy_) strategy_->on_execution_report(t, rpt, *this);
            });
        }
        std::ostringstream ss;
        ss << "{\"event\":\"RISK_REJECTED\",\"time\":" << now
           << ",\"id\":" << cmd.order_id
           << ",\"account\":\"" << account_id
           << "\",\"reason\":\"UNAUTHORIZED_ORDER_CANCELLATION\"}";
        event_log_lines_.push_back(ss.str());
        return;
    }

    std::vector<Event> matcher_events;
    book_.process(cmd, matcher_events);

    for (const auto& ev : matcher_events) {
        std::visit([&](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, OrderCanceledEvent>) {
                ledger_.release_reservation_on_cancel(e.order_id, e.canceled_quantity);

                std::ostringstream ss;
                ss << "{\"event\":\"CANCELED\",\"time\":" << now
                   << ",\"id\":" << e.order_id
                   << ",\"canceled_qty\":" << e.canceled_quantity
                   << ",\"reason\":\"" << to_string(e.reason) << "\"}";
                event_log_lines_.push_back(ss.str());

                if (account_id == config_.strategy_account_id) {
                    auto rpt_time = safe_add_time(now, config_.latencies.execution_report_delay);
                    SimTime del_time = rpt_time ? *rpt_time : now;
                    ExecutionReport rpt{ExecutionReport::Type::Canceled, del_time, e.order_id, Side::Buy, 0, e.canceled_quantity, 0, std::string(to_string(e.reason))};
                    scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                        if (strategy_) strategy_->on_execution_report(t, rpt, *this);
                    });
                }
            } else if constexpr (std::is_same_v<T, OrderRejectedEvent>) {
                std::ostringstream ss;
                ss << "{\"event\":\"MATCHER_REJECTED\",\"time\":" << now
                   << ",\"id\":" << e.order_id
                   << ",\"reason\":\"" << to_string(e.reason) << "\"}";
                event_log_lines_.push_back(ss.str());

                if (account_id == config_.strategy_account_id) {
                    auto rpt_time = safe_add_time(now, config_.latencies.execution_report_delay);
                    SimTime del_time = rpt_time ? *rpt_time : now;
                    ExecutionReport rpt{ExecutionReport::Type::Rejected, del_time, e.order_id, Side::Buy, 0, 0, 0, std::string(to_string(e.reason))};
                    scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                        if (strategy_) strategy_->on_execution_report(t, rpt, *this);
                    });
                }
            }
        }, ev);
    }
}

SimulationSummary SimulationEngine::run() {
    TerminationReason term_reason = TerminationReason::AllEventsProcessed;

    while (scheduler_.pending_event_count() > 0) {
        if (scheduler_.now() >= config_.run_horizon_ns) {
            term_reason = TerminationReason::TimeHorizonReached;
            break;
        }
        if (scheduler_.total_events_processed() >= config_.max_events) {
            term_reason = TerminationReason::EventLimitReached;
            break;
        }

        bool stepped = scheduler_.step_one(config_.run_horizon_ns);
        if (!stepped) {
            term_reason = TerminationReason::TimeHorizonReached;
            break;
        }
    }

    SimulationSummary summary;
    summary.termination_reason = term_reason;
    summary.simulated_time_ns = scheduler_.now();
    summary.total_events_processed = scheduler_.total_events_processed();

    summary.initial_total_cash = initial_total_cash_;
    summary.final_total_cash = ledger_.get_total_cash();

    summary.initial_total_inventory = initial_total_inventory_;
    summary.final_total_inventory = ledger_.get_total_inventory();

    auto audit = ledger_.verify_all_invariants(initial_total_cash_, initial_total_inventory_);
    summary.cash_conserved = audit.ok && ledger_.verify_cash_conservation(initial_total_cash_);
    summary.inventory_conserved = audit.ok && ledger_.verify_inventory_conservation(initial_total_inventory_);

    const auto* strat_pos = ledger_.get_account(config_.strategy_account_id);
    if (strat_pos) {
        summary.strategy_final_cash = strat_pos->cash;
        summary.strategy_final_inventory = strat_pos->inventory;
        summary.strategy_realized_pnl = strat_pos->realized_pnl;
        summary.strategy_unrealized_pnl = strat_pos->calculate_unrealized_pnl(last_mark_price_);
        summary.strategy_total_pnl = summary.strategy_unrealized_pnl
            ? (summary.strategy_realized_pnl + *summary.strategy_unrealized_pnl)
            : std::optional<int64_t>(summary.strategy_realized_pnl);
        summary.strategy_fees_paid = strat_pos->total_fees_paid;
    }

    summary.strategy_orders_submitted = strategy_orders_submitted_;
    summary.strategy_orders_accepted = strategy_orders_accepted_;
    summary.strategy_risk_rejections = strategy_risk_rejections_;
    summary.strategy_trades = strategy_trades_;

    const auto* fee_pos = ledger_.get_account(config_.fees.fee_account_id);
    if (fee_pos) {
        summary.total_fees_collected = fee_pos->cash;
    }

    summary.total_trades = total_trades_;
    summary.total_volume_traded = total_volume_traded_;
    summary.total_notional_traded = total_notional_traded_;

    return summary;
}

void SimulationEngine::setup_synthetic_scenario() {
    std::mt19937_64 rng(config_.seed);
    Price current_mid = 100;
    Price min_mid = 95;
    Price max_mid = 105;

    SimTime step_ns = 100000000ULL; // 100ms
    SimTime horizon = config_.run_horizon_ns;

    for (SimTime t = step_ns; t < horizon; t += step_ns) {
        // Deterministic price random walk (-1, 0, +1)
        int dir = static_cast<int>(rng() % 3) - 1;
        current_mid += dir;
        if (current_mid < min_mid) current_mid = min_mid;
        if (current_mid > max_mid) current_mid = max_mid;

        Price best_bid = current_mid - 1;
        Price best_ask = current_mid + 1;
        emit_market_observation(t, best_bid, best_ask, current_mid, current_mid, 10);

        // Background counterparty order 40ms later
        SimTime bg_time = t + 40000000ULL;
        if (bg_time < horizon) {
            bool is_buy = (rng() % 2 == 0);
            Side bg_side = is_buy ? Side::Buy : Side::Sell;
            Price bg_price = is_buy ? (current_mid + 1) : (current_mid - 1);
            Quantity bg_qty = 5 + (rng() % 6); // 5 to 10 lots
            submit_background_order(config_.background_account_id,
                                    bg_side,
                                    OrderType::Limit,
                                    bg_price,
                                    bg_qty,
                                    TimeInForce::IOC,
                                    bg_time);
        }
    }
}

std::string SimulationEngine::get_events_content() const {
    std::string out;
    for (const auto& line : event_log_lines_) {
        out += line;
        out += '\n';
    }
    return out;
}

std::string SimulationEngine::get_fills_content() const {
    std::string out;
    for (const auto& fill : fill_log_) {
        std::ostringstream ss;
        ss << "{\"trade_id\":" << fill.trade_id
           << ",\"time\":" << fill.execution_time
           << ",\"taker_order_id\":" << fill.taker_order_id
           << ",\"maker_order_id\":" << fill.maker_order_id
           << ",\"taker_acc\":\"" << fill.taker_account_id
           << "\",\"maker_acc\":\"" << fill.maker_account_id
           << "\",\"taker_side\":\"" << to_string(fill.taker_side)
           << "\",\"price\":" << fill.price
           << ",\"qty\":" << fill.quantity
           << ",\"notional\":" << fill.notional
           << ",\"taker_fee\":" << fill.taker_fee
           << ",\"maker_fee\":" << fill.maker_fee << "}\n";
        out += ss.str();
    }
    return out;
}

std::string SimulationEngine::get_state_content() const {
    std::ostringstream ss;
    ss << "{\"accounts\":[";
    bool first = true;
    for (const auto& [id, pos] : ledger_.accounts()) {
        if (!first) ss << ",";
        first = false;
        ss << "{\"id\":\"" << id
           << "\",\"cash\":" << pos.cash
           << ",\"inventory\":" << pos.inventory
           << ",\"reserved_cash\":" << pos.reserved_cash
           << ",\"reserved_inventory\":" << pos.reserved_inventory
           << ",\"pending_buy_qty\":" << pos.pending_buy_quantity
           << ",\"total_cost\":" << pos.total_cost
           << ",\"fees_paid\":" << pos.total_fees_paid
           << ",\"realized_pnl\":" << pos.realized_pnl
           << ",\"avg_cost\":" << pos.cost_basis_per_lot() << "}";
    }
    ss << "],\"book\":" << Serializer::to_json(book_.get_snapshot()) << "}\n";
    return ss.str();
}

std::string SimulationEngine::get_manifest_content(const std::string& config_content) const {
    std::string config_hash = Sha256::hash_string(config_content);
    std::ostringstream ss;
    ss << "{\"schema_version\":\"marketlab.v1\""
       << ",\"seed\":" << config_.seed
       << ",\"run_horizon_ns\":" << config_.run_horizon_ns
       << ",\"config_hash\":\"" << config_hash << "\""
       << ",\"code_revision\":\"" << MARKETLAB_BUILD_REVISION << "\""
       << ",\"environment\":\"" << get_build_environment() << "\""
       << "}\n";
    return ss.str();
}

bool SimulationEngine::export_events_binary(const std::string& path) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    std::string content = get_events_content();
    ofs.write(content.data(), content.size());
    ofs.flush();
    return true;
}

bool SimulationEngine::export_fills_binary(const std::string& path) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    std::string content = get_fills_content();
    ofs.write(content.data(), content.size());
    ofs.flush();
    return true;
}

bool SimulationEngine::export_state_binary(const std::string& path) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    std::string content = get_state_content();
    ofs.write(content.data(), content.size());
    ofs.flush();
    return true;
}

bool SimulationEngine::export_manifest_binary(const std::string& path, const std::string& config_content) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    std::string content = get_manifest_content(config_content);
    ofs.write(content.data(), content.size());
    ofs.flush();
    return true;
}

} // namespace marketlab
