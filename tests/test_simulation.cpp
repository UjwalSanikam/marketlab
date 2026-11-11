#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cmath>

#include "marketlab/sim_types.hpp"
#include "marketlab/scheduler.hpp"
#include "marketlab/portfolio.hpp"
#include "marketlab/risk_engine.hpp"
#include "marketlab/strategy.hpp"
#include "marketlab/simulation_engine.hpp"
#include "marketlab/sha256.hpp"

using namespace marketlab;

#define SIM_TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "SIM TEST FAILED: " << msg << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
    } while (0)

// 1. Stable ordering of equal-time events (time, tier, seq)
bool test_scheduler_deterministic_tie_breaking() {
    std::cout << "[RUN] test_scheduler_deterministic_tie_breaking...\n";
    VirtualScheduler sched;
    std::vector<int> execution_order;

    // Schedule 4 events at the same timestamp 100 with different tiers and insertion orders
    sched.schedule(100, EventPriority::OrderAdmission, [&](SimTime) { execution_order.push_back(3); });
    sched.schedule(100, EventPriority::MarketDataDelivery, [&](SimTime) { execution_order.push_back(1); });
    sched.schedule(100, EventPriority::StrategyWakeup, [&](SimTime) { execution_order.push_back(2); });
    sched.schedule(100, EventPriority::ExecutionReportDelivery, [&](SimTime) { execution_order.push_back(4); });
    // And another OrderAdmission (should come after first OrderAdmission due to FIFO seq_id)
    sched.schedule(100, EventPriority::OrderAdmission, [&](SimTime) { execution_order.push_back(5); });

    while (sched.pending_event_count() > 0) {
        sched.step_one(200);
    }

    SIM_TEST_ASSERT(execution_order.size() == 5, "Expected 5 executed events");
    SIM_TEST_ASSERT(execution_order[0] == 1, "Tier 1 (MarketDataDelivery) must execute first");
    SIM_TEST_ASSERT(execution_order[1] == 2, "Tier 2 (StrategyWakeup) must execute second");
    SIM_TEST_ASSERT(execution_order[2] == 3, "Tier 3 first OrderAdmission must execute third");
    SIM_TEST_ASSERT(execution_order[3] == 5, "Tier 3 second OrderAdmission must execute fourth (FIFO)");
    SIM_TEST_ASSERT(execution_order[4] == 4, "Tier 4 (ExecutionReportDelivery) must execute fifth");

    // Cannot schedule in the past
    bool past_sched = sched.schedule(50, EventPriority::OrderAdmission, [](SimTime) {});
    SIM_TEST_ASSERT(!past_sched, "Must reject scheduling in the past");

    std::cout << "[PASS] test_scheduler_deterministic_tie_breaking\n";
    return true;
}

// 2. Pre-trade risk: Two orders competing for the same funds
bool test_competing_orders_for_cash() {
    std::cout << "[RUN] test_competing_orders_for_cash...\n";
    FeeConfig fees;
    Ledger ledger(fees);
    RiskEngine risk;
    OrderBook book;

    // Account with 10,000 cash, 0 inventory
    ledger.fund_account("TRADER", 10000, 0);

    // Order 1: Buy 80 @ 100 = 8000 notional (fits within 10,000 cash)
    NewOrderCommand cmd1{1, 101, Side::Buy, OrderType::Limit, 100, 80, TimeInForce::GTC};
    auto res1 = risk.evaluate_and_reserve("TRADER", cmd1, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(res1.approved, "Order 1 should be approved");

    const auto* pos = ledger.get_account("TRADER");
    SIM_TEST_ASSERT(pos->reserved_cash == 8000, "8000 cash should be reserved");
    SIM_TEST_ASSERT(pos->available_cash() == 2000, "Available cash should be 2000");

    // Order 2: Buy 30 @ 100 = 3000 notional (exceeds available 2000 cash!)
    NewOrderCommand cmd2{2, 102, Side::Buy, OrderType::Limit, 100, 30, TimeInForce::GTC};
    auto res2 = risk.evaluate_and_reserve("TRADER", cmd2, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(!res2.approved, "Order 2 must be rejected");
    SIM_TEST_ASSERT(res2.reject_reason == RiskRejectReason::InsufficientAvailableCash,
                    "Rejection reason must be InsufficientAvailableCash");

    // Verify zero state corruption after rejection
    SIM_TEST_ASSERT(pos->reserved_cash == 8000, "Reserved cash must remain 8000");
    SIM_TEST_ASSERT(pos->available_cash() == 2000, "Available cash must remain 2000");

    std::cout << "[PASS] test_competing_orders_for_cash\n";
    return true;
}

// 3. Pre-trade risk: No short selling
bool test_no_short_selling() {
    std::cout << "[RUN] test_no_short_selling...\n";
    FeeConfig fees;
    Ledger ledger(fees);
    RiskEngine risk;
    OrderBook book;

    // Account with 50 lots inventory
    ledger.fund_account("SELLER", 10000, 50);

    // Order 1: Sell 40 lots (approved, 10 available remaining)
    NewOrderCommand cmd1{1, 201, Side::Sell, OrderType::Limit, 100, 40, TimeInForce::GTC};
    auto res1 = risk.evaluate_and_reserve("SELLER", cmd1, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(res1.approved, "Order 1 should be approved");

    // Order 2: Sell 20 lots (exceeds available 10 inventory -> short selling prohibited!)
    NewOrderCommand cmd2{2, 202, Side::Sell, OrderType::Limit, 100, 20, TimeInForce::GTC};
    auto res2 = risk.evaluate_and_reserve("SELLER", cmd2, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(!res2.approved, "Order 2 must be rejected for short selling");
    SIM_TEST_ASSERT(res2.reject_reason == RiskRejectReason::ShortSellingProhibited,
                    "Must reject with ShortSellingProhibited");

    std::cout << "[PASS] test_no_short_selling\n";
    return true;
}

// 4. Reservation adjustment on partial fill and cancellation
bool test_partial_fill_and_cancel_reservation() {
    std::cout << "[RUN] test_partial_fill_and_cancel_reservation...\n";
    FeeConfig fees;
    Ledger ledger(fees);
    RiskEngine risk;
    OrderBook book;

    ledger.fund_account("BUYER", 10000, 0);
    ledger.fund_account("SELLER", 0, 100);

    // Buyer places Buy 50 @ 100 (reserves 5000 cash)
    NewOrderCommand buy_cmd{1, 301, Side::Buy, OrderType::Limit, 100, 50, TimeInForce::GTC};
    auto res_buy = risk.evaluate_and_reserve("BUYER", buy_cmd, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(res_buy.approved, "Buy approved");

    std::vector<Event> events;
    book.process(buy_cmd, events);

    // Seller crosses with 20 lots (partial fill)
    NewOrderCommand sell_cmd{2, 302, Side::Sell, OrderType::Limit, 100, 20, TimeInForce::IOC};
    auto res_sell = risk.evaluate_and_reserve("SELLER", sell_cmd, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(res_sell.approved, "Sell approved");

    events.clear();
    book.process(sell_cmd, events);

    // Trade of 20 lots @ 100 = 2000 notional
    TradeSettlement trade{1, 1000, 302, 301, "SELLER", "BUYER", Side::Sell, 100, 20, 2000, 0, 0};
    ledger.settle_trade(trade);

    const auto* buyer_pos = ledger.get_account("BUYER");
    // After 20 filled: 3000 should remain reserved, cash balance = 8000, inventory = 20
    SIM_TEST_ASSERT(buyer_pos->reserved_cash == 3000, "3000 cash should remain reserved");
    SIM_TEST_ASSERT(buyer_pos->cash == 8000, "Buyer cash should be 8000");
    SIM_TEST_ASSERT(buyer_pos->inventory == 20, "Buyer inventory should be 20");

    // Now cancel the remaining 30 lots of Order 301
    CancelOrderCommand cancel_cmd{3, 301};
    events.clear();
    book.process(cancel_cmd, events);
    ledger.release_reservation_on_cancel(301, 30);

    SIM_TEST_ASSERT(buyer_pos->reserved_cash == 0, "All remaining reservation must be released");
    SIM_TEST_ASSERT(buyer_pos->available_cash() == 8000, "All remaining cash must be available");

    std::cout << "[PASS] test_partial_fill_and_cancel_reservation\n";
    return true;
}

// 5. Cancel ownership check
bool test_cancel_ownership() {
    std::cout << "[RUN] test_cancel_ownership...\n";
    FeeConfig fees;
    Ledger ledger(fees);
    RiskEngine risk;
    OrderBook book;

    ledger.fund_account("ALICE", 5000, 0);
    ledger.fund_account("BOB", 5000, 0);

    NewOrderCommand cmd{1, 401, Side::Buy, OrderType::Limit, 100, 10, TimeInForce::GTC};
    auto res_alice = risk.evaluate_and_reserve("ALICE", cmd, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(res_alice.approved, "Alice order should be approved");

    // BOB attempts to cancel ALICE's order
    bool bob_can_cancel = risk.can_cancel_order("BOB", 401, ledger);
    SIM_TEST_ASSERT(!bob_can_cancel, "Bob must not be allowed to cancel Alice's order");

    // ALICE cancels her own order
    bool alice_can_cancel = risk.can_cancel_order("ALICE", 401, ledger);
    SIM_TEST_ASSERT(alice_can_cancel, "Alice must be allowed to cancel her own order");

    std::cout << "[PASS] test_cancel_ownership\n";
    return true;
}

// 6. Fee calculation, monetary rounding, and conservation
bool test_fees_and_conservation() {
    std::cout << "[RUN] test_fees_and_conservation...\n";
    FeeConfig fees;
    fees.maker_fee_bps = 10; // 0.10%
    fees.taker_fee_bps = 20; // 0.20%
    fees.fee_account_id = "EXCHANGE_FEES";

    Ledger ledger(fees);

    ledger.fund_account("BUYER", 100000, 0);
    ledger.fund_account("SELLER", 0, 100);

    int64_t initial_cash = ledger.get_total_cash();
    Quantity initial_inv = ledger.get_total_inventory();

    // Trade: 10 lots @ 500 = 5000 notional
    // Taker fee = 5000 * 20 / 10000 = 10 units
    // Maker fee = 5000 * 10 / 10000 = 5 units
    int64_t notional = 5000;
    int64_t taker_fee = fees.calculate_taker_fee(notional);
    int64_t maker_fee = fees.calculate_maker_fee(notional);

    SIM_TEST_ASSERT(taker_fee == 10, "Taker fee should be 10");
    SIM_TEST_ASSERT(maker_fee == 5, "Maker fee should be 5");

    TradeSettlement trade{1, 1000, 501, 502, "BUYER", "SELLER", Side::Buy, 500, 10, notional, taker_fee, maker_fee};
    ledger.settle_trade(trade);

    const auto* buyer = ledger.get_account("BUYER");
    const auto* seller = ledger.get_account("SELLER");
    const auto* fee_acc = ledger.get_account("EXCHANGE_FEES");

    // Buyer paid notional + taker_fee = 5000 + 10 = 5010
    SIM_TEST_ASSERT(buyer->cash == (100000 - 5010), "Buyer cash after fee");
    SIM_TEST_ASSERT(buyer->inventory == 10, "Buyer inventory");

    // Seller received notional - maker_fee = 5000 - 5 = 4995
    SIM_TEST_ASSERT(seller->cash == 4995, "Seller cash after fee");
    SIM_TEST_ASSERT(seller->inventory == 90, "Seller inventory");

    // Fee account collected 10 + 5 = 15
    SIM_TEST_ASSERT(fee_acc->cash == 15, "Fee account collected 15");

    // Strict conservation
    SIM_TEST_ASSERT(ledger.verify_cash_conservation(initial_cash), "Cash must be strictly conserved");
    SIM_TEST_ASSERT(ledger.verify_inventory_conservation(initial_inv), "Inventory must be strictly conserved");

    // Test idempotence: duplicate trade delivery must reject and not double-apply
    bool re_settle = ledger.settle_trade(trade);
    SIM_TEST_ASSERT(!re_settle, "Duplicate trade settlement must be rejected");
    SIM_TEST_ASSERT(buyer->cash == (100000 - 5010), "Buyer cash must not change on duplicate delivery");

    std::cout << "[PASS] test_fees_and_conservation\n";
    return true;
}

// 7. Hand-calculated Realized and Unrealized PnL scenario
bool test_hand_calculated_pnl() {
    std::cout << "[RUN] test_hand_calculated_pnl...\n";
    FeeConfig fees; // Zero fees for clean PnL isolation
    Ledger ledger(fees);

    ledger.fund_account("TRADER", 50000, 0);
    ledger.fund_account("COUNTERPARTY", 50000, 100);

    // Trade 1: Buy 10 lots @ 100. Cost basis = 100.
    TradeSettlement t1{1, 1000, 1, 2, "TRADER", "COUNTERPARTY", Side::Buy, 100, 10, 1000, 0, 0};
    ledger.settle_trade(t1);

    const auto* pos = ledger.get_account("TRADER");
    SIM_TEST_ASSERT(pos->inventory == 10, "Inventory 10");
    SIM_TEST_ASSERT(pos->cost_basis_per_lot() == 100, "Cost basis 100");

    // Trade 2: Buy 10 lots @ 200. Total 20 lots. Total cost = 1000 + 2000 = 3000. Avg cost = 150.
    TradeSettlement t2{2, 2000, 3, 4, "TRADER", "COUNTERPARTY", Side::Buy, 200, 10, 2000, 0, 0};
    ledger.settle_trade(t2);
    SIM_TEST_ASSERT(pos->inventory == 20, "Inventory 20");
    SIM_TEST_ASSERT(pos->cost_basis_per_lot() == 150, "Avg cost basis 150");

    // Trade 3: Sell 5 lots @ 250.
    // Realized PnL on 5 lots = (250 - 150) * 5 = +500.
    TradeSettlement t3{3, 3000, 5, 6, "TRADER", "COUNTERPARTY", Side::Sell, 250, 5, 1250, 0, 0};
    ledger.settle_trade(t3);
    SIM_TEST_ASSERT(pos->inventory == 15, "Inventory 15");
    SIM_TEST_ASSERT(pos->realized_pnl == 500, "Realized PnL must be exactly +500");
    SIM_TEST_ASSERT(pos->cost_basis_per_lot() == 150, "Cost basis remains 150");

    // Unrealized PnL check at mark price 180:
    // 15 lots * (180 - 150) = +450.
    auto unrl = pos->calculate_unrealized_pnl(180);
    SIM_TEST_ASSERT(unrl.has_value() && *unrl == 450, "Unrealized PnL must be exactly +450");

    std::cout << "[PASS] test_hand_calculated_pnl\n";
    return true;
}

// 8. Delayed Execution Reports vs Authoritative Settlement
bool test_delayed_execution_reports() {
    std::cout << "[RUN] test_delayed_execution_reports...\n";
    SimulationConfig config;
    config.latencies.market_data_delay = 5000000;      // 5ms
    config.latencies.strategy_delay = 2000000;         // 2ms
    config.latencies.order_entry_delay = 3000000;      // 3ms
    config.latencies.execution_report_delay = 10000000; // 10ms report delay
    config.run_horizon_ns = 50000000; // 50ms

    SimulationEngine engine(config);

    // Initial mid observation at t = 1,000,000 (1ms)
    // Delivered to strategy at 1ms + 5ms = 6ms
    // Strategy submits quote at 6ms + 2ms = 8ms
    // Order arrives at matching engine at 8ms + 3ms = 11ms
    // Matcher accepts order at 11ms.
    // Report delivered to strategy at 11ms + 10ms = 21ms!
    engine.emit_market_observation(1000000, 99, 101, 100, 100, 10);

    auto summary = engine.run();

    SIM_TEST_ASSERT(summary.cash_conserved, "Cash conserved");
    SIM_TEST_ASSERT(summary.inventory_conserved, "Inventory conserved");
    SIM_TEST_ASSERT(summary.strategy_orders_submitted >= 1, "Strategy should have submitted quotes");

    std::cout << "[PASS] test_delayed_execution_reports\n";
    return true;
}

// 9. Deterministic Replay Reproducibility (SHA-256 Parity)
bool test_simulation_reproducibility() {
    std::cout << "[RUN] test_simulation_reproducibility...\n";
    SimulationConfig config;
    config.seed = 777;
    config.run_horizon_ns = 2000000000ULL; // 2 seconds

    auto run_sim_and_get_hashes = [](const SimulationConfig& cfg,
                                     const std::string& ev_file,
                                     const std::string& st_file,
                                     const std::string& fl_file) -> std::pair<std::string, std::string> {
        SimulationEngine eng(cfg);

        // Feed background market activity
        for (SimTime t = 100000000; t < cfg.run_horizon_ns; t += 200000000) {
            eng.emit_market_observation(t, 99, 101, 100, 100, 5);
            // Background buy order
            eng.submit_background_order(cfg.background_account_id, Side::Buy, OrderType::Limit, 102, 5, TimeInForce::IOC, t + 50000000);
        }

        eng.run();
        eng.export_events_binary(ev_file);
        eng.export_state_binary(st_file);
        eng.export_fills_binary(fl_file);

        auto h_ev = Sha256::hash_file(ev_file);
        auto h_st = Sha256::hash_file(st_file);
        return {h_ev.value_or(""), h_st.value_or("")};
    };

    std::string e1 = "test_sim1_events.jsonl", s1 = "test_sim1_state.json", f1 = "test_sim1_fills.jsonl";
    std::string e2 = "test_sim2_events.jsonl", s2 = "test_sim2_state.json", f2 = "test_sim2_fills.jsonl";

    auto [ev_h1, st_h1] = run_sim_and_get_hashes(config, e1, s1, f1);
    auto [ev_h2, st_h2] = run_sim_and_get_hashes(config, e2, s2, f2);

    SIM_TEST_ASSERT(ev_h1 == ev_h2, "Two simulation runs must produce identical events SHA-256");
    SIM_TEST_ASSERT(st_h1 == st_h2, "Two simulation runs must produce identical state SHA-256");

    // Clean files
    std::remove(e1.c_str()); std::remove(s1.c_str()); std::remove(f1.c_str());
    std::remove(e2.c_str()); std::remove(s2.c_str()); std::remove(f2.c_str());

    std::cout << "[PASS] test_simulation_reproducibility\n";
    return true;
}

// 10. Simulation Config JSON Roundtrip
bool test_config_json_roundtrip() {
    std::cout << "[RUN] test_config_json_roundtrip...\n";
    SimulationConfig original;
    original.seed = 999;
    original.run_horizon_ns = 5000000000ULL;
    original.fees.maker_fee_bps = 8;
    original.fees.taker_fee_bps = 15;
    original.strategy_config.half_spread = 3;
    original.strategy_config.quote_size = 12;

    std::string json_repr = original.to_json();
    SimulationConfig parsed = SimulationConfig::from_json(json_repr);

    SIM_TEST_ASSERT(parsed.seed == 999, "Seed parsed");
    SIM_TEST_ASSERT(parsed.run_horizon_ns == 5000000000ULL, "Horizon parsed");
    SIM_TEST_ASSERT(parsed.fees.maker_fee_bps == 8, "Maker fee parsed");
    SIM_TEST_ASSERT(parsed.fees.taker_fee_bps == 15, "Taker fee parsed");
    SIM_TEST_ASSERT(parsed.strategy_config.half_spread == 3, "Half spread parsed");
    SIM_TEST_ASSERT(parsed.strategy_config.quote_size == 12, "Quote size parsed");

    std::cout << "[PASS] test_config_json_roundtrip\n";
    return true;
}

// 11. End-to-end synthetic scenario execution and conservation audit
bool test_synthetic_scenario_execution() {
    std::cout << "[RUN] test_synthetic_scenario_execution...\n";
    SimulationConfig cfg;
    cfg.seed = 42;
    cfg.run_horizon_ns = 3000000000ULL; // 3 seconds
    cfg.fees.maker_fee_bps = 50; // 0.50%
    cfg.fees.taker_fee_bps = 100; // 1.00%

    SimulationEngine engine(cfg);
    engine.setup_synthetic_scenario();
    auto summary = engine.run();

    SIM_TEST_ASSERT(summary.cash_conserved, "Synthetic session: cash must be strictly conserved");
    SIM_TEST_ASSERT(summary.inventory_conserved, "Synthetic session: inventory must be strictly conserved");
    SIM_TEST_ASSERT(summary.total_trades > 0, "Synthetic session: trades must have occurred");
    SIM_TEST_ASSERT(summary.strategy_trades > 0, "Strategy must have participated in trades");
    SIM_TEST_ASSERT(summary.total_fees_collected > 0, "Exchange fees must have been collected");

    std::cout << "[PASS] test_synthetic_scenario_execution (trades=" << summary.total_trades
              << ", strategy_trades=" << summary.strategy_trades
              << ", fees_collected=" << summary.total_fees_collected << ")\n";
    return true;
}

// REGRESSION 1: Pending buy quantities must be included in projected inventory limit
bool regression_pending_buy_inventory_cap() {
    std::cout << "[RUN] regression_pending_buy_inventory_cap...\n";
    FeeConfig fees;
    Ledger ledger(fees);
    RiskEngine risk(RiskLimits{100, 1000000, 100, 10000000000ULL, false}); // max_inventory = 100
    OrderBook book;

    ledger.fund_account("TRADER", 50000, 80); // Settled inventory = 80

    // Order 1: Buy 15 lots @ 100. (80 + 15 = 95 <= 100). Approved!
    NewOrderCommand cmd1{1, 1001, Side::Buy, OrderType::Limit, 100, 15, TimeInForce::GTC};
    auto res1 = risk.evaluate_and_reserve("TRADER", cmd1, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(res1.approved, "Order 1 should be approved");

    // Order 2: Buy 15 lots @ 100.
    // Outstanding buy is 15. Total projected = 80 + 15 + 15 = 110 > 100!
    // Must be rejected!
    NewOrderCommand cmd2{2, 1002, Side::Buy, OrderType::Limit, 100, 15, TimeInForce::GTC};
    auto res2 = risk.evaluate_and_reserve("TRADER", cmd2, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(!res2.approved, "Order 2 must be rejected for exceeding max inventory including pending buys");
    SIM_TEST_ASSERT(res2.reject_reason == RiskRejectReason::MaxInventoryExceeded, "Reject reason must be MaxInventoryExceeded");

    // Cancel Order 1 -> releases pending buy quantity
    ledger.release_reservation_on_cancel(1001, 15);

    // Order 3: Buy 15 lots @ 100. Now pending is 0, so 80 + 15 = 95 <= 100. Approved!
    NewOrderCommand cmd3{3, 1003, Side::Buy, OrderType::Limit, 100, 15, TimeInForce::GTC};
    auto res3 = risk.evaluate_and_reserve("TRADER", cmd3, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(res3.approved, "Order 3 should be approved after cancellation of Order 1");

    std::cout << "[PASS] regression_pending_buy_inventory_cap\n";
    return true;
}

// REGRESSION 2: Self-trade prevention must inspect entire executable matching path
bool regression_self_trade_prevention_deep_book() {
    std::cout << "[RUN] regression_self_trade_prevention_deep_book...\n";
    RiskEngine risk(RiskLimits{100, 1000000, 1000, 10000000000ULL, false}); // STP enabled
    FeeConfig fees;
    Ledger ledger(fees);
    OrderBook book;

    ledger.fund_account("ALICE", 50000, 50);
    ledger.fund_account("BOB", 50000, 50);

    std::vector<Event> events;
    // Level 1: Ask @ 100, qty 5 (BOB)
    NewOrderCommand bob_ask{1, 101, Side::Sell, OrderType::Limit, 100, 5, TimeInForce::GTC};
    auto res_b = risk.evaluate_and_reserve("BOB", bob_ask, 100, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(res_b.approved, "Bob ask approved");
    book.process(bob_ask, events);

    // Level 2: Ask @ 102, qty 10 (ALICE)
    events.clear();
    NewOrderCommand alice_ask{2, 102, Side::Sell, OrderType::Limit, 102, 10, TimeInForce::GTC};
    auto res_a = risk.evaluate_and_reserve("ALICE", alice_ask, 102, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(res_a.approved, "Alice ask approved");
    book.process(alice_ask, events);

    // Now ALICE submits Buy 15 @ 102.
    // It would match BOB (5 @ 100) and then match ALICE (10 @ 102).
    // Buggy STP only checked top of book (BOB @ 100) and approved!
    // Correct STP must check the entire executable path and REJECT!
    NewOrderCommand alice_buy{3, 103, Side::Buy, OrderType::Limit, 102, 15, TimeInForce::IOC};
    auto res = risk.evaluate_and_reserve("ALICE", alice_buy, 102, 1000, 1000, book, ledger);
    SIM_TEST_ASSERT(!res.approved, "ALICE buy must be rejected for deep self-trade at Level 2");
    SIM_TEST_ASSERT(res.reject_reason == RiskRejectReason::SelfTradeProhibited, "Must reject with SelfTradeProhibited");

    std::cout << "[PASS] regression_self_trade_prevention_deep_book\n";
    return true;
}

// REGRESSION 3: Market-buy protection cap enforced during execution (IOC semantics, cash never negative)
bool regression_market_buy_protected_ioc_execution() {
    std::cout << "[RUN] regression_market_buy_protected_ioc_execution...\n";
    SimulationConfig cfg;
    cfg.latencies = {}; // Zero latency
    cfg.strategy_starting_cash = 1500;
    cfg.strategy_starting_inventory = 0;
    cfg.strategy_config.quote_size = 0; // Disable passive quoting so strategy only submits explicit orders
    cfg.background_starting_cash = 100000;
    cfg.background_starting_inventory = 1000;

    SimulationEngine engine(cfg);

    // Book liquidity:
    // Level 1: 1 lot @ 100
    // Level 2: 10 lots @ 500 (far above reasonable prices)
    engine.submit_background_order("BACKGROUND", Side::Sell, OrderType::Limit, 100, 1, TimeInForce::GTC, 10);
    engine.submit_background_order("BACKGROUND", Side::Sell, OrderType::Limit, 500, 10, TimeInForce::GTC, 20);

    // Provide initial mark price observation at t=30: mark = 100
    engine.emit_market_observation(30, 99, 101, 100, 100, 1);

    // At t=50, Strategy submits market buy for 10 lots with explicit protection cap 110.
    // If unprotected, it sweeps 1 lot @ 100 + 9 lots @ 500 = 4600 notional, which would make Alice's cash (1500) NEGATIVE!
    // With protected IOC execution, it must fill 1 lot @ 100 and cancel the remaining 9 lots as IOC remainder!
    engine.submit_background_order(cfg.strategy_account_id, Side::Buy, OrderType::Market, 110, 10, TimeInForce::IOC, 50);

    auto summary = engine.run();
    (void)summary;

    const auto* pos = engine.ledger().get_account(cfg.strategy_account_id);
    SIM_TEST_ASSERT(pos != nullptr, "Strategy account must exist");
    SIM_TEST_ASSERT(pos->cash >= 0, "Cash must NEVER become negative!");
    SIM_TEST_ASSERT(pos->inventory == 1, "Only 1 lot @ 100 should have filled under the 110 protection cap");
    SIM_TEST_ASSERT(pos->reserved_cash == 0, "All remaining reservation must be released");
    SIM_TEST_ASSERT(pos->available_cash() == pos->cash, "Available cash must match settled cash");

    // Level 500 must still be resting on the book (untouched)
    auto ask_snap = engine.book().get_snapshot().asks;
    SIM_TEST_ASSERT(!ask_snap.empty() && ask_snap.front().price == 500, "Level 500 must be untouched by protected market buy");

    std::cout << "[PASS] regression_market_buy_protected_ioc_execution\n";
    return true;
}

// REGRESSION 4: Simulation causality & time arithmetic overflow
bool regression_simulation_causality() {
    std::cout << "[RUN] regression_simulation_causality...\n";
    SimulationConfig cfg;
    cfg.run_horizon_ns = 1000000000ULL; // 1 second
    cfg.latencies.market_data_delay = 1000000; // 1ms

    SimulationEngine engine(cfg);

    // Schedule observation 1 at t=100ms with mark=100
    engine.emit_market_observation(100000000ULL, 99, 101, 100, 100, 5);
    // Schedule observation 2 at t=500ms with mark=250
    engine.emit_market_observation(500000000ULL, 249, 251, 250, 250, 5);

    // At t=0 before stepping: engine's authoritative last_market_data_time must be 0, NOT 500ms!
    SIM_TEST_ASSERT(engine.last_market_data_time() == 0, "Future scheduled data cannot update runtime in the past!");
    SIM_TEST_ASSERT(!engine.last_mark_price().has_value(), "Future mark price cannot exist prior to scheduled time!");

    // Test time arithmetic overflow:
    auto ovf = safe_add_time(std::numeric_limits<SimTime>::max() - 5, 10);
    SIM_TEST_ASSERT(!ovf.has_value(), "safe_add_time must catch uint64_t overflow");

    std::cout << "[PASS] regression_simulation_causality\n";
    return true;
}

// REGRESSION 5: Integer accounting & Net PnL capitalization with buy fees
bool regression_accounting_net_pnl_with_buy_fees() {
    std::cout << "[RUN] regression_accounting_net_pnl_with_buy_fees...\n";
    FeeConfig fees;
    fees.maker_fee_bps = 10; // 0.10%
    fees.taker_fee_bps = 20; // 0.20%
    fees.fee_account_id = "FEES";

    Ledger ledger(fees);
    ledger.fund_account("TRADER", 10000, 0);
    ledger.fund_account("COUNTERPARTY", 10000, 100);

    // Trade 1: Buy 10 lots @ 100 as Taker.
    // Notional = 1000. Taker fee = 1000 * 20 / 10000 = 2.
    // Cash deducted = 1000 + 2 = 1002. Trader cash = 10000 - 1002 = 8998.
    // Capitalized total cost = 1002.
    TradeSettlement t1{1, 1000, 101, 102, "TRADER", "COUNTERPARTY", Side::Buy, 100, 10, 1000, 2, 1};
    bool s1 = ledger.settle_trade(t1);
    SIM_TEST_ASSERT(s1, "Trade 1 settled");

    const auto* pos = ledger.get_account("TRADER");
    SIM_TEST_ASSERT(pos->inventory == 10, "Inventory = 10");
    SIM_TEST_ASSERT(pos->total_cost == 1002, "Total cost capitalized with buy fees = 1002");
    SIM_TEST_ASSERT(pos->cost_basis_per_lot() == 100, "Cost basis per lot floor = 100");

    // Trade 2: Sell 5 lots @ 120 as Maker.
    // Notional = 600. Maker fee = 600 * 10 / 10000 = 0 (integer division: 6 / 10 = 0).
    // Cost of 5 lots sold = 1002 * 5 / 10 = 501.
    // Net proceeds = 600 - 0 = 600.
    // Realized PnL on Trade 2 = 600 - 501 = +99.
    // Remaining total cost = 1002 - 501 = 501.
    TradeSettlement t2{2, 2000, 103, 104, "COUNTERPARTY", "TRADER", Side::Buy, 120, 5, 600, 1, 0};
    bool s2 = ledger.settle_trade(t2);
    SIM_TEST_ASSERT(s2, "Trade 2 settled");

    SIM_TEST_ASSERT(pos->inventory == 5, "Remaining inventory = 5");
    SIM_TEST_ASSERT(pos->total_cost == 501, "Remaining total cost = 501");
    SIM_TEST_ASSERT(pos->realized_pnl == 99, "Realized PnL = +99");

    // Trade 3: Sell remaining 5 lots @ 150 as Taker.
    // Notional = 750. Taker fee = 750 * 20 / 10000 = 1.
    // Cost of 5 lots sold = 501.
    // Net proceeds = 750 - 1 = 749.
    // Realized PnL on Trade 3 = 749 - 501 = +248.
    // Total Realized PnL = 99 + 248 = +347.
    TradeSettlement t3{3, 3000, 105, 106, "TRADER", "COUNTERPARTY", Side::Sell, 150, 5, 750, 1, 0};
    bool s3 = ledger.settle_trade(t3);
    SIM_TEST_ASSERT(s3, "Trade 3 settled");

    SIM_TEST_ASSERT(pos->inventory == 0, "Inventory = 0");
    SIM_TEST_ASSERT(pos->total_cost == 0, "Total cost reset to 0 when flat");
    SIM_TEST_ASSERT(pos->realized_pnl == 347, "Total Realized PnL = +347");

    // Verify Net Cash Change:
    // Initial cash: 10000.
    // Trade 1: -1002 -> 8998.
    // Trade 2: +600 -> 9598.
    // Trade 3: +749 -> 10347.
    // Final cash: 10347. Net cash profit = +347!
    SIM_TEST_ASSERT(pos->cash == 10347, "Final cash = 10347");
    SIM_TEST_ASSERT(pos->cash - 10000 == pos->realized_pnl, "Net cash change must IDENTICALLY match realized PnL!");

    std::cout << "[PASS] regression_accounting_net_pnl_with_buy_fees\n";
    return true;
}

// REGRESSION 6: Expanded Ledger Invariant Audit & Reservation Protection
bool regression_expanded_ledger_invariants() {
    std::cout << "[RUN] regression_expanded_ledger_invariants...\n";
    FeeConfig fees;
    Ledger ledger(fees);

    ledger.fund_account("USER_A", 10000, 50);
    ledger.fund_account("USER_B", 10000, 50);

    // Reserve for order 1
    bool r1 = ledger.reserve_for_order("USER_A", 501, Side::Buy, 100, 10, 1000, 0);
    SIM_TEST_ASSERT(r1, "Order 501 reserved");

    // Cannot overwrite live order 501's reservation
    bool r_dup = ledger.reserve_for_order("USER_B", 501, Side::Buy, 100, 5, 500, 0);
    SIM_TEST_ASSERT(!r_dup, "Cannot overwrite live reservation with duplicate order ID");

    // Unknown order cancellation does not alter state
    bool c_unknown = ledger.release_reservation_on_cancel(99999, 10);
    SIM_TEST_ASSERT(!c_unknown, "Canceling unknown order must return false");

    // Invariant check passes
    auto audit = ledger.verify_all_invariants(20000, 100);
    SIM_TEST_ASSERT(audit.ok, audit.message);

    std::cout << "[PASS] regression_expanded_ledger_invariants\n";
    return true;
}

// REGRESSION 7: Strict Simulation Configuration Validation
bool regression_config_strict_validation() {
    std::cout << "[RUN] regression_config_strict_validation...\n";

    // 1. Invalid tick size (zero)
    std::string bad_json_tick = "{\"instrument\": {\"tick_size\": 0}}";
    auto [c1, err1] = SimulationConfig::from_json_strict(bad_json_tick);
    SIM_TEST_ASSERT(!c1.has_value(), "Must reject zero tick_size");

    // 2. Invalid run horizon (zero)
    std::string bad_json_horizon = "{\"run_horizon_ns\": 0}";
    auto [c2, err2] = SimulationConfig::from_json_strict(bad_json_horizon);
    SIM_TEST_ASSERT(!c2.has_value(), "Must reject zero run_horizon_ns");

    // 3. Invalid maker fee (negative)
    std::string bad_json_fee = "{\"fees\": {\"maker_fee_bps\": -5}}";
    auto [c3, err3] = SimulationConfig::from_json_strict(bad_json_fee);
    SIM_TEST_ASSERT(!c3.has_value(), "Must reject negative fee");

    // 4. Identical strategy and background account IDs
    std::string bad_json_accounts = "{\"strategy_account_id\": \"SAME\", \"background_account_id\": \"SAME\"}";
    auto [c4, err4] = SimulationConfig::from_json_strict(bad_json_accounts);
    SIM_TEST_ASSERT(!c4.has_value(), "Must reject identical account IDs");

    // 5. Valid config must pass
    SimulationConfig valid_cfg;
    auto val_res = valid_cfg.validate();
    SIM_TEST_ASSERT(val_res.valid, "Default config must be valid");

    std::cout << "[PASS] regression_config_strict_validation\n";
    return true;
}

int main() {
    std::cout << "Running MarketLab Milestone 2 Simulation & Accounting Tests...\n\n";

    bool all_ok = true;
    all_ok &= test_scheduler_deterministic_tie_breaking();
    all_ok &= test_competing_orders_for_cash();
    all_ok &= test_no_short_selling();
    all_ok &= test_partial_fill_and_cancel_reservation();
    all_ok &= test_cancel_ownership();
    all_ok &= test_fees_and_conservation();
    all_ok &= test_hand_calculated_pnl();
    all_ok &= test_delayed_execution_reports();
    all_ok &= test_simulation_reproducibility();
    all_ok &= test_config_json_roundtrip();
    all_ok &= test_synthetic_scenario_execution();
    all_ok &= regression_pending_buy_inventory_cap();
    all_ok &= regression_self_trade_prevention_deep_book();
    all_ok &= regression_market_buy_protected_ioc_execution();
    all_ok &= regression_simulation_causality();
    all_ok &= regression_accounting_net_pnl_with_buy_fees();
    all_ok &= regression_expanded_ledger_invariants();
    all_ok &= regression_config_strict_validation();

    if (all_ok) {
        std::cout << "\nALL MILESTONE 2 SIMULATION TESTS PASSED SUCCESSFULLY!\n";
        return 0;
    } else {
        std::cerr << "\nSOME SIMULATION TESTS FAILED!\n";
        return 1;
    }
}
