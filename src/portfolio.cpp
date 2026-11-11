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

void Ledger::fund_account(const AccountId& id, int64_t cash, Quantity inventory, int64_t initial_cost_per_lot) {
    auto& pos = get_or_create_account(id);
    pos.cash += cash;
    pos.inventory += inventory;
    if (inventory > 0 && initial_cost_per_lot > 0) {
        pos.total_cost += (inventory * initial_cost_per_lot);
    }
}

bool Ledger::reserve_for_order(const AccountId& account_id,
                               OrderId order_id,
                               Side side,
                               Price price,
                               Quantity quantity,
                               int64_t cash_to_reserve,
                               Quantity inventory_to_reserve) {
    // Disallow overwriting an existing live order's reservation
    if (reservations_.find(order_id) != reservations_.end()) {
        return false;
    }

    auto& pos = get_or_create_account(account_id);

    if (side == Side::Buy) {
        if (pos.available_cash() < cash_to_reserve) {
            return false;
        }
        pos.reserved_cash += cash_to_reserve;
        pos.pending_buy_quantity += quantity;
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
        Quantity qty_to_release = std::min(canceled_qty, res.remaining_quantity);
        if (canceled_qty >= res.remaining_quantity) {
            cash_to_release = res.reserved_cash_amount;
            res.reserved_cash_amount = 0;
            res.remaining_quantity = 0;
        } else {
            // Documented exact integer proportional release
            cash_to_release = static_cast<int64_t>(
                (static_cast<__int128_t>(res.reserved_cash_amount) * canceled_qty) / res.remaining_quantity
            );
            res.reserved_cash_amount -= cash_to_release;
            res.remaining_quantity -= canceled_qty;
        }
        pos.reserved_cash = std::max<int64_t>(0, pos.reserved_cash - cash_to_release);
        pos.pending_buy_quantity = std::max<Quantity>(0, pos.pending_buy_quantity - qty_to_release);
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

    // Overflow pre-checks before state mutation
    auto buyer_cash_deduct = CheckedMath::safe_add_quantity(settlement.notional, buyer_fee);
    if (!buyer_cash_deduct) return false;
    auto buyer_new_inv = CheckedMath::safe_add_quantity(buyer.inventory, settlement.quantity);
    if (!buyer_new_inv) return false;
    auto buyer_new_cost = CheckedMath::safe_add_quantity(buyer.total_cost, *buyer_cash_deduct);
    if (!buyer_new_cost) return false;

    // 2. Adjust reservations
    // Buyer reservation release
    OrderId buyer_order_id = (settlement.taker_side == Side::Buy) ? settlement.taker_order_id : settlement.maker_order_id;
    auto b_res_it = reservations_.find(buyer_order_id);
    if (b_res_it != reservations_.end()) {
        auto& res = b_res_it->second;
        int64_t reserved_cash_portion = 0;
        Quantity rel_qty = std::min(settlement.quantity, res.remaining_quantity);
        if (settlement.quantity >= res.remaining_quantity) {
            reserved_cash_portion = res.reserved_cash_amount;
            res.reserved_cash_amount = 0;
            res.remaining_quantity = 0;
        } else {
            reserved_cash_portion = static_cast<int64_t>(
                (static_cast<__int128_t>(res.reserved_cash_amount) * settlement.quantity) / res.remaining_quantity
            );
            res.reserved_cash_amount -= reserved_cash_portion;
            res.remaining_quantity -= settlement.quantity;
        }
        buyer.reserved_cash = std::max<int64_t>(0, buyer.reserved_cash - reserved_cash_portion);
        buyer.pending_buy_quantity = std::max<Quantity>(0, buyer.pending_buy_quantity - rel_qty);
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
    buyer.cash -= *buyer_cash_deduct;
    buyer.total_fees_paid += buyer_fee;
    buyer.inventory = *buyer_new_inv;
    buyer.total_cost = *buyer_new_cost; // Buy fees explicitly capitalized in total cost

    // 4. Settle Seller
    seller.cash += (settlement.notional - seller_fee);
    seller.total_fees_paid += seller_fee;

    // Documented integer proportional cost of sold inventory
    int64_t cost_of_sold = 0;
    if (seller.inventory > 0) {
        cost_of_sold = static_cast<int64_t>(
            (static_cast<__int128_t>(seller.total_cost) * settlement.quantity) / seller.inventory
        );
    }
    seller.total_cost = std::max<int64_t>(0, seller.total_cost - cost_of_sold);

    // Net Realized PnL: (net trade proceeds) - (proportional acquisition cost of sold units)
    int64_t net_proceeds = settlement.notional - seller_fee;
    seller.realized_pnl += (net_proceeds - cost_of_sold);

    seller.inventory -= settlement.quantity;
    if (seller.inventory == 0) {
        seller.total_cost = 0;
    }

    // 5. Settle Fee Account
    fee_acc.cash += (buyer_fee + seller_fee);

    // 6. Record settled trade
    settled_trade_ids_.insert(settlement.trade_id);
    return true;
}

LedgerAuditResult Ledger::verify_all_invariants(int64_t expected_total_cash, Quantity expected_total_inv) const noexcept {
    if (!verify_cash_conservation(expected_total_cash)) {
        return {false, "Cash conservation breached: total cash != expected cash"};
    }
    if (!verify_inventory_conservation(expected_total_inv)) {
        return {false, "Inventory conservation breached: total inventory != expected inventory"};
    }

    std::unordered_map<AccountId, int64_t> sum_res_cash;
    std::unordered_map<AccountId, Quantity> sum_res_inv;
    std::unordered_map<AccountId, Quantity> sum_pending_buy;

    for (const auto& [id, res] : reservations_) {
        if (res.side == Side::Buy) {
            sum_res_cash[res.account_id] += res.reserved_cash_amount;
            sum_pending_buy[res.account_id] += res.remaining_quantity;
        } else {
            sum_res_inv[res.account_id] += res.reserved_inventory_amount;
        }
    }

    for (const auto& [id, pos] : accounts_) {
        if (pos.cash < 0) {
            return {false, "Account " + id + " has negative settled cash: " + std::to_string(pos.cash)};
        }
        if (pos.available_cash() < 0) {
            return {false, "Account " + id + " has negative available cash: " + std::to_string(pos.available_cash())};
        }
        if (pos.reserved_cash < 0) {
            return {false, "Account " + id + " has negative reserved cash: " + std::to_string(pos.reserved_cash)};
        }
        if (pos.inventory < 0) {
            return {false, "Account " + id + " has negative settled inventory: " + std::to_string(pos.inventory)};
        }
        if (pos.available_inventory() < 0) {
            return {false, "Account " + id + " has negative available inventory: " + std::to_string(pos.available_inventory())};
        }
        if (pos.reserved_inventory < 0) {
            return {false, "Account " + id + " has negative reserved inventory: " + std::to_string(pos.reserved_inventory)};
        }
        if (pos.pending_buy_quantity < 0) {
            return {false, "Account " + id + " has negative pending buy quantity: " + std::to_string(pos.pending_buy_quantity)};
        }
        if (pos.total_cost < 0) {
            return {false, "Account " + id + " has negative total cost: " + std::to_string(pos.total_cost)};
        }

        int64_t expected_res_cash = sum_res_cash[id];
        if (pos.reserved_cash != expected_res_cash) {
            return {false, "Account " + id + " reserved cash mismatch: ledger=" + std::to_string(pos.reserved_cash) + " vs sum=" + std::to_string(expected_res_cash)};
        }

        Quantity expected_res_inv = sum_res_inv[id];
        if (pos.reserved_inventory != expected_res_inv) {
            return {false, "Account " + id + " reserved inventory mismatch: ledger=" + std::to_string(pos.reserved_inventory) + " vs sum=" + std::to_string(expected_res_inv)};
        }

        Quantity expected_pending_buy = sum_pending_buy[id];
        if (pos.pending_buy_quantity != expected_pending_buy) {
            return {false, "Account " + id + " pending buy quantity mismatch: ledger=" + std::to_string(pos.pending_buy_quantity) + " vs sum=" + std::to_string(expected_pending_buy)};
        }
    }

    return {true, "All ledger balances, reservations, and conservation invariants verified successfully"};
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
