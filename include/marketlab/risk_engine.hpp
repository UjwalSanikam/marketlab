#pragma once

#include <optional>
#include <string>

#include "types.hpp"
#include "commands.hpp"
#include "sim_types.hpp"
#include "portfolio.hpp"
#include "order_book.hpp"

namespace marketlab {

struct RiskLimits {
    Quantity max_order_qty{1000000};
    int64_t max_order_notional{1000000000};
    Quantity max_inventory{5000000};
    SimTime max_data_age_ns{10000000000ULL}; // 10 seconds default
    bool allow_self_trade{false};
};

struct OrderAdmissionResult {
    bool approved{false};
    RiskRejectReason reject_reason{RiskRejectReason::None};
    int64_t reserved_cash{0};
    Quantity reserved_inventory{0};
    Price effective_price{0};
};

class RiskEngine {
public:
    explicit RiskEngine(RiskLimits limits = {}) : limits_(limits) {}

    // Evaluate risk and atomically reserve resources if approved
    [[nodiscard]] OrderAdmissionResult evaluate_and_reserve(
        const AccountId& account_id,
        const NewOrderCommand& cmd,
        Price market_buy_protection_price,
        SimTime current_time,
        SimTime last_market_data_time,
        const OrderBook& book,
        Ledger& ledger
    ) {
        OrderAdmissionResult result;

        // 1. Check max order quantity
        if (cmd.quantity <= 0 || cmd.quantity > limits_.max_order_qty) {
            result.reject_reason = RiskRejectReason::OrderQuantityExceedsMax;
            return result;
        }

        // 2. Determine effective limit / protection price
        Price effective_price = cmd.price;
        if (cmd.type == OrderType::Market) {
            if (cmd.side == Side::Buy) {
                if (market_buy_protection_price <= 0) {
                    result.reject_reason = RiskRejectReason::MarketBuyProtectionPriceRequired;
                    return result;
                }
                effective_price = market_buy_protection_price;
            } else {
                // Market sell matches resting bids; price for notional bounds check can use current best bid
                auto bb = book.best_bid();
                effective_price = bb ? *bb : 1;
            }
        }
        result.effective_price = effective_price;

        // 3. Check order notional cap
        auto notional_opt = CheckedMath::safe_mul_notional(effective_price, cmd.quantity);
        if (!notional_opt || *notional_opt > limits_.max_order_notional) {
            result.reject_reason = RiskRejectReason::OrderNotionalExceedsMax;
            return result;
        }
        int64_t notional = *notional_opt;

        // 4. Market data freshness check
        if (last_market_data_time > 0 && current_time >= last_market_data_time) {
            SimTime age = current_time - last_market_data_time;
            if (age > limits_.max_data_age_ns) {
                result.reject_reason = RiskRejectReason::StaleMarketData;
                return result;
            }
        }

        // 5. Self-trade prevention check across the ENTIRE executable matching path
        if (!limits_.allow_self_trade) {
            Quantity matchable_so_far = 0;
            if (cmd.side == Side::Buy) {
                for (const auto& [lvl_price, level] : book.asks()) {
                    if (cmd.type == OrderType::Limit && lvl_price > cmd.price) break;
                    if (cmd.type == OrderType::Market && effective_price > 0 && lvl_price > effective_price) break;
                    for (const auto& maker : level.orders) {
                        auto owner = ledger.get_order_owner(maker.order_id);
                        if (owner && *owner == account_id) {
                            result.reject_reason = RiskRejectReason::SelfTradeProhibited;
                            return result;
                        }
                        matchable_so_far += maker.remaining_quantity;
                        if (matchable_so_far >= cmd.quantity) break;
                    }
                    if (matchable_so_far >= cmd.quantity) break;
                }
            } else {
                for (const auto& [lvl_price, level] : book.bids()) {
                    if (cmd.type == OrderType::Limit && lvl_price < cmd.price) break;
                    if (cmd.type == OrderType::Market && effective_price > 0 && lvl_price < effective_price) break;
                    for (const auto& maker : level.orders) {
                        auto owner = ledger.get_order_owner(maker.order_id);
                        if (owner && *owner == account_id) {
                            result.reject_reason = RiskRejectReason::SelfTradeProhibited;
                            return result;
                        }
                        matchable_so_far += maker.remaining_quantity;
                        if (matchable_so_far >= cmd.quantity) break;
                    }
                    if (matchable_so_far >= cmd.quantity) break;
                }
            }
        }

        // 6. Account position and reservation checks
        const auto* pos_ptr = ledger.get_account(account_id);
        if (!pos_ptr) {
            result.reject_reason = RiskRejectReason::InsufficientAvailableCash;
            return result;
        }
        const auto& pos = *pos_ptr;

        int64_t cash_needed = 0;
        Quantity inv_needed = 0;

        if (cmd.side == Side::Buy) {
            // Projected inventory check includes settled inventory + all pending buy orders + current order
            auto pending_plus_curr = CheckedMath::safe_add_quantity(pos.pending_buy_quantity, cmd.quantity);
            if (!pending_plus_curr) {
                result.reject_reason = RiskRejectReason::MaxInventoryExceeded;
                return result;
            }
            auto proj_inv = CheckedMath::safe_add_quantity(pos.inventory, *pending_plus_curr);
            if (!proj_inv || *proj_inv > limits_.max_inventory) {
                result.reject_reason = RiskRejectReason::MaxInventoryExceeded;
                return result;
            }

            // Cash reservation: conservative notional + max applicable maker/taker fee
            int64_t fee_res = ledger.fee_config().conservative_fee_reservation(notional);
            auto total_cash_needed = CheckedMath::safe_add_quantity(notional, fee_res);
            if (!total_cash_needed) {
                result.reject_reason = RiskRejectReason::OrderNotionalExceedsMax;
                return result;
            }
            cash_needed = *total_cash_needed;

            if (pos.available_cash() < cash_needed) {
                result.reject_reason = RiskRejectReason::InsufficientAvailableCash;
                return result;
            }
        } else { // Side::Sell
            // No short selling: available inventory must cover order quantity
            inv_needed = cmd.quantity;
            if (pos.available_inventory() < inv_needed) {
                result.reject_reason = RiskRejectReason::ShortSellingProhibited;
                return result;
            }
        }

        // 7. Atomic reservation in ledger
        bool ok = ledger.reserve_for_order(account_id, cmd.order_id, cmd.side, effective_price, cmd.quantity, cash_needed, inv_needed);
        if (!ok) {
            result.reject_reason = (cmd.side == Side::Buy)
                ? RiskRejectReason::InsufficientAvailableCash
                : RiskRejectReason::ShortSellingProhibited;
            return result;
        }

        result.approved = true;
        result.reserved_cash = cash_needed;
        result.reserved_inventory = inv_needed;
        return result;
    }

    // Verify account ownership before canceling
    [[nodiscard]] bool can_cancel_order(const AccountId& requester, OrderId order_id, const Ledger& ledger) const noexcept {
        auto owner = ledger.get_order_owner(order_id);
        if (!owner) return false;
        return (*owner == requester);
    }

    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }

private:
    RiskLimits limits_;
};

} // namespace marketlab
