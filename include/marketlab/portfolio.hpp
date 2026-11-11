#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>
#include <cstdint>

#include "types.hpp"
#include "sim_types.hpp"
#include "checked_math.hpp"

namespace marketlab {

struct AccountPosition {
    AccountId id;
    int64_t cash{0};                  // Settled cash balance
    Quantity inventory{0};            // Settled base asset inventory (lots)
    int64_t reserved_cash{0};         // Cash reserved for open buy orders
    Quantity reserved_inventory{0};   // Inventory reserved for open sell orders
    int64_t total_fees_paid{0};       // Cumulative fees paid
    int64_t realized_pnl{0};          // Cumulative realized PnL
    double avg_cost_basis{0.0};       // Average cost per unit of inventory

    [[nodiscard]] int64_t available_cash() const noexcept {
        return cash - reserved_cash;
    }

    [[nodiscard]] Quantity available_inventory() const noexcept {
        return inventory - reserved_inventory;
    }

    [[nodiscard]] std::optional<int64_t> calculate_unrealized_pnl(std::optional<Price> mark_price) const noexcept {
        if (!mark_price || inventory == 0) return 0;
        double diff = static_cast<double>(*mark_price) - avg_cost_basis;
        return static_cast<int64_t>(diff * static_cast<double>(inventory));
    }
};

struct OrderReservation {
    OrderId order_id{0};
    AccountId account_id;
    Side side{Side::Buy};
    Price limit_or_protection_price{0};
    Quantity original_quantity{0};
    Quantity remaining_quantity{0};
    int64_t reserved_cash_amount{0};      // Includes notional + fee reservation
    Quantity reserved_inventory_amount{0};
};

struct TradeSettlement {
    uint64_t trade_id{0};
    SimTime execution_time{0};
    OrderId taker_order_id{0};
    OrderId maker_order_id{0};
    AccountId taker_account_id;
    AccountId maker_account_id;
    Side taker_side{Side::Buy};
    Price price{0};
    Quantity quantity{0};
    int64_t notional{0};
    int64_t taker_fee{0};
    int64_t maker_fee{0};
};

class Ledger {
public:
    explicit Ledger(FeeConfig fee_config = {}) : fee_config_(std::move(fee_config)) {
        // Ensure fee account exists
        get_or_create_account(fee_config_.fee_account_id);
    }

    // Account setup
    AccountPosition& get_or_create_account(const AccountId& id);
    [[nodiscard]] const AccountPosition* get_account(const AccountId& id) const noexcept;
    void fund_account(const AccountId& id, int64_t cash, Quantity inventory);

    // Reservation lifecycle
    bool reserve_for_order(const AccountId& account_id,
                           OrderId order_id,
                           Side side,
                           Price price,
                           Quantity quantity,
                           int64_t cash_to_reserve,
                           Quantity inventory_to_reserve);

    bool release_reservation_on_cancel(OrderId order_id, Quantity canceled_qty);
    bool release_reservation_on_reject(OrderId order_id);

    // Trade settlement (authoritative)
    // Returns true on successful settlement, false if duplicate or invalid
    bool settle_trade(const TradeSettlement& settlement);

    // Audit and conservation checks
    [[nodiscard]] bool verify_cash_conservation(int64_t expected_total_cash) const noexcept;
    [[nodiscard]] bool verify_inventory_conservation(Quantity expected_total_inv) const noexcept;
    [[nodiscard]] int64_t get_total_cash() const noexcept;
    [[nodiscard]] Quantity get_total_inventory() const noexcept;

    // Inspection
    [[nodiscard]] const std::unordered_map<AccountId, AccountPosition>& accounts() const noexcept { return accounts_; }
    [[nodiscard]] const std::unordered_map<OrderId, OrderReservation>& reservations() const noexcept { return reservations_; }
    [[nodiscard]] const FeeConfig& fee_config() const noexcept { return fee_config_; }

    [[nodiscard]] std::optional<AccountId> get_order_owner(OrderId order_id) const noexcept {
        auto it = reservations_.find(order_id);
        if (it != reservations_.end()) return it->second.account_id;
        auto hist = order_owners_history_.find(order_id);
        if (hist != order_owners_history_.end()) return hist->second;
        return std::nullopt;
    }

private:
    FeeConfig fee_config_;
    std::unordered_map<AccountId, AccountPosition> accounts_;
    std::unordered_map<OrderId, OrderReservation> reservations_;
    std::unordered_map<OrderId, AccountId> order_owners_history_;
    std::unordered_set<uint64_t> settled_trade_ids_; // For idempotence
};

} // namespace marketlab
