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

        // 5. Self-trade prevention check against opposing top-of-book
        if (!limits_.allow_self_trade) {
            if (cmd.side == Side::Buy) {
                if (!book.asks().empty()) {
                    const auto& top_level = book.asks().begin()->second;
                    if (!top_level.orders.empty()) {
                        OrderId top_maker_id = top_level.orders.front().order_id;
                        auto owner = ledger.get_order_owner(top_maker_id);
                        if (owner && *owner == account_id && (cmd.type == OrderType::Market || cmd.price >= top_level.price)) {
                            result.reject_reason = RiskRejectReason::SelfTradeProhibited;
                            return result;
                        }
                    }
                }
            } else {
                if (!book.bids().empty()) {
                    const auto& top_level = book.bids().begin()->second;
                    if (!top_level.orders.empty()) {
                        OrderId top_maker_id = top_level.orders.front().order_id;
                        auto owner = ledger.get_order_owner(top_maker_id);
                        if (owner && *owner == account_id && (cmd.type == OrderType::Market || cmd.price <= top_level.price)) {
                            result.reject_reason = RiskRejectReason::SelfTradeProhibited;
                            return result;
                        }
                    }
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
            // Projected inventory check
            auto proj_inv = CheckedMath::safe_add_quantity(pos.inventory, cmd.quantity);
            if (!proj_inv || *proj_inv > limits_.max_inventory) {
                result.reject_reason = RiskRejectReason::MaxInventoryExceeded;
                return result;
            }

            // Cash reservation: conservative notional + max taker fee
            int64_t fee_res = ledger.fee_config().conservative_taker_fee_reservation(notional);
            cash_needed = notional + fee_res;

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
