#include "marketlab/portfolio.hpp"
#include <algorithm>
#include <cmath>

namespace marketlab {

AccountPosition& Ledger::get_or_create_account(const AccountId& id) {
    auto it = accounts_.find(id);
    if (it == accounts_.end()) {
        AccountPosition pos;
        pos.id = id;
        auto [ins_it, _] = accounts_.emplace(id, std::move(pos));
        return ins_it->second;
    }
    return it->second;
}

const AccountPosition* Ledger::get_account(const AccountId& id) const noexcept {
    auto it = accounts_.find(id);
    if (it != accounts_.end()) return &it->second;
    return nullptr;
}

void Ledger::fund_account(const AccountId& id, int64_t cash, Quantity inventory) {
    auto& pos = get_or_create_account(id);
    pos.cash += cash;
    pos.inventory += inventory;
}

bool Ledger::reserve_for_order(const AccountId& account_id,
                               OrderId order_id,
                               Side side,
                               Price price,
                               Quantity quantity,
                               int64_t cash_to_reserve,
                               Quantity inventory_to_reserve) {
    auto& pos = get_or_create_account(account_id);

    if (side == Side::Buy) {
        if (pos.available_cash() < cash_to_reserve) {
            return false;
        }
        pos.reserved_cash += cash_to_reserve;
    } else {
        if (pos.available_inventory() < inventory_to_reserve) {
            return false;
        }
        pos.reserved_inventory += inventory_to_reserve;
    }

    OrderReservation res;
    res.order_id = order_id;
    res.account_id = account_id;
    res.side = side;
    res.limit_or_protection_price = price;
    res.original_quantity = quantity;
    res.remaining_quantity = quantity;
    res.reserved_cash_amount = cash_to_reserve;
    res.reserved_inventory_amount = inventory_to_reserve;

    reservations_[order_id] = res;
    order_owners_history_[order_id] = account_id;
    return true;
}

bool Ledger::release_reservation_on_cancel(OrderId order_id, Quantity canceled_qty) {
    auto res_it = reservations_.find(order_id);
    if (res_it == reservations_.end()) return false;

    auto& res = res_it->second;
    auto& pos = get_or_create_account(res.account_id);

    if (res.side == Side::Buy) {
        int64_t cash_to_release = 0;
        if (canceled_qty >= res.remaining_quantity) {
            cash_to_release = res.reserved_cash_amount;
            res.reserved_cash_amount = 0;
            res.remaining_quantity = 0;
        } else {
            // Proportional release
            cash_to_release = static_cast<int64_t>(
                (static_cast<double>(res.reserved_cash_amount) * canceled_qty) / res.remaining_quantity
            );
            res.reserved_cash_amount -= cash_to_release;
            res.remaining_quantity -= canceled_qty;
        }
        pos.reserved_cash = std::max<int64_t>(0, pos.reserved_cash - cash_to_release);
    } else {
        Quantity inv_to_release = std::min(canceled_qty, res.remaining_quantity);
        pos.reserved_inventory = std::max<Quantity>(0, pos.reserved_inventory - inv_to_release);
        res.reserved_inventory_amount -= inv_to_release;
        res.remaining_quantity -= inv_to_release;
    }

    if (res.remaining_quantity == 0) {
        reservations_.erase(res_it);
    }
    return true;
}

bool Ledger::release_reservation_on_reject(OrderId order_id) {
    auto res_it = reservations_.find(order_id);
    if (res_it == reservations_.end()) return false;
    return release_reservation_on_cancel(order_id, res_it->second.remaining_quantity);
}

bool Ledger::settle_trade(const TradeSettlement& settlement) {
    // 1. Idempotence check: each trade settlement applied exactly once
    if (settled_trade_ids_.find(settlement.trade_id) != settled_trade_ids_.end()) {
        return false; // Duplicate fill delivery rejected
    }

    // Determine buyer and seller
    AccountId buyer_id = (settlement.taker_side == Side::Buy) ? settlement.taker_account_id : settlement.maker_account_id;
    AccountId seller_id = (settlement.taker_side == Side::Sell) ? settlement.taker_account_id : settlement.maker_account_id;

    int64_t buyer_fee = (settlement.taker_side == Side::Buy) ? settlement.taker_fee : settlement.maker_fee;
    int64_t seller_fee = (settlement.taker_side == Side::Sell) ? settlement.taker_fee : settlement.maker_fee;

    auto& buyer = get_or_create_account(buyer_id);
    auto& seller = get_or_create_account(seller_id);
    auto& fee_acc = get_or_create_account(fee_config_.fee_account_id);

    // 2. Adjust reservations
    // Buyer reservation release
    OrderId buyer_order_id = (settlement.taker_side == Side::Buy) ? settlement.taker_order_id : settlement.maker_order_id;
    auto b_res_it = reservations_.find(buyer_order_id);
    if (b_res_it != reservations_.end()) {
        auto& res = b_res_it->second;
        int64_t reserved_cash_portion = 0;
        if (settlement.quantity >= res.remaining_quantity) {
            reserved_cash_portion = res.reserved_cash_amount;
            res.reserved_cash_amount = 0;
            res.remaining_quantity = 0;
        } else {
            reserved_cash_portion = static_cast<int64_t>(
                (static_cast<double>(res.reserved_cash_amount) * settlement.quantity) / res.remaining_quantity
            );
            res.reserved_cash_amount -= reserved_cash_portion;
            res.remaining_quantity -= settlement.quantity;
        }
        buyer.reserved_cash = std::max<int64_t>(0, buyer.reserved_cash - reserved_cash_portion);
        if (res.remaining_quantity == 0) {
            reservations_.erase(b_res_it);
        }
    }

    // Seller reservation release
    OrderId seller_order_id = (settlement.taker_side == Side::Sell) ? settlement.taker_order_id : settlement.maker_order_id;
    auto s_res_it = reservations_.find(seller_order_id);
    if (s_res_it != reservations_.end()) {
        auto& res = s_res_it->second;
        Quantity rel_qty = std::min(settlement.quantity, res.remaining_quantity);
        seller.reserved_inventory = std::max<Quantity>(0, seller.reserved_inventory - rel_qty);
        res.reserved_inventory_amount -= rel_qty;
        res.remaining_quantity -= rel_qty;
        if (res.remaining_quantity == 0) {
            reservations_.erase(s_res_it);
        }
    }

    // 3. Settle Buyer
    buyer.cash -= (settlement.notional + buyer_fee);
    buyer.total_fees_paid += buyer_fee;

    // Update Buyer average cost basis
    double prev_cost = buyer.avg_cost_basis * static_cast<double>(buyer.inventory);
    buyer.inventory += settlement.quantity;
    if (buyer.inventory > 0) {
        buyer.avg_cost_basis = (prev_cost + static_cast<double>(settlement.notional)) / static_cast<double>(buyer.inventory);
    }

    // 4. Settle Seller
    seller.cash += (settlement.notional - seller_fee);
    seller.total_fees_paid += seller_fee;

    // Calculate seller Realized PnL: (trade_price - avg_cost) * qty - fee
    double cost_of_sold = seller.avg_cost_basis * static_cast<double>(settlement.quantity);
    double gross_pnl = static_cast<double>(settlement.notional) - cost_of_sold;
    int64_t net_pnl = static_cast<int64_t>(gross_pnl) - seller_fee;
    seller.realized_pnl += net_pnl;

    seller.inventory -= settlement.quantity;
    if (seller.inventory == 0) {
        seller.avg_cost_basis = 0.0;
    }

    // 5. Settle Fee Account
    fee_acc.cash += (buyer_fee + seller_fee);

    // 6. Record settled trade
    settled_trade_ids_.insert(settlement.trade_id);
    return true;
}

bool Ledger::verify_cash_conservation(int64_t expected_total_cash) const noexcept {
    return get_total_cash() == expected_total_cash;
}

bool Ledger::verify_inventory_conservation(Quantity expected_total_inv) const noexcept {
    return get_total_inventory() == expected_total_inv;
}

int64_t Ledger::get_total_cash() const noexcept {
    int64_t total = 0;
    for (const auto& [_, pos] : accounts_) {
        total += pos.cash;
    }
    return total;
}

Quantity Ledger::get_total_inventory() const noexcept {
    Quantity total = 0;
    for (const auto& [_, pos] : accounts_) {
        total += pos.inventory;
    }
    return total;
}

} // namespace marketlab
