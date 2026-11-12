#include "marketlab/backtest_engine.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cmath>

namespace marketlab {

BacktestEngine::BacktestEngine(BacktestConfig config)
    : config_(std::move(config)),
      ledger_(config_.fees),
      risk_engine_(config_.risk),
      execution_adapter_(book_, ledger_, risk_engine_, config_.fees, config_.execution_model) {
    // Fund strategy account and fee counterparty
    ledger_.fund_account(config_.strategy_account_id,
                         config_.strategy_starting_cash,
                         config_.strategy_starting_inventory);
    ledger_.fund_account(config_.fees.fee_account_id, 0, 0);

    // Default strategy if not overridden
    strategy_ = std::make_unique<ReferenceStrategy>(config_.strategy_config);

    init_system_cash_ = ledger_.get_total_cash();
    init_system_inv_ = ledger_.get_total_inventory();

    min_inventory_ = config_.strategy_starting_inventory;
    max_inventory_ = config_.strategy_starting_inventory;
    peak_abs_inventory_ = std::abs(config_.strategy_starting_inventory);

    starting_equity_ = config_.strategy_starting_cash;
    peak_equity_ = starting_equity_;
    max_drawdown_ = 0;
}

void BacktestEngine::load_events(std::vector<NormalizedEvent> events) {
    loaded_events_ = std::move(events);
    std::stable_sort(loaded_events_.begin(), loaded_events_.end(), [](const auto& a, const auto& b) {
        return get_event_time(a) < get_event_time(b);
    });

    if (!loaded_events_.empty()) {
        first_event_time_ = get_event_time(loaded_events_.front());
        last_event_time_ = get_event_time(loaded_events_.back());
    }
}

void BacktestEngine::record_mid_price(SimTime time) {
    mid_history_.push_back({time, book_.mid_price()});
}

void BacktestEngine::evaluate_equity(SimTime) {
    const auto* acc = ledger_.get_account(config_.strategy_account_id);
    if (!acc) return;

    min_inventory_ = std::min(min_inventory_, acc->inventory);
    max_inventory_ = std::max(max_inventory_, acc->inventory);
    peak_abs_inventory_ = std::max<Quantity>(peak_abs_inventory_, std::abs(acc->inventory));

    auto mid = book_.mid_price();
    if (mid) {
        int64_t current_val = (*mid) * acc->inventory;
        int64_t equity = acc->cash + current_val;

        if (starting_equity_ == config_.strategy_starting_cash && acc->inventory != 0 && mid_history_.size() <= 2) {
            starting_equity_ = acc->cash + current_val;
            peak_equity_ = starting_equity_;
        }

        if (equity > peak_equity_) {
            peak_equity_ = equity;
        }
        int64_t dd = peak_equity_ - equity;
        if (dd > max_drawdown_) {
            max_drawdown_ = dd;
        }
    }
}

void BacktestEngine::record_trade(SimTime time, OrderId order_id, Side side, Price price, Quantity qty, int64_t fee, bool is_aggressive) {
    ++next_trade_id_;
    TradeRecord tr;
    tr.trade_id = next_trade_id_;
    tr.execution_time = time;
    tr.order_id = order_id;
    tr.side = side;
    tr.price = price;
    tr.quantity = qty;
    tr.notional = static_cast<int64_t>(price) * qty;
    tr.fee = fee;
    tr.is_aggressive = is_aggressive;
    recorded_trades_.push_back(tr);

    evaluate_equity(time);
}

BacktestReport BacktestEngine::run() {
    scheduler_ = VirtualScheduler();
    event_log_lines_.clear();
    fill_log_lines_.clear();
    mid_history_.clear();
    recorded_trades_.clear();
    next_trade_id_ = 0;

    for (const auto& ev : loaded_events_) {
        SimTime t = get_event_time(ev);

        std::visit([this, t](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, NormalizedSnapshot>) {
                scheduler_.schedule(t, EventPriority::MarketDataAuthoritative, [this, e, t](SimTime) {
                    this->book_.apply_snapshot(e.last_update_id, e.bids, e.asks);
                    this->execution_adapter_.on_snapshot();
                    this->record_mid_price(t);
                    this->evaluate_equity(t);
                    this->event_log_lines_.push_back(MarketDataNormalizer::to_json(e));
                    this->schedule_observation_delivery(t, this->book_.best_bid(), this->book_.best_ask(),
                                                        this->book_.mid_price(), std::nullopt, 0);
                });
            } else if constexpr (std::is_same_v<T, NormalizedDepthUpdate>) {
                scheduler_.schedule(t, EventPriority::MarketDataAuthoritative, [this, e, t](SimTime) {
                    this->book_.apply_depth_update(e.first_update_id, e.final_update_id, e.bids, e.asks);
                    this->execution_adapter_.on_depth_update(e);
                    this->record_mid_price(t);
                    this->evaluate_equity(t);
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

                    // Record settlements matching strategy passive orders
                    for (const auto& st : settlements) {
                        if (st.maker_account_id == this->config_.strategy_account_id) {
                            Side passive_side = (st.taker_side == Side::Buy) ? Side::Sell : Side::Buy;
                            this->record_trade(t, st.maker_order_id, passive_side, st.price, st.quantity, st.maker_fee, false);
                        }
                    }

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

                    this->record_mid_price(t);
                    this->evaluate_equity(t);
                    this->schedule_observation_delivery(t, this->book_.best_bid(), this->book_.best_ask(),
                                                        this->book_.mid_price(), e.price, e.quantity);
                });
            } else if constexpr (std::is_same_v<T, NormalizedFeedStatus>) {
                scheduler_.schedule(t, EventPriority::MarketDataAuthoritative, [this, e, t](SimTime) {
                    if (e.status == FeedStatus::Invalid) {
                        this->book_.mark_invalid(e.reason);
                    }
                    this->record_mid_price(t);
                    this->event_log_lines_.push_back(MarketDataNormalizer::to_json(e));
                });
            }
        }, ev);
    }

    // Wakeup strategy at first event time
    if (strategy_) {
        scheduler_.schedule(first_event_time_, EventPriority::StrategyWakeup, [this](SimTime now) {
            this->strategy_->on_start(now, *this);
        });
    }

    uint64_t events_processed = 0;
    SimTime max_horizon = config_.run_horizon_ns > 0 ? config_.run_horizon_ns : UINT64_MAX;

    while (scheduler_.pending_event_count() > 0) {
        if (config_.run_horizon_ns > 0 && scheduler_.now() >= config_.run_horizon_ns) {
            break;
        }
        bool stepped = scheduler_.step_one(max_horizon);
        if (!stepped) break;
        ++events_processed;
    }

    SimTime session_end = scheduler_.now();

    // Prepare BacktestReport
    BacktestReport report;
    report.strategy_name = "ReferenceStrategy";
    report.strategy_account_id = config_.strategy_account_id;
    report.instrument = config_.instrument;
    report.execution_model = config_.execution_model;

    report.start_time_ns = first_event_time_;
    report.end_time_ns = session_end;
    report.duration_ns = (session_end >= first_event_time_) ? (session_end - first_event_time_) : 0;
    report.total_events_processed = events_processed;
    report.final_feed_status = book_.status();

    const auto* acc = ledger_.get_account(config_.strategy_account_id);
    if (acc) {
        report.initial_cash = config_.strategy_starting_cash;
        report.final_cash = acc->cash;
        report.initial_inventory = config_.strategy_starting_inventory;
        report.final_inventory = acc->inventory;

        auto final_mid = book_.mid_price();
        int64_t final_val = final_mid ? ((*final_mid) * acc->inventory) : 0;
        report.ending_equity = acc->cash + final_val;
        report.starting_equity = starting_equity_;

        report.realized_pnl = acc->realized_pnl;
        auto unpnl = acc->calculate_unrealized_pnl(final_mid);
        report.unrealized_pnl = unpnl.value_or(0);
        report.net_pnl = report.realized_pnl + report.unrealized_pnl - execution_adapter_.stats().total_fees_paid;
        report.gross_pnl = report.net_pnl + execution_adapter_.stats().total_fees_paid;
    }

    const auto& st = execution_adapter_.stats();
    report.orders_submitted = orders_submitted_;
    report.orders_accepted = st.orders_accepted;
    report.orders_rejected = st.orders_rejected;
    report.orders_canceled = st.orders_canceled;
    report.total_trades = st.total_trades;
    report.aggressive_trades = st.aggressive_fills;
    report.passive_trades = st.passive_fills;
    report.total_order_qty_submitted = total_order_qty_submitted_;
    report.filled_quantity = st.total_volume_traded;
    report.total_fees_paid = st.total_fees_paid;
    report.turnover = st.total_notional_traded;

    if (total_order_qty_submitted_ > 0) {
        report.fill_rate_qty_pct = (static_cast<double>(report.filled_quantity) / static_cast<double>(total_order_qty_submitted_)) * 100.0;
    }
    if (orders_submitted_ > 0) {
        report.fill_rate_orders_pct = (static_cast<double>(report.total_trades) / static_cast<double>(orders_submitted_)) * 100.0;
    }

    report.min_inventory = min_inventory_;
    report.max_inventory = max_inventory_;
    report.peak_absolute_inventory = peak_abs_inventory_;
    report.peak_equity = peak_equity_;
    report.max_drawdown = max_drawdown_;
    if (peak_equity_ > 0) {
        report.max_drawdown_pct = (static_cast<double>(max_drawdown_) / static_cast<double>(peak_equity_)) * 100.0;
    }

    int64_t final_sys_cash = ledger_.get_total_cash();
    Quantity final_sys_inv = ledger_.get_total_inventory();
    report.initial_system_cash = init_system_cash_;
    report.final_system_cash = final_sys_cash;
    report.initial_system_inventory = init_system_inv_;
    report.final_system_inventory = final_sys_inv;
    report.cash_conserved = (init_system_cash_ == final_sys_cash);
    report.inventory_conserved = (init_system_inv_ == final_sys_inv);

    // Compute post-fill markouts
    compute_markouts(session_end, report);
    report.trades = recorded_trades_;

    return report;
}

void BacktestEngine::compute_markouts(SimTime session_end_time, BacktestReport& report) {
    for (auto& tr : recorded_trades_) {
        tr.markouts.clear();
        for (SimTime h : config_.markout_horizons) {
            MarkoutPoint pt;
            pt.horizon_ns = h;
            pt.target_time = tr.execution_time + h;

            if (pt.target_time > session_end_time) {
                pt.status = MarkoutStatus::Unfinished;
            } else {
                auto it = std::upper_bound(mid_history_.begin(), mid_history_.end(), pt.target_time,
                                           [](SimTime t, const auto& pair) { return t < pair.first; });
                if (it != mid_history_.begin()) {
                    --it;
                    if (it->second.has_value()) {
                        pt.status = MarkoutStatus::Completed;
                        pt.mark_price = *(it->second);
                        pt.markout_ticks = (tr.side == Side::Buy) ? (*pt.mark_price - tr.price)
                                                                  : (tr.price - *pt.mark_price);
                        if (tr.price > 0) {
                            pt.markout_bps = (static_cast<double>(*pt.markout_ticks) / static_cast<double>(tr.price)) * 10000.0;
                        } else {
                            pt.markout_bps = 0.0;
                        }
                    } else {
                        pt.status = MarkoutStatus::MissingMark;
                    }
                } else {
                    pt.status = MarkoutStatus::MissingMark;
                }
            }
            tr.markouts.push_back(pt);
        }
    }

    // Summaries per horizon
    report.markout_summaries.clear();
    for (size_t hi = 0; hi < config_.markout_horizons.size(); ++hi) {
        SimTime h = config_.markout_horizons[hi];
        HorizonMarkoutSummary hs;
        hs.horizon_ns = h;

        // Label
        if (h >= 1000000000ULL && (h % 1000000000ULL == 0)) {
            hs.horizon_label = std::to_string(h / 1000000000ULL) + "s";
        } else if (h >= 1000000ULL && (h % 1000000ULL == 0)) {
            hs.horizon_label = std::to_string(h / 1000000ULL) + "ms";
        } else {
            hs.horizon_label = std::to_string(h) + "ns";
        }

        hs.total_trades = recorded_trades_.size();
        double sum_ticks = 0.0;
        double sum_bps = 0.0;
        double sum_vw_ticks = 0.0;
        double sum_vw_bps = 0.0;
        Quantity total_completed_qty = 0;

        for (const auto& tr : recorded_trades_) {
            if (hi >= tr.markouts.size()) continue;
            const auto& pt = tr.markouts[hi];
            if (pt.status == MarkoutStatus::Completed) {
                ++hs.completed_count;
                if (pt.markout_ticks) {
                    sum_ticks += static_cast<double>(*pt.markout_ticks);
                    sum_vw_ticks += static_cast<double>(*pt.markout_ticks) * static_cast<double>(tr.quantity);
                }
                if (pt.markout_bps) {
                    sum_bps += *pt.markout_bps;
                    sum_vw_bps += (*pt.markout_bps) * static_cast<double>(tr.quantity);
                }
                total_completed_qty += tr.quantity;
            } else if (pt.status == MarkoutStatus::Unfinished) {
                ++hs.unfinished_count;
            } else if (pt.status == MarkoutStatus::MissingMark) {
                ++hs.missing_mark_count;
            }
        }

        if (hs.completed_count > 0) {
            hs.mean_markout_ticks = sum_ticks / static_cast<double>(hs.completed_count);
            hs.mean_markout_bps = sum_bps / static_cast<double>(hs.completed_count);
        }
        if (total_completed_qty > 0) {
            hs.volume_weighted_markout_ticks = sum_vw_ticks / static_cast<double>(total_completed_qty);
            hs.volume_weighted_markout_bps = sum_vw_bps / static_cast<double>(total_completed_qty);
        }

        report.markout_summaries.push_back(hs);
    }
}

void BacktestEngine::schedule_observation_delivery(SimTime time,
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

void BacktestEngine::submit_order(Side side, OrderType type, Price price, Quantity qty, TimeInForce tif) {
    OrderId id = ++next_order_id_;
    SeqNum seq = ++next_cmd_seq_;
    NewOrderCommand cmd{seq, id, side, type, price, qty, tif};

    ++orders_submitted_;
    total_order_qty_submitted_ += qty;

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

    scheduler_.schedule(arrival_time, EventPriority::OrderAdmission, [this, cmd, protection_price](SimTime now) {
        std::vector<ExecutionReport> reports;
        std::vector<TradeSettlement> settlements;
        this->execution_adapter_.submit_order(now, this->config_.strategy_account_id, cmd,
                                              protection_price, reports, settlements);

        // Record aggressive fills
        for (const auto& st : settlements) {
            if (st.taker_account_id == this->config_.strategy_account_id) {
                this->record_trade(now, st.taker_order_id, st.taker_side, st.price, st.quantity, st.taker_fee, true);
            }
        }

        // Deliver reports
        for (const auto& rpt : reports) {
            auto rep_del_opt = safe_add_time(now, this->config_.latencies.execution_report_delay);
            SimTime rep_time = rep_del_opt ? *rep_del_opt : now;
            this->scheduler_.schedule(rep_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                if (this->strategy_) this->strategy_->on_execution_report(t, rpt, *this);
            });

            std::ostringstream ss;
            ss << "{\"event\":\"REPORT\",\"time\":" << now
               << ",\"type\":\"" << ((rpt.type == ExecutionReport::Type::Trade) ? "TRADE" :
                                    (rpt.type == ExecutionReport::Type::Accepted ? "ACCEPTED" :
                                    (rpt.type == ExecutionReport::Type::Canceled ? "CANCELED" : "REJECTED")))
               << "\",\"id\":" << rpt.order_id
               << ",\"side\":\"" << ::marketlab::to_string(rpt.side)
               << "\",\"price\":" << rpt.price
               << ",\"fill_qty\":" << (rpt.quantity - rpt.remaining_quantity)
               << ",\"rem_qty\":" << rpt.remaining_quantity;
            if (!rpt.reason.empty()) {
                ss << ",\"reason\":\"" << rpt.reason << "\"";
            }
            ss << "}";
            this->fill_log_lines_.push_back(ss.str());
        }
    });
}

void BacktestEngine::cancel_order(OrderId order_id) {
    ++next_cmd_seq_;

    auto delay_opt = safe_add_time(config_.latencies.strategy_delay, config_.latencies.order_entry_delay);
    if (!delay_opt) return;
    auto arr_opt = safe_add_time(scheduler_.now(), *delay_opt);
    if (!arr_opt) return;
    SimTime arrival_time = *arr_opt;

    scheduler_.schedule(arrival_time, EventPriority::OrderAdmission, [this, order_id](SimTime now) {
        std::vector<ExecutionReport> reports;
        this->execution_adapter_.cancel_order(now, this->config_.strategy_account_id, order_id, reports);

        for (const auto& rpt : reports) {
            auto rep_del_opt = safe_add_time(now, this->config_.latencies.execution_report_delay);
            SimTime rep_time = rep_del_opt ? *rep_del_opt : now;
            this->scheduler_.schedule(rep_time, EventPriority::ExecutionReportDelivery, [this, rpt](SimTime t) {
                if (this->strategy_) this->strategy_->on_execution_report(t, rpt, *this);
            });

            std::ostringstream ss;
            ss << "{\"event\":\"REPORT\",\"time\":" << now
               << ",\"type\":\"" << ((rpt.type == ExecutionReport::Type::Trade) ? "TRADE" : "CANCELED")
               << "\",\"id\":" << rpt.order_id
               << ",\"side\":\"" << ::marketlab::to_string(rpt.side)
               << "\",\"price\":" << rpt.price
               << ",\"fill_qty\":" << (rpt.quantity - rpt.remaining_quantity)
               << ",\"rem_qty\":" << rpt.remaining_quantity << "}";
            this->fill_log_lines_.push_back(ss.str());
        }
    });
}

std::string BacktestEngine::get_events_content() const {
    std::ostringstream ss;
    for (const auto& line : event_log_lines_) {
        ss << line << "\n";
    }
    return ss.str();
}

std::string BacktestEngine::get_fills_content() const {
    std::ostringstream ss;
    for (const auto& line : fill_log_lines_) {
        ss << line << "\n";
    }
    return ss.str();
}

std::string BacktestEngine::get_state_content() const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"feed_status\": \"" << to_string(book_.status()) << "\",\n";
    ss << "  \"last_update_id\": " << book_.last_update_id() << ",\n";
    ss << "  \"best_bid\": " << (book_.best_bid() ? std::to_string(*book_.best_bid()) : "null") << ",\n";
    ss << "  \"best_ask\": " << (book_.best_ask() ? std::to_string(*book_.best_ask()) : "null") << ",\n";
    ss << "  \"mid_price\": " << (book_.mid_price() ? std::to_string(*book_.mid_price()) : "null") << ",\n";
    ss << "  \"strategy_account\": {\n";
    const auto* acc = ledger_.get_account(config_.strategy_account_id);
    if (acc) {
        ss << "    \"account_id\": \"" << config_.strategy_account_id << "\",\n";
        ss << "    \"cash\": " << acc->cash << ",\n";
        ss << "    \"inventory\": " << acc->inventory << ",\n";
        ss << "    \"reserved_cash\": " << acc->reserved_cash << ",\n";
        ss << "    \"reserved_inventory\": " << acc->reserved_inventory << ",\n";
        ss << "    \"realized_pnl\": " << acc->realized_pnl << "\n";
    }
    ss << "  }\n";
    ss << "}\n";
    return ss.str();
}

std::string BacktestEngine::get_manifest_content(const BacktestReport& report,
                                                 const std::string& events_hash,
                                                 const std::string& fills_hash,
                                                 const std::string& state_hash,
                                                 const std::string& report_hash) const {
    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"manifest_version\": \"1.0\",\n";
    ss << "  \"baseline_commit\": \"60261a3\",\n";
    ss << "  \"run_type\": \"BACKTEST\",\n";
    ss << "  \"instrument\": {\n";
    ss << "    \"symbol\": \"" << config_.instrument.symbol << "\",\n";
    ss << "    \"tick_size\": " << config_.instrument.tick_size << ",\n";
    ss << "    \"lot_size\": " << config_.instrument.lot_size << "\n";
    ss << "  },\n";
    ss << "  \"execution_model\": {\n";
    ss << "    \"queue_position_model\": \"" << ::marketlab::to_string(config_.execution_model.queue_model) << "\",\n";
    ss << "    \"queue_position_pct\": " << config_.execution_model.queue_position_pct << ",\n";
    ss << "    \"aggressive_impact_model\": \"" << ::marketlab::to_string(config_.execution_model.impact_model) << "\",\n";
    ss << "    \"fixed_slippage_ticks\": " << config_.execution_model.fixed_slippage_ticks << ",\n";
    ss << "    \"impact_ticks_per_lot\": " << config_.execution_model.impact_ticks_per_lot << "\n";
    ss << "  },\n";
    ss << "  \"strategy\": {\n";
    ss << "    \"mode\": \"" << ::marketlab::to_string(config_.strategy_config.mode) << "\",\n";
    ss << "    \"quote_size\": " << config_.strategy_config.quote_size << ",\n";
    ss << "    \"half_spread\": " << config_.strategy_config.half_spread << ",\n";
    ss << "    \"inventory_target\": " << config_.strategy_config.inventory_target << ",\n";
    ss << "    \"skew_per_lot\": " << config_.strategy_config.skew_per_lot << ",\n";
    ss << "    \"rebalance_threshold\": " << config_.strategy_config.rebalance_threshold << "\n";
    ss << "  },\n";
    ss << "  \"events_hash\": \"" << events_hash << "\",\n";
    ss << "  \"fills_hash\": \"" << fills_hash << "\",\n";
    ss << "  \"state_hash\": \"" << state_hash << "\",\n";
    ss << "  \"report_hash\": \"" << report_hash << "\",\n";
    ss << "  \"total_events\": " << report.total_events_processed << ",\n";
    ss << "  \"total_trades\": " << report.total_trades << ",\n";
    ss << "  \"net_pnl\": " << report.net_pnl << "\n";
    ss << "}\n";
    return ss.str();
}

} // namespace marketlab
