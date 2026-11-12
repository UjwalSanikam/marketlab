#include "marketlab/historical_replay_engine.hpp"
#include "marketlab/sha256.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>

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
    env += std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__);
#elif defined(_MSC_VER)
    env += "MSVC ";
    env += std::to_string(_MSC_VER);
#else
    env += "Unknown Compiler";
#endif
    return env;
}

} // namespace

HistoricalReplayEngine::HistoricalReplayEngine(HistoricalReplayConfig config)
    : config_(std::move(config)),
      book_(config_.instrument),
      ledger_(config_.fees),
      risk_engine_(config_.risk),
      execution_adapter_(book_, ledger_, risk_engine_, config_.fees, config_.execution_model),
      strategy_(std::make_unique<InventoryAwareMarketMaker>(config_.strategy_config)) {

    ledger_.fund_account(config_.strategy_account_id,
                         config_.strategy_starting_cash,
                         config_.strategy_starting_inventory);
}

void HistoricalReplayEngine::load_events(std::vector<NormalizedEvent> events) {
    loaded_events_ = std::move(events);
    std::stable_sort(loaded_events_.begin(), loaded_events_.end(), [](const auto& a, const auto& b) {
        return get_event_time(a) < get_event_time(b);
    });
}

HistoricalReplaySummary HistoricalReplayEngine::run() {
    int64_t init_cash = ledger_.get_total_cash();
    Quantity init_inv = ledger_.get_total_inventory();

    if (!loaded_events_.empty()) {
        first_event_time_ = get_event_time(loaded_events_.front());
        last_event_time_ = get_event_time(loaded_events_.back());
    }

    // Schedule all historical normalized events
    for (const auto& ev : loaded_events_) {
        SimTime t = get_event_time(ev);

        std::visit([this, t](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, NormalizedSnapshot>) {
                scheduler_.schedule(t, EventPriority::MarketDataAuthoritative, [this, e, t](SimTime) {
                    this->book_.apply_snapshot(e.last_update_id, e.bids, e.asks);
                    this->execution_adapter_.on_snapshot();
                    this->event_log_lines_.push_back(MarketDataNormalizer::to_json(e));
                    this->schedule_observation_delivery(t, this->book_.best_bid(), this->book_.best_ask(),
                                                        this->book_.mid_price(), std::nullopt, 0);
                });
            } else if constexpr (std::is_same_v<T, NormalizedDepthUpdate>) {
                scheduler_.schedule(t, EventPriority::MarketDataAuthoritative, [this, e, t](SimTime) {
                    this->book_.apply_depth_update(e.first_update_id, e.final_update_id, e.bids, e.asks);
                    this->execution_adapter_.on_depth_update(e);
                    this->event_log_lines_.push_back(MarketDataNormalizer::to_json(e));
                    this->schedule_observation_delivery(t, this->book_.best_bid(), this->book_.best_ask(),
                                                        this->book_.mid_price(), std::nullopt, 0);
                });
            } else if constexpr (std::is_same_v<T, NormalizedTrade>) {
                scheduler_.schedule(t, EventPriority::MarketDataAuthoritative, [this, e, t](SimTime) {
                    this->event_log_lines_.push_back(MarketDataNormalizer::to_json(e));
                    std::vector<ExecutionReport> reports;
                    std::vector<TradeSettlement> settlements;
                    this->execution_adapter_.on_public_trade(e, reports, settlements);

                    // Deliver reports and log fills
                    for (const auto& rpt : reports) {
                        auto del_opt = safe_add_time(t, this->config_.latencies.execution_report_delay);
                        SimTime del_time = del_opt ? *del_opt : t;
                        this->scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime now) {
                            if (this->strategy_) this->strategy_->on_execution_report(now, rpt, *this);
                        });

                        std::ostringstream ss;
                        ss << "{\"event\":\"REPORT\",\"time\":" << t
                           << ",\"type\":\"" << ((rpt.type == ExecutionReport::Type::Trade) ? "TRADE" : "CANCEL")
                           << "\",\"id\":" << rpt.order_id
                           << ",\"side\":\"" << to_string(rpt.side)
                           << "\",\"price\":" << rpt.price
                           << ",\"fill_qty\":" << (rpt.quantity - rpt.remaining_quantity)
                           << ",\"rem_qty\":" << rpt.remaining_quantity << "}";
                        this->fill_log_lines_.push_back(ss.str());
                    }

                    this->schedule_observation_delivery(t, this->book_.best_bid(), this->book_.best_ask(),
                                                        this->book_.mid_price(), e.price, e.quantity);
                });
            } else if constexpr (std::is_same_v<T, NormalizedFeedStatus>) {
                scheduler_.schedule(t, EventPriority::MarketDataAuthoritative, [this, e](SimTime) {
                    if (e.status == FeedStatus::Invalid) {
                        this->book_.mark_invalid(e.reason);
                    }
                    this->event_log_lines_.push_back(MarketDataNormalizer::to_json(e));
                });
            }
        }, ev);
    }

    // Notify strategy start at first event time
    if (strategy_) {
        scheduler_.schedule(first_event_time_, EventPriority::StrategyWakeup, [this](SimTime now) {
            this->strategy_->on_start(now, *this);
        });
    }

    // Step virtual scheduler
    uint64_t events_processed = 0;
    TerminationReason term_reason = TerminationReason::AllEventsProcessed;
    SimTime max_horizon = config_.run_horizon_ns > 0 ? config_.run_horizon_ns : UINT64_MAX;

    while (scheduler_.pending_event_count() > 0) {
        if (config_.run_horizon_ns > 0 && scheduler_.now() >= config_.run_horizon_ns) {
            term_reason = TerminationReason::TimeHorizonReached;
            break;
        }

        bool stepped = scheduler_.step_one(max_horizon);
        if (!stepped) {
            term_reason = TerminationReason::TimeHorizonReached;
            break;
        }
        ++events_processed;
    }

    int64_t final_cash = ledger_.get_total_cash();
    Quantity final_inv = ledger_.get_total_inventory();

    HistoricalReplaySummary summary;
    summary.simulated_time_ns = scheduler_.now();
    summary.termination_reason = term_reason;
    summary.total_events_processed = events_processed;

    summary.final_feed_status = book_.status();
    summary.final_update_id = book_.last_update_id();
    summary.bid_depth_levels = book_.bids().size();
    summary.ask_depth_levels = book_.asks().size();
    summary.best_bid = book_.best_bid();
    summary.best_ask = book_.best_ask();

    const auto& st = execution_adapter_.stats();
    summary.strategy_orders_submitted = st.orders_submitted;
    summary.strategy_orders_accepted = st.orders_accepted;
    summary.strategy_risk_rejections = st.orders_rejected;
    summary.strategy_orders_canceled = st.orders_canceled;
    summary.strategy_aggressive_trades = st.aggressive_fills;
    summary.strategy_passive_trades = st.passive_fills;
    summary.strategy_total_trades = st.total_trades;
    summary.strategy_volume_traded = st.total_volume_traded;
    summary.strategy_notional_traded = st.total_notional_traded;
    summary.strategy_fees_paid = st.total_fees_paid;

    const auto* strat_pos = ledger_.get_account(config_.strategy_account_id);
    if (strat_pos) {
        summary.strategy_final_cash = strat_pos->cash;
        summary.strategy_final_inventory = strat_pos->inventory;
        summary.strategy_realized_pnl = strat_pos->realized_pnl;
        summary.strategy_unrealized_pnl = strat_pos->calculate_unrealized_pnl(book_.mid_price());
        summary.strategy_total_pnl = summary.strategy_unrealized_pnl
            ? (summary.strategy_realized_pnl + *summary.strategy_unrealized_pnl)
            : std::optional<int64_t>(summary.strategy_realized_pnl);
    }

    summary.initial_total_cash = init_cash;
    summary.final_total_cash = final_cash;
    summary.initial_total_inventory = init_inv;
    summary.final_total_inventory = final_inv;
    summary.cash_conserved = (init_cash == final_cash);
    summary.inventory_conserved = (init_inv == final_inv);

    return summary;
}

void HistoricalReplayEngine::schedule_observation_delivery(SimTime time,
                                                           std::optional<Price> best_bid,
                                                           std::optional<Price> best_ask,
                                                           std::optional<Price> mid_price,
                                                           std::optional<Price> last_trade_price,
                                                           Quantity last_trade_qty) {
    auto del_opt = safe_add_time(time, config_.latencies.market_data_delay);
    if (!del_opt) return;
    SimTime delivery_time = *del_opt;

    MarketObservation obs{time, best_bid, best_ask, mid_price, last_trade_price, last_trade_qty};
    scheduler_.schedule(delivery_time, EventPriority::MarketDataDelivery, [this, obs](SimTime t) {
        if (this->strategy_) {
            this->strategy_->on_market_data(t, obs, *this);
        }
    });
}

void HistoricalReplayEngine::submit_order(Side side, OrderType type, Price price, Quantity qty, TimeInForce tif) {
    OrderId id = ++next_order_id_;
    SeqNum seq = ++next_cmd_seq_;
    NewOrderCommand cmd{seq, id, side, type, price, qty, tif};

    auto delay_opt = safe_add_time(config_.latencies.strategy_delay, config_.latencies.order_entry_delay);
    if (!delay_opt) return;
    auto arr_opt = safe_add_time(scheduler_.now(), *delay_opt);
    if (!arr_opt) return;
    SimTime arrival_time = *arr_opt;

    Price protection_price = price;
    if (type == OrderType::Market && side == Side::Buy) {
        if (price > 0) {
            protection_price = price;
        } else if (book_.best_ask().has_value()) {
            protection_price = *book_.best_ask() + 500; // Protection cap
        } else {
            protection_price = 0;
        }
    }

    scheduler_.schedule(arrival_time, EventPriority::OrderAdmission, [this, cmd, protection_price](SimTime) {
        this->process_strategy_order_admission(cmd, protection_price);
    });
}

void HistoricalReplayEngine::cancel_order(OrderId order_id) {
    SeqNum seq = ++next_cmd_seq_;
    CancelOrderCommand cmd{seq, order_id};

    auto delay_opt = safe_add_time(config_.latencies.strategy_delay, config_.latencies.order_entry_delay);
    if (!delay_opt) return;
    auto arr_opt = safe_add_time(scheduler_.now(), *delay_opt);
    if (!arr_opt) return;
    SimTime arrival_time = *arr_opt;

    scheduler_.schedule(arrival_time, EventPriority::OrderAdmission, [this, cmd](SimTime) {
        this->process_strategy_cancel_admission(cmd);
    });
}

void HistoricalReplayEngine::process_strategy_order_admission(NewOrderCommand cmd, Price protection_price) {
    SimTime now = scheduler_.now();
    std::vector<ExecutionReport> reports;
    std::vector<TradeSettlement> settlements;

    bool accepted = execution_adapter_.submit_order(
        now, config_.strategy_account_id, cmd, protection_price, reports, settlements
    );
    (void)accepted;

    for (const auto& rpt : reports) {
        auto del_opt = safe_add_time(now, config_.latencies.execution_report_delay);
        SimTime del_time = del_opt ? *del_opt : now;
        scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
            if (this->strategy_) this->strategy_->on_execution_report(t, rpt, *this);
        });

        std::ostringstream ss;
        ss << "{\"event\":\"REPORT\",\"time\":" << now
           << ",\"type\":\"" << ((rpt.type == ExecutionReport::Type::Accepted) ? "ACCEPTED" :
                                 (rpt.type == ExecutionReport::Type::Trade) ? "TRADE" :
                                 (rpt.type == ExecutionReport::Type::Rejected) ? "REJECTED" : "CANCELED")
           << "\",\"id\":" << rpt.order_id
           << ",\"side\":\"" << to_string(rpt.side)
           << "\",\"price\":" << rpt.price
           << ",\"qty\":" << rpt.quantity
           << ",\"rem_qty\":" << rpt.remaining_quantity;
        if (!rpt.reason.empty()) ss << ",\"reason\":\"" << rpt.reason << "\"";
        ss << "}";
        fill_log_lines_.push_back(ss.str());
    }
}

void HistoricalReplayEngine::process_strategy_cancel_admission(CancelOrderCommand cmd) {
    SimTime now = scheduler_.now();
    std::vector<ExecutionReport> reports;
    execution_adapter_.cancel_order(now, config_.strategy_account_id, cmd.order_id, reports);

    for (const auto& rpt : reports) {
        auto del_opt = safe_add_time(now, config_.latencies.execution_report_delay);
        SimTime del_time = del_opt ? *del_opt : now;
        scheduler_.schedule(del_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
            if (this->strategy_) this->strategy_->on_execution_report(t, rpt, *this);
        });

        std::ostringstream ss;
        ss << "{\"event\":\"REPORT\",\"time\":" << now
           << ",\"type\":\"CANCELED\",\"id\":" << rpt.order_id
           << ",\"side\":\"" << to_string(rpt.side)
           << "\",\"price\":" << rpt.price
           << ",\"qty\":" << rpt.quantity
           << ",\"rem_qty\":0,\"reason\":\"" << rpt.reason << "\"}";
        fill_log_lines_.push_back(ss.str());
    }
}

std::string HistoricalReplayEngine::get_events_content() const {
    std::ostringstream ss;
    for (const auto& line : event_log_lines_) {
        ss << line << "\n";
    }
    return ss.str();
}

std::string HistoricalReplayEngine::get_fills_content() const {
    std::ostringstream ss;
    for (const auto& line : fill_log_lines_) {
        ss << line << "\n";
    }
    return ss.str();
}

std::string HistoricalReplayEngine::get_state_content() const {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"feed_status\": \"" << to_string(book_.status()) << "\",\n"
       << "  \"last_update_id\": " << book_.last_update_id() << ",\n"
       << "  \"best_bid\": " << (book_.best_bid() ? std::to_string(*book_.best_bid()) : "null") << ",\n"
       << "  \"best_ask\": " << (book_.best_ask() ? std::to_string(*book_.best_ask()) : "null") << ",\n"
       << "  \"mid_price\": " << (book_.mid_price() ? std::to_string(*book_.mid_price()) : "null") << ",\n"
       << "  \"total_bid_depth\": " << book_.total_bid_quantity() << ",\n"
       << "  \"total_ask_depth\": " << book_.total_ask_quantity() << ",\n"
       << "  \"accounts\": [";

    std::map<AccountId, AccountPosition> sorted_accounts(ledger_.accounts().begin(), ledger_.accounts().end());
    bool first = true;
    for (const auto& [id, pos] : sorted_accounts) {
        if (!first) ss << ",";
        first = false;
        ss << "\n    {\"id\":\"" << id
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
    ss << "\n  ]\n}\n";
    return ss.str();
}

std::string HistoricalReplayEngine::get_manifest_content(const std::string& raw_input_hash) const {
    std::ostringstream ss;
    ss << "{\n"
       << "  \"manifest_version\": \"marketlab.manifest.v2\",\n"
       << "  \"mode\": \"HISTORICAL_REPLAY\",\n"
       << "  \"build_revision\": \"" << MARKETLAB_BUILD_REVISION << "\",\n"
       << "  \"build_environment\": \"" << get_build_environment() << "\",\n"
       << "  \"raw_input_checksum_sha256\": \"" << raw_input_hash << "\",\n"
       << "  \"instrument\": {\n"
       << "    \"symbol\": \"" << config_.instrument.symbol << "\",\n"
       << "    \"price_scale\": " << config_.instrument.price_scale << ",\n"
       << "    \"qty_scale\": " << config_.instrument.qty_scale << ",\n"
       << "    \"tick_size\": " << config_.instrument.tick_size << ",\n"
       << "    \"lot_size\": " << config_.instrument.lot_size << "\n"
       << "  },\n"
       << "  \"execution_model\": {\n"
       << "    \"queue_model\": \"" << to_string(config_.execution_model.queue_model) << "\",\n"
       << "    \"queue_position_pct\": " << config_.execution_model.queue_position_pct << ",\n"
       << "    \"impact_model\": \"" << to_string(config_.execution_model.impact_model) << "\",\n"
       << "    \"fixed_slippage_ticks\": " << config_.execution_model.fixed_slippage_ticks << ",\n"
       << "    \"impact_ticks_per_lot\": " << config_.execution_model.impact_ticks_per_lot << ",\n"
       << "    \"assumptions\": [\n"
       << "      \"L2 aggregated depth does not reveal exact FIFO queue position or hidden orders\",\n"
       << "      \"Queue ahead initialized based on queue_model (Conservative: 100%, Optimistic: 0%, Proportional: queue_position_pct%)\",\n"
       << "      \"Public trades deplete queue ahead before passive orders can fill\",\n"
       << "      \"Public trade volume cannot fill multiple hypothetical orders beyond actual volume\",\n"
       << "      \"Aggressive orders hypothetically consume visible depth until refreshed by subsequent depth updates\",\n"
       << "      \"Aggressive slippage/impact applied according to impact_model\",\n"
       << "      \"External counterparty HISTORICAL_MARKET ensures exact double-entry ledger conservation\"\n"
       << "    ]\n"
       << "  },\n"
       << "  \"latencies\": {\n"
       << "    \"market_data_delay_ns\": " << config_.latencies.market_data_delay << ",\n"
       << "    \"strategy_delay_ns\": " << config_.latencies.strategy_delay << ",\n"
       << "    \"order_entry_delay_ns\": " << config_.latencies.order_entry_delay << ",\n"
       << "    \"execution_report_delay_ns\": " << config_.latencies.execution_report_delay << "\n"
       << "  },\n"
       << "  \"strategy_account_id\": \"" << config_.strategy_account_id << "\"\n"
       << "}\n";
    return ss.str();
}

bool HistoricalReplayEngine::export_events_binary(const std::string& path) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    for (const auto& line : event_log_lines_) {
        ofs.write(line.data(), line.size());
        ofs.put('\n');
    }
    ofs.flush();
    return true;
}

bool HistoricalReplayEngine::export_fills_binary(const std::string& path) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    for (const auto& line : fill_log_lines_) {
        ofs.write(line.data(), line.size());
        ofs.put('\n');
    }
    ofs.flush();
    return true;
}

bool HistoricalReplayEngine::export_state_binary(const std::string& path) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    std::string s = get_state_content();
    ofs.write(s.data(), s.size());
    ofs.flush();
    return true;
}

bool HistoricalReplayEngine::export_manifest_binary(const std::string& path, const std::string& raw_input_hash) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    std::string s = get_manifest_content(raw_input_hash);
    ofs.write(s.data(), s.size());
    ofs.flush();
    return true;
}

} // namespace marketlab
