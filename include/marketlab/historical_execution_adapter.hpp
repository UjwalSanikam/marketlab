#pragma once

#include <vector>
#include <map>
#include <unordered_map>
#include <string>
#include <optional>
#include <algorithm>
#include <cstdint>

#include "types.hpp"
#include "commands.hpp"
#include "events.hpp"
#include "sim_types.hpp"
#include "portfolio.hpp"
#include "risk_engine.hpp"
#include "strategy.hpp"
#include "market_data_types.hpp"
#include "reconstructed_book.hpp"
#include "execution_model.hpp"

namespace marketlab {

struct PassiveOrder {
    OrderId order_id{0};
    AccountId account_id;
    Side side{Side::Buy};
    Price price{0};
    Quantity original_qty{0};
    Quantity remaining_qty{0};
    Quantity queue_ahead{0};
    SimTime arrival_time{0};
};

struct ExecutionStats {
    size_t orders_submitted{0};
    size_t orders_accepted{0};
    size_t orders_rejected{0};
    size_t orders_canceled{0};
    size_t aggressive_fills{0};
    size_t passive_fills{0};
    size_t total_trades{0};
    Quantity total_volume_traded{0};
    int64_t total_notional_traded{0};
    int64_t total_fees_paid{0};
};

class HistoricalExecutionAdapter {
public:
    static constexpr const char* EXTERNAL_MARKET_ACCOUNT = "HISTORICAL_MARKET";

    HistoricalExecutionAdapter(ReconstructedOrderBook& book,
                               Ledger& ledger,
                               RiskEngine& risk_engine,
                               FeeConfig fee_config,
                               ExecutionModelConfig exec_model = {})
        : book_(book),
          ledger_(ledger),
          risk_engine_(risk_engine),
          fee_config_(std::move(fee_config)),
          exec_model_(std::move(exec_model)) {
        // Fund external counterparty account with ample initial balances for double-entry settlement
        ledger_.fund_account(EXTERNAL_MARKET_ACCOUNT, 100000000000LL, 1000000000LL);
    }

    [[nodiscard]] const ExecutionStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const ExecutionModelConfig& execution_model() const noexcept { return exec_model_; }
    [[nodiscard]] const std::unordered_map<OrderId, PassiveOrder>& active_passive_orders() const noexcept {
        return active_orders_;
    }

    // Called when a depth update modifies price levels: resets hypothetical liquidity consumption at those levels
    void on_depth_update(const NormalizedDepthUpdate& update) {
        for (const auto& lvl : update.bids) {
            consumed_bid_liquidity_.erase(lvl.price);
        }
        for (const auto& lvl : update.asks) {
            consumed_ask_liquidity_.erase(lvl.price);
        }
    }

    // Called on a snapshot: clear all hypothetical liquidity consumption
    void on_snapshot() {
        consumed_bid_liquidity_.clear();
        consumed_ask_liquidity_.clear();
    }

    // Process a public market trade and match against resting passive strategy orders
    void on_public_trade(const NormalizedTrade& trade,
                         std::vector<ExecutionReport>& out_reports,
                         std::vector<TradeSettlement>& out_settlements) {
        if (!book_.is_valid()) return;
        if (trade.quantity <= 0) return;

        Quantity remaining_trade_qty = trade.quantity;

        if (trade.taker_side == Side::Sell) {
            // Taker sold into bids: eligible to fill resting passive BUY orders at price >= trade.price
            std::vector<PassiveOrder*> candidates;
            for (auto& [id, order] : active_orders_) {
                if (order.side == Side::Buy && order.price >= trade.price && order.remaining_qty > 0) {
                    candidates.push_back(&order);
                }
            }

            // Price-time priority: higher price first, then earlier arrival time
            std::sort(candidates.begin(), candidates.end(), [](const PassiveOrder* a, const PassiveOrder* b) {
                if (a->price != b->price) return a->price > b->price;
                return a->arrival_time < b->arrival_time;
            });

            for (auto* order_ptr : candidates) {
                if (remaining_trade_qty <= 0) break;
                auto& order = *order_ptr;

                // If trade price is strictly lower than resting bid price, all queue ahead was crossed
                if (trade.price < order.price) {
                    order.queue_ahead = 0;
                } else if (order.queue_ahead > 0) {
                    Quantity deplete = std::min(remaining_trade_qty, order.queue_ahead);
                    order.queue_ahead -= deplete;
                    remaining_trade_qty -= deplete;
                }

                // If queue ahead depleted, fill from remaining public trade quantity
                if (order.queue_ahead == 0 && remaining_trade_qty > 0) {
                    Quantity fill_qty = std::min(remaining_trade_qty, order.remaining_qty);
                    execute_passive_fill(trade.local_receipt_time_ns, order, fill_qty, order.price,
                                         out_reports, out_settlements);
                    remaining_trade_qty -= fill_qty;
                }
            }

        } else { // trade.taker_side == Side::Buy
            // Taker bought from asks: eligible to fill resting passive SELL orders at price <= trade.price
            std::vector<PassiveOrder*> candidates;
            for (auto& [id, order] : active_orders_) {
                if (order.side == Side::Sell && order.price <= trade.price && order.remaining_qty > 0) {
                    candidates.push_back(&order);
                }
            }

            // Price-time priority: lower price first, then earlier arrival time
            std::sort(candidates.begin(), candidates.end(), [](const PassiveOrder* a, const PassiveOrder* b) {
                if (a->price != b->price) return a->price < b->price;
                return a->arrival_time < b->arrival_time;
            });

            for (auto* order_ptr : candidates) {
                if (remaining_trade_qty <= 0) break;
                auto& order = *order_ptr;

                if (trade.price > order.price) {
                    order.queue_ahead = 0;
                } else if (order.queue_ahead > 0) {
                    Quantity deplete = std::min(remaining_trade_qty, order.queue_ahead);
                    order.queue_ahead -= deplete;
                    remaining_trade_qty -= deplete;
                }

                if (order.queue_ahead == 0 && remaining_trade_qty > 0) {
                    Quantity fill_qty = std::min(remaining_trade_qty, order.remaining_qty);
                    execute_passive_fill(trade.local_receipt_time_ns, order, fill_qty, order.price,
                                         out_reports, out_settlements);
                    remaining_trade_qty -= fill_qty;
                }
            }
        }

        // Clean up fully filled passive orders
        for (auto it = active_orders_.begin(); it != active_orders_.end();) {
            if (it->second.remaining_qty == 0) {
                it = active_orders_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Submit strategy order at admission time
    bool submit_order(SimTime now,
                      const AccountId& account_id,
                      const NewOrderCommand& cmd,
                      Price market_buy_protection_price,
                      std::vector<ExecutionReport>& out_reports,
                      std::vector<TradeSettlement>& out_settlements) {
        ++stats_.orders_submitted;

        // 1. Data freshness & book validity check
        if (!book_.is_valid()) {
            ++stats_.orders_rejected;
            out_reports.push_back(ExecutionReport{
                ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity,
                "INVALID_FEED_STATE: " + (book_.invalid_reason().empty() ? "Unsynchronized" : book_.invalid_reason())
            });
            return false;
        }

        // 2. Determine effective price and protection
        Price effective_price = cmd.price;
        if (cmd.type == OrderType::Market) {
            if (cmd.side == Side::Buy) {
                if (market_buy_protection_price <= 0) {
                    ++stats_.orders_rejected;
                    out_reports.push_back(ExecutionReport{
                        ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity,
                        "MARKET_BUY_PROTECTION_PRICE_REQUIRED"
                    });
                    return false;
                }
                effective_price = market_buy_protection_price;
            } else {
                auto bb = book_.best_bid();
                effective_price = bb ? *bb : 1;
            }
        }

        // 3. Self-trade prevention against own resting passive orders
        for (const auto& [id, passive] : active_orders_) {
            if (passive.account_id == account_id) {
                if (cmd.side == Side::Buy && passive.side == Side::Sell) {
                    if (cmd.type == OrderType::Market || cmd.price >= passive.price) {
                        ++stats_.orders_rejected;
                        out_reports.push_back(ExecutionReport{
                            ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity,
                            "SELF_TRADE_PROHIBITED"
                        });
                        return false;
                    }
                } else if (cmd.side == Side::Sell && passive.side == Side::Buy) {
                    if (cmd.type == OrderType::Market || cmd.price <= passive.price) {
                        ++stats_.orders_rejected;
                        out_reports.push_back(ExecutionReport{
                            ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity,
                            "SELF_TRADE_PROHIBITED"
                        });
                        return false;
                    }
                }
            }
        }

        // 4. Pre-trade Risk Check & Atomic Reservation in Ledger
        const auto* pos_ptr = ledger_.get_account(account_id);
        if (!pos_ptr) {
            ++stats_.orders_rejected;
            out_reports.push_back(ExecutionReport{
                ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity,
                "INSUFFICIENT_AVAILABLE_CASH"
            });
            return false;
        }
        const auto& pos = *pos_ptr;

        // Order size and notional checks
        if (cmd.quantity <= 0 || cmd.quantity > risk_engine_.limits().max_order_qty) {
            ++stats_.orders_rejected;
            out_reports.push_back(ExecutionReport{
                ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity,
                "ORDER_QUANTITY_EXCEEDS_MAX"
            });
            return false;
        }

        auto notional_opt = CheckedMath::safe_mul_notional(effective_price, cmd.quantity);
        if (!notional_opt || *notional_opt > risk_engine_.limits().max_order_notional) {
            ++stats_.orders_rejected;
            out_reports.push_back(ExecutionReport{
                ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity,
                "ORDER_NOTIONAL_EXCEEDS_MAX"
            });
            return false;
        }
        int64_t max_notional = *notional_opt;

        // Position limits and balance reservations
        int64_t cash_needed = 0;
        Quantity inv_needed = 0;
        if (cmd.side == Side::Buy) {
            auto pending_plus_curr = CheckedMath::safe_add_quantity(pos.pending_buy_quantity, cmd.quantity);
            if (!pending_plus_curr) {
                ++stats_.orders_rejected;
                out_reports.push_back(ExecutionReport{ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity, "MAX_INVENTORY_EXCEEDED"});
                return false;
            }
            auto proj_inv = CheckedMath::safe_add_quantity(pos.inventory, *pending_plus_curr);
            if (!proj_inv || *proj_inv > risk_engine_.limits().max_inventory) {
                ++stats_.orders_rejected;
                out_reports.push_back(ExecutionReport{ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity, "MAX_INVENTORY_EXCEEDED"});
                return false;
            }

            int64_t fee_res = fee_config_.conservative_fee_reservation(max_notional);
            auto total_cash = CheckedMath::safe_add_quantity(max_notional, fee_res);
            if (!total_cash || pos.available_cash() < *total_cash) {
                ++stats_.orders_rejected;
                out_reports.push_back(ExecutionReport{ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity, "INSUFFICIENT_AVAILABLE_CASH"});
                return false;
            }
            cash_needed = *total_cash;
        } else {
            if (pos.available_inventory() < cmd.quantity) {
                ++stats_.orders_rejected;
                out_reports.push_back(ExecutionReport{ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity, "SHORT_SELLING_PROHIBITED"});
                return false;
            }
            inv_needed = cmd.quantity;
        }

        // Atomically reserve in authoritative Ledger
        if (!ledger_.reserve_for_order(account_id, cmd.order_id, cmd.side, effective_price, cmd.quantity, cash_needed, inv_needed)) {
            ++stats_.orders_rejected;
            out_reports.push_back(ExecutionReport{ExecutionReport::Type::Rejected, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity, "RESERVATION_FAILED"});
            return false;
        }

        ++stats_.orders_accepted;
        out_reports.push_back(ExecutionReport{
            ExecutionReport::Type::Accepted, now, cmd.order_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity, ""
        });

        // 5. Determine whether order is Aggressive or Passive
        bool is_aggressive = false;
        if (cmd.type == OrderType::Market) {
            is_aggressive = true;
        } else if (cmd.side == Side::Buy && book_.best_ask().has_value() && cmd.price >= *book_.best_ask()) {
            is_aggressive = true;
        } else if (cmd.side == Side::Sell && book_.best_bid().has_value() && cmd.price <= *book_.best_bid()) {
            is_aggressive = true;
        }

        if (is_aggressive) {
            execute_aggressive_order(now, account_id, cmd, effective_price, out_reports, out_settlements);
        } else {
            // Passive resting order
            Quantity visible_depth = 0;
            if (cmd.side == Side::Buy) {
                Quantity book_q = book_.get_bid_qty(cmd.price);
                Quantity consumed = 0;
                auto it = consumed_bid_liquidity_.find(cmd.price);
                if (it != consumed_bid_liquidity_.end()) consumed = it->second;
                visible_depth = (book_q > consumed) ? (book_q - consumed) : 0;
            } else {
                Quantity book_q = book_.get_ask_qty(cmd.price);
                Quantity consumed = 0;
                auto it = consumed_ask_liquidity_.find(cmd.price);
                if (it != consumed_ask_liquidity_.end()) consumed = it->second;
                visible_depth = (book_q > consumed) ? (book_q - consumed) : 0;
            }

            Quantity q_ahead = exec_model_.calculate_initial_queue_ahead(visible_depth);

            active_orders_[cmd.order_id] = PassiveOrder{
                cmd.order_id, account_id, cmd.side, cmd.price, cmd.quantity, cmd.quantity, q_ahead, now
            };
        }

        return true;
    }

    // Cancel order
    bool cancel_order(SimTime now,
                      const AccountId& account_id,
                      OrderId order_id,
                      std::vector<ExecutionReport>& out_reports) {
        auto it = active_orders_.find(order_id);
        if (it == active_orders_.end()) {
            // Order might have already filled or does not exist
            return false;
        }

        if (it->second.account_id != account_id) {
            return false; // Unauthorized
        }

        Quantity canceled_qty = it->second.remaining_qty;
        Side side = it->second.side;
        Price price = it->second.price;
        Quantity orig_qty = it->second.original_qty;
        active_orders_.erase(it);

        // Release reservation in ledger
        ledger_.release_reservation_on_cancel(order_id, canceled_qty);

        ++stats_.orders_canceled;
        out_reports.push_back(ExecutionReport{
            ExecutionReport::Type::Canceled, now, order_id, side, price,
            orig_qty, 0, "USER_CANCELED"
        });
        return true;
    }

private:
    void execute_aggressive_order(SimTime now,
                                  const AccountId& account_id,
                                  const NewOrderCommand& cmd,
                                  Price protection_price,
                                  std::vector<ExecutionReport>& out_reports,
                                  std::vector<TradeSettlement>& out_settlements) {
        Quantity remaining_qty = cmd.quantity;

        if (cmd.side == Side::Buy) {
            for (const auto& [lvl_price, total_qty] : book_.asks()) {
                if (remaining_qty <= 0) break;
                if (cmd.type == OrderType::Limit && lvl_price > cmd.price) break;
                if (lvl_price > protection_price) break; // Market buy protection cap

                Quantity consumed = 0;
                auto cit = consumed_ask_liquidity_.find(lvl_price);
                if (cit != consumed_ask_liquidity_.end()) consumed = cit->second;

                if (consumed >= total_qty) continue; // Level already consumed
                Quantity available = total_qty - consumed;
                Quantity trade_qty = std::min(remaining_qty, available);

                Price exec_price = exec_model_.apply_aggressive_impact(lvl_price, Side::Buy, trade_qty);
                if (cmd.type == OrderType::Limit && exec_price > cmd.price) break; // Exceeds limit price
                if (exec_price > protection_price) break; // Market buy protection cap

                // Consume liquidity
                consumed_ask_liquidity_[lvl_price] += trade_qty;
                remaining_qty -= trade_qty;

                // Execute trade against external counterparty
                int64_t notional = exec_price * trade_qty;
                int64_t taker_fee = fee_config_.calculate_taker_fee(notional);
                int64_t maker_fee = fee_config_.calculate_maker_fee(notional);

                uint64_t tid = ++next_trade_id_;
                TradeSettlement settlement{
                    tid, now, cmd.order_id, 0,
                    account_id, EXTERNAL_MARKET_ACCOUNT, Side::Buy,
                    exec_price, trade_qty, notional, taker_fee, maker_fee
                };
                ledger_.settle_trade(settlement);
                out_settlements.push_back(settlement);

                ++stats_.aggressive_fills;
                ++stats_.total_trades;
                stats_.total_volume_traded += trade_qty;
                stats_.total_notional_traded += notional;
                stats_.total_fees_paid += taker_fee;

                out_reports.push_back(ExecutionReport{
                    ExecutionReport::Type::Trade, now, cmd.order_id, Side::Buy, exec_price,
                    cmd.quantity, remaining_qty, ""
                });
            }

            // Unfilled remainder handling
            if (remaining_qty > 0) {
                // Release reservation for unfilled portion
                ledger_.release_reservation_on_cancel(cmd.order_id, remaining_qty);
                out_reports.push_back(ExecutionReport{
                    ExecutionReport::Type::Canceled, now, cmd.order_id, Side::Buy, cmd.price,
                    cmd.quantity, 0, (cmd.type == OrderType::Market) ? "MARKET_REMAINDER" : "IOC_REMAINDER"
                });
            }

        } else { // Side::Sell
            for (const auto& [lvl_price, total_qty] : book_.bids()) {
                if (remaining_qty <= 0) break;
                if (cmd.type == OrderType::Limit && lvl_price < cmd.price) break;

                Quantity consumed = 0;
                auto cit = consumed_bid_liquidity_.find(lvl_price);
                if (cit != consumed_bid_liquidity_.end()) consumed = cit->second;

                if (consumed >= total_qty) continue;
                Quantity available = total_qty - consumed;
                Quantity trade_qty = std::min(remaining_qty, available);

                Price exec_price = exec_model_.apply_aggressive_impact(lvl_price, Side::Sell, trade_qty);
                if (cmd.type == OrderType::Limit && exec_price < cmd.price) break; // Below limit price

                consumed_bid_liquidity_[lvl_price] += trade_qty;
                remaining_qty -= trade_qty;

                int64_t notional = exec_price * trade_qty;
                int64_t taker_fee = fee_config_.calculate_taker_fee(notional);
                int64_t maker_fee = fee_config_.calculate_maker_fee(notional);

                uint64_t tid = ++next_trade_id_;
                TradeSettlement settlement{
                    tid, now, cmd.order_id, 0,
                    account_id, EXTERNAL_MARKET_ACCOUNT, Side::Sell,
                    exec_price, trade_qty, notional, taker_fee, maker_fee
                };
                ledger_.settle_trade(settlement);
                out_settlements.push_back(settlement);

                ++stats_.aggressive_fills;
                ++stats_.total_trades;
                stats_.total_volume_traded += trade_qty;
                stats_.total_notional_traded += notional;
                stats_.total_fees_paid += taker_fee;

                out_reports.push_back(ExecutionReport{
                    ExecutionReport::Type::Trade, now, cmd.order_id, Side::Sell, exec_price,
                    cmd.quantity, remaining_qty, ""
                });
            }

            if (remaining_qty > 0) {
                ledger_.release_reservation_on_cancel(cmd.order_id, remaining_qty);
                out_reports.push_back(ExecutionReport{
                    ExecutionReport::Type::Canceled, now, cmd.order_id, Side::Sell, cmd.price,
                    cmd.quantity, 0, (cmd.type == OrderType::Market) ? "MARKET_REMAINDER" : "IOC_REMAINDER"
                });
            }
        }
    }

    void execute_passive_fill(SimTime now,
                              PassiveOrder& order,
                              Quantity fill_qty,
                              Price trade_price,
                              std::vector<ExecutionReport>& out_reports,
                              std::vector<TradeSettlement>& out_settlements) {
        order.remaining_qty -= fill_qty;

        int64_t notional = trade_price * fill_qty;
        int64_t maker_fee = fee_config_.calculate_maker_fee(notional);
        int64_t taker_fee = fee_config_.calculate_taker_fee(notional);

        uint64_t tid = ++next_trade_id_;
        TradeSettlement settlement{
            tid, now, 0, order.order_id,
            EXTERNAL_MARKET_ACCOUNT, order.account_id,
            (order.side == Side::Buy ? Side::Sell : Side::Buy), // Counterparty taker side
            trade_price, fill_qty, notional, taker_fee, maker_fee
        };
        ledger_.settle_trade(settlement);
        out_settlements.push_back(settlement);

        ++stats_.passive_fills;
        ++stats_.total_trades;
        stats_.total_volume_traded += fill_qty;
        stats_.total_notional_traded += notional;
        stats_.total_fees_paid += maker_fee;

        out_reports.push_back(ExecutionReport{
            ExecutionReport::Type::Trade, now, order.order_id, order.side, trade_price,
            order.original_qty, order.remaining_qty, ""
        });
    }

    ReconstructedOrderBook& book_;
    Ledger& ledger_;
    RiskEngine& risk_engine_;
    FeeConfig fee_config_;
    ExecutionModelConfig exec_model_;
    ExecutionStats stats_;

    uint64_t next_trade_id_{50000};
    std::unordered_map<OrderId, PassiveOrder> active_orders_;
    std::unordered_map<Price, Quantity> consumed_ask_liquidity_;
    std::unordered_map<Price, Quantity> consumed_bid_liquidity_;
};

} // namespace marketlab
