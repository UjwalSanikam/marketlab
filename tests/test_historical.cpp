#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <sstream>

#include "marketlab/market_data_types.hpp"
#include "marketlab/market_data_normalizer.hpp"
#include "marketlab/reconstructed_book.hpp"
#include "marketlab/historical_execution_adapter.hpp"
#include "marketlab/historical_replay_engine.hpp"
#include "marketlab/sha256.hpp"

using namespace marketlab;

#define HIST_TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "HIST TEST FAILED: " << msg << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
    } while (0)

// 1. Exact integer decimal parsing
bool test_exact_decimal_parsing() {
    std::cout << "[RUN] test_exact_decimal_parsing...\n";

    // Price scale = 2
    auto p1 = DecimalParser::parse_decimal("73948.50", 2);
    HIST_TEST_ASSERT(p1.has_value() && *p1 == 7394850, "Failed parsing 73948.50 with scale 2");

    auto p2 = DecimalParser::parse_decimal("100", 2);
    HIST_TEST_ASSERT(p2.has_value() && *p2 == 10000, "Failed parsing integer with scale 2");

    auto p3 = DecimalParser::parse_decimal("0.05", 2);
    HIST_TEST_ASSERT(p3.has_value() && *p3 == 5, "Failed parsing 0.05 with scale 2");

    // Quantity scale = 5
    auto q1 = DecimalParser::parse_decimal("1.50000", 5);
    HIST_TEST_ASSERT(q1.has_value() && *q1 == 150000, "Failed parsing 1.50000 with scale 5");

    auto q2 = DecimalParser::parse_decimal("0.00001", 5);
    HIST_TEST_ASSERT(q2.has_value() && *q2 == 1, "Failed parsing 0.00001 with scale 5");

    // Sub-tick rejection (e.g. 3 decimal places with scale 2)
    auto p_invalid = DecimalParser::parse_decimal("100.005", 2);
    HIST_TEST_ASSERT(!p_invalid.has_value(), "Sub-tick precision must be rejected");

    // Trailing garbage rejection
    auto p_garbage = DecimalParser::parse_decimal("100.50abc", 2);
    HIST_TEST_ASSERT(!p_garbage.has_value(), "Trailing characters must be rejected");

    return true;
}

// 2. Snapshot-update bridge protocol (U <= lastUpdateId + 1 <= u)
bool test_bridge_protocol() {
    std::cout << "[RUN] test_bridge_protocol...\n";
    ReconstructedOrderBook book;
    HIST_TEST_ASSERT(book.status() == FeedStatus::Uninitialized, "Initial status must be Uninitialized");

    // Applying snapshot moves status to Synchronizing
    book.apply_snapshot(100, {{6000000, 100000}}, {{6001000, 100000}});
    HIST_TEST_ASSERT(book.status() == FeedStatus::Synchronizing, "Status must be Synchronizing after snapshot");
    HIST_TEST_ASSERT(book.last_update_id() == 100, "last_update_id must match snapshot");

    // Stale update (u <= 100) must be dropped
    bool stale_applied = book.apply_depth_update(90, 95, {}, {});
    HIST_TEST_ASSERT(!stale_applied, "Stale update u=95 <= 100 must not apply");
    HIST_TEST_ASSERT(book.status() == FeedStatus::Synchronizing, "Status must remain Synchronizing");

    // Valid bridge update (U=98 <= 101 <= u=105) must transition to Valid
    bool bridge_applied = book.apply_depth_update(98, 105, {{6000000, 150000}}, {{6001000, 120000}});
    HIST_TEST_ASSERT(bridge_applied, "Bridge update bridging 101 must apply");
    HIST_TEST_ASSERT(book.status() == FeedStatus::Valid, "Status must transition to Valid");
    HIST_TEST_ASSERT(book.last_update_id() == 105, "last_update_id must update to 105");

    return true;
}

// 3. Sequence gap detection and feed status transition to INVALID
bool test_sequence_gap_detection() {
    std::cout << "[RUN] test_sequence_gap_detection...\n";
    ReconstructedOrderBook book;
    book.apply_snapshot(100, {{6000000, 100000}}, {{6001000, 100000}});
    book.apply_depth_update(98, 105, {}, {});
    HIST_TEST_ASSERT(book.is_valid(), "Book must be Valid after bridge update");

    // Next update has U=108 > 105 + 1: Sequence gap!
    bool gap_applied = book.apply_depth_update(108, 115, {}, {});
    HIST_TEST_ASSERT(!gap_applied, "Gapped update must not apply");
    HIST_TEST_ASSERT(book.status() == FeedStatus::Invalid, "Status must become Invalid on sequence gap");
    HIST_TEST_ASSERT(!book.invalid_reason().empty(), "Invalid reason must describe gap");

    return true;
}

// 4. Feed status state transitions (UNINITIALIZED -> SYNCHRONIZING -> VALID -> INVALID)
bool test_feed_status_transitions() {
    std::cout << "[RUN] test_feed_status_transitions...\n";
    ReconstructedOrderBook book;
    HIST_TEST_ASSERT(book.status() == FeedStatus::Uninitialized, "State 1: Uninitialized");

    book.apply_snapshot(200, {}, {});
    HIST_TEST_ASSERT(book.status() == FeedStatus::Synchronizing, "State 2: Synchronizing");

    book.apply_depth_update(199, 205, {}, {});
    HIST_TEST_ASSERT(book.status() == FeedStatus::Valid, "State 3: Valid");

    book.mark_invalid("Explicit disconnection");
    HIST_TEST_ASSERT(book.status() == FeedStatus::Invalid, "State 4: Invalid");

    return true;
}

// 5. Reconstructed book price-level updates, adds, and zero-quantity removals
bool test_reconstructed_book_levels() {
    std::cout << "[RUN] test_reconstructed_book_levels...\n";
    ReconstructedOrderBook book;
    book.apply_snapshot(100, {{6000000, 100000}, {5999000, 200000}}, {{6001000, 100000}, {6002000, 300000}});
    book.apply_depth_update(98, 105, {}, {});

    HIST_TEST_ASSERT(book.bids().size() == 2, "Should have 2 bid levels");
    HIST_TEST_ASSERT(book.asks().size() == 2, "Should have 2 ask levels");
    HIST_TEST_ASSERT(book.total_bid_quantity() == 300000, "Total bid qty 300000");

    // Apply update that:
    // 1. Modifies 6000000 from 100000 to 180000 (+80000)
    // 2. Deletes 5999000 (qty = 0)
    // 3. Adds new level 5998000 (qty = 50000)
    std::vector<LevelUpdate> bid_updates{
        {6000000, 180000},
        {5999000, 0},
        {5998000, 50000}
    };
    book.apply_depth_update(106, 110, bid_updates, {});

    HIST_TEST_ASSERT(book.bids().size() == 2, "Should still have 2 bid levels after 1 delete and 1 add");
    HIST_TEST_ASSERT(book.get_bid_qty(6000000) == 180000, "Level 6000000 quantity updated");
    HIST_TEST_ASSERT(book.get_bid_qty(5999000) == 0, "Level 5999000 deleted");
    HIST_TEST_ASSERT(book.get_bid_qty(5998000) == 50000, "Level 5998000 added");
    HIST_TEST_ASSERT(book.total_bid_quantity() == 230000, "Total bid quantity updated to 230000");

    HIST_TEST_ASSERT(book.check_invariants(), "Book invariants must pass");
    return true;
}

// 6. Best bid/ask and top-of-book tracking
bool test_top_of_book_tracking() {
    std::cout << "[RUN] test_top_of_book_tracking...\n";
    ReconstructedOrderBook book;
    book.apply_snapshot(100, {{6000000, 100000}}, {{6001000, 100000}});
    book.apply_depth_update(98, 105, {}, {});

    HIST_TEST_ASSERT(book.best_bid() == 6000000, "Best bid matches");
    HIST_TEST_ASSERT(book.best_ask() == 6001000, "Best ask matches");
    HIST_TEST_ASSERT(book.mid_price() == 6000500, "Mid price matches");

    // Insert closer bid at 6000500
    book.apply_depth_update(106, 110, {{6000500, 50000}}, {});
    HIST_TEST_ASSERT(book.best_bid() == 6000500, "Best bid updated to tighter price");

    // Delete best bid
    book.apply_depth_update(111, 115, {{6000500, 0}}, {});
    HIST_TEST_ASSERT(book.best_bid() == 6000000, "Best bid reverted to 6000000 after deletion");

    return true;
}

// 7. Aggressive order liquidity consumption across multiple levels
bool test_aggressive_liquidity_consumption() {
    std::cout << "[RUN] test_aggressive_liquidity_consumption...\n";
    ReconstructedOrderBook book;
    book.apply_snapshot(100, {{6000000, 100000}}, {{6001000, 100000}, {6002000, 200000}});
    book.apply_depth_update(98, 105, {}, {});

    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    Ledger ledger(fees);
    RiskLimits risk;
    risk.max_order_notional = 10000000000000LL;
    risk.max_order_qty = 10000000LL;
    risk.max_inventory = 10000000LL;
    RiskEngine risk_engine(risk);
    HistoricalExecutionAdapter adapter(book, ledger, risk_engine, fees);

    AccountId strat = "STRAT";
    ledger.fund_account(strat, 2000000000000LL, 100000LL);

    // Buy order for 150000: sweeps all 100000 @ 6001000 and 50000 @ 6002000
    NewOrderCommand cmd{1, 1001, Side::Buy, OrderType::Limit, 6002000, 150000, TimeInForce::IOC};
    std::vector<ExecutionReport> reports;
    std::vector<TradeSettlement> settlements;

    bool submitted = adapter.submit_order(1000, strat, cmd, 6002000, reports, settlements);
    HIST_TEST_ASSERT(submitted, "Aggressive order must be submitted and accepted");
    HIST_TEST_ASSERT(adapter.stats().aggressive_fills == 2, "Must produce 2 fills across 2 levels");
    HIST_TEST_ASSERT(adapter.stats().total_volume_traded == 150000, "Total traded volume must be 150000");

    // Second aggressive buy at 6001000 cannot match: level 6001000 was completely consumed!
    reports.clear();
    settlements.clear();
    NewOrderCommand cmd2{2, 1002, Side::Buy, OrderType::Limit, 6001000, 50000, TimeInForce::IOC};
    adapter.submit_order(1010, strat, cmd2, 6001000, reports, settlements);
    HIST_TEST_ASSERT(adapter.stats().aggressive_fills == 2, "No new fills: level was exhausted");

    return true;
}

// 8. Restoring consumed liquidity on subsequent depth updates
bool test_restoring_consumed_liquidity() {
    std::cout << "[RUN] test_restoring_consumed_liquidity...\n";
    ReconstructedOrderBook book;
    book.apply_snapshot(100, {}, {{6001000, 100000}});
    book.apply_depth_update(98, 105, {}, {});

    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    Ledger ledger(fees);
    RiskLimits risk;
    risk.max_order_notional = 10000000000000LL;
    risk.max_order_qty = 10000000LL;
    RiskEngine risk_engine(risk);
    HistoricalExecutionAdapter adapter(book, ledger, risk_engine, fees);

    AccountId strat = "STRAT";
    ledger.fund_account(strat, 2000000000000LL, 100000LL);

    // Consume entire 100000 @ 6001000
    NewOrderCommand cmd{1, 1001, Side::Buy, OrderType::Limit, 6001000, 100000, TimeInForce::IOC};
    std::vector<ExecutionReport> reports;
    std::vector<TradeSettlement> settlements;
    adapter.submit_order(1000, strat, cmd, 6001000, reports, settlements);
    HIST_TEST_ASSERT(adapter.stats().aggressive_fills == 1, "Level consumed");

    // New depth update arrives for 6001000 with 150000 lots
    NormalizedDepthUpdate upd{1050, 1700000000000ULL, 106, 110, {}, {{6001000, 150000}}};
    book.apply_depth_update(upd.first_update_id, upd.final_update_id, upd.bids, upd.asks);
    adapter.on_depth_update(upd);

    // Now another buy order can match again!
    reports.clear();
    settlements.clear();
    NewOrderCommand cmd2{2, 1002, Side::Buy, OrderType::Limit, 6001000, 50000, TimeInForce::IOC};
    adapter.submit_order(1100, strat, cmd2, 6001000, reports, settlements);
    HIST_TEST_ASSERT(adapter.stats().aggressive_fills == 2, "Liquidity was restored by depth update");

    return true;
}

// 9. Rejecting aggressive orders when feed status is INVALID
bool test_reject_when_feed_invalid() {
    std::cout << "[RUN] test_reject_when_feed_invalid...\n";
    ReconstructedOrderBook book;
    book.apply_snapshot(100, {{6000000, 100000}}, {{6001000, 100000}});
    book.apply_depth_update(98, 105, {}, {});
    book.mark_invalid("Connection lost");

    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    Ledger ledger(fees);
    RiskLimits risk;
    risk.max_order_notional = 10000000000000LL;
    RiskEngine risk_engine(risk);
    HistoricalExecutionAdapter adapter(book, ledger, risk_engine, fees);

    AccountId strat = "STRAT";
    ledger.fund_account(strat, 2000000000000LL, 100000LL);

    NewOrderCommand cmd{1, 1001, Side::Buy, OrderType::Limit, 6001000, 10000, TimeInForce::IOC};
    std::vector<ExecutionReport> reports;
    std::vector<TradeSettlement> settlements;

    bool submitted = adapter.submit_order(1000, strat, cmd, 6001000, reports, settlements);
    HIST_TEST_ASSERT(!submitted, "Order submission must fail when feed is invalid");
    HIST_TEST_ASSERT(reports.size() == 1 && reports[0].type == ExecutionReport::Type::Rejected, "Order must be rejected");

    return true;
}

// 10. Passive order queue-ahead depletion and fill capping by public trade volume
bool test_passive_queue_ahead_and_capping() {
    std::cout << "[RUN] test_passive_queue_ahead_and_capping...\n";
    ReconstructedOrderBook book;
    book.apply_snapshot(100, {{6000000, 100000}}, {{6001000, 100000}});
    book.apply_depth_update(98, 105, {}, {});

    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    Ledger ledger(fees);
    RiskLimits risk;
    risk.max_order_notional = 10000000000000LL;
    risk.max_order_qty = 10000000LL;
    RiskEngine risk_engine(risk);
    HistoricalExecutionAdapter adapter(book, ledger, risk_engine, fees);

    AccountId strat = "STRAT";
    ledger.fund_account(strat, 2000000000000LL, 100000LL);

    // Passive Buy order @ 6000000 for 50000 lots
    NewOrderCommand cmd{1, 1001, Side::Buy, OrderType::Limit, 6000000, 50000, TimeInForce::GTC};
    std::vector<ExecutionReport> reports;
    std::vector<TradeSettlement> settlements;
    adapter.submit_order(1000, strat, cmd, 6000000, reports, settlements);

    auto it = adapter.active_passive_orders().find(1001);
    HIST_TEST_ASSERT(it != adapter.active_passive_orders().end(), "Order must be active");
    HIST_TEST_ASSERT(it->second.queue_ahead == 100000, "Queue ahead initialized to observed resting book quantity (100000)");

    // Public Sell trade of 60000 at 6000000
    reports.clear();
    settlements.clear();
    NormalizedTrade t1{2000, 1700000000000ULL, 1, 6000000, 60000, Side::Sell};
    adapter.on_public_trade(t1, reports, settlements);

    it = adapter.active_passive_orders().find(1001);
    HIST_TEST_ASSERT(it->second.queue_ahead == 40000, "Queue ahead depleted from 100000 to 40000");
    HIST_TEST_ASSERT(adapter.stats().passive_fills == 0, "No fill before queue ahead is exhausted");

    // Second public trade of 70000 at 6000000:
    // 40000 exhausts remaining queue ahead; remaining 30000 fills passive order (capped at 30000!)
    reports.clear();
    settlements.clear();
    NormalizedTrade t2{3000, 1700000000100ULL, 2, 6000000, 70000, Side::Sell};
    adapter.on_public_trade(t2, reports, settlements);

    it = adapter.active_passive_orders().find(1001);
    HIST_TEST_ASSERT(it != adapter.active_passive_orders().end(), "Order partially filled");
    HIST_TEST_ASSERT(it->second.queue_ahead == 0, "Queue ahead exhausted");
    HIST_TEST_ASSERT(it->second.remaining_qty == 20000, "Filled 30000, remaining 20000");
    HIST_TEST_ASSERT(adapter.stats().total_volume_traded == 30000, "Trade volume capped at trade remainder");

    return true;
}

// 11. Self-trade prevention (STP) against resting strategy orders
bool test_self_trade_prevention_historical() {
    std::cout << "[RUN] test_self_trade_prevention_historical...\n";
    ReconstructedOrderBook book;
    book.apply_snapshot(100, {{6000000, 100000}}, {{6001000, 100000}});
    book.apply_depth_update(98, 105, {}, {});

    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    Ledger ledger(fees);
    RiskLimits risk;
    risk.max_order_notional = 10000000000000LL;
    risk.max_order_qty = 10000000LL;
    risk.allow_self_trade = false;
    RiskEngine risk_engine(risk);
    HistoricalExecutionAdapter adapter(book, ledger, risk_engine, fees);

    AccountId strat = "STRAT";
    ledger.fund_account(strat, 2000000000000LL, 100000LL);

    // Place passive sell order at 6001000
    NewOrderCommand sell_cmd{1, 1001, Side::Sell, OrderType::Limit, 6001000, 10000, TimeInForce::GTC};
    std::vector<ExecutionReport> reports;
    std::vector<TradeSettlement> settlements;
    adapter.submit_order(1000, strat, sell_cmd, 6001000, reports, settlements);
    HIST_TEST_ASSERT(adapter.active_passive_orders().count(1001) == 1, "Resting passive sell placed");

    // Strategy submits buy order crossing its own resting sell price
    reports.clear();
    settlements.clear();
    NewOrderCommand buy_cmd{2, 1002, Side::Buy, OrderType::Limit, 6001000, 5000, TimeInForce::IOC};
    bool sub = adapter.submit_order(1010, strat, buy_cmd, 6001000, reports, settlements);
    HIST_TEST_ASSERT(!sub, "Crossing buy order must be rejected for self-trade");
    HIST_TEST_ASSERT(reports.size() == 1 && reports[0].reason == "SELF_TRADE_PROHIBITED", "Reason must be SELF_TRADE_PROHIBITED");

    return true;
}

// 12. Double-entry ledger reconciliation and invariance
bool test_double_entry_reconciliation() {
    std::cout << "[RUN] test_double_entry_reconciliation...\n";
    ReconstructedOrderBook book;
    book.apply_snapshot(100, {{6000000, 100000}}, {{6001000, 100000}});
    book.apply_depth_update(98, 105, {}, {});

    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    Ledger ledger(fees);
    RiskLimits risk;
    risk.max_order_notional = 10000000000000LL;
    risk.max_order_qty = 10000000LL;
    RiskEngine risk_engine(risk);
    HistoricalExecutionAdapter adapter(book, ledger, risk_engine, fees);

    AccountId strat = "STRAT";
    int64_t strat_cash = 2000000000000LL;
    Quantity strat_inv = 100000LL;
    ledger.fund_account(strat, strat_cash, strat_inv);

    int64_t init_cash = ledger.get_total_cash();
    Quantity init_inv = ledger.get_total_inventory();

    // Perform multiple aggressive and passive trades
    std::vector<ExecutionReport> reports;
    std::vector<TradeSettlement> settlements;
    NewOrderCommand b1{1, 1001, Side::Buy, OrderType::Limit, 6001000, 50000, TimeInForce::IOC};
    adapter.submit_order(1000, strat, b1, 6001000, reports, settlements);

    NewOrderCommand p1{2, 1002, Side::Buy, OrderType::Limit, 6000000, 30000, TimeInForce::GTC};
    adapter.submit_order(2000, strat, p1, 6000000, reports, settlements);

    NormalizedTrade t1{3000, 1700000000000ULL, 1, 6000000, 150000, Side::Sell};
    adapter.on_public_trade(t1, reports, settlements);

    auto audit = ledger.verify_all_invariants(init_cash, init_inv);
    HIST_TEST_ASSERT(audit.ok, "All ledger invariants must pass after mixed executions");
    HIST_TEST_ASSERT(ledger.verify_cash_conservation(init_cash), "Total cash conserved");
    HIST_TEST_ASSERT(ledger.verify_inventory_conservation(init_inv), "Total inventory conserved");

    return true;
}

// 13. Replay Repeatability & Bitwise Canonical Hashing
bool test_replay_repeatability() {
    std::cout << "[RUN] test_replay_repeatability...\n";

    std::vector<NormalizedEvent> events;
    events.push_back(NormalizedSnapshot{1000000000, 100, {{6000000, 150000}}, {{6001000, 150000}}});
    events.push_back(NormalizedDepthUpdate{1000100000, 1700000000000ULL, 98, 105, {{6000000, 180000}}, {}});
    events.push_back(NormalizedTrade{1000200000, 1700000000100ULL, 1, 6000000, 50000, Side::Sell});

    HistoricalReplayConfig cfg;
    cfg.strategy_account_id = "MM";
    cfg.strategy_starting_cash = 100000000LL;
    cfg.strategy_starting_inventory = 100000;

    HistoricalReplayEngine engine1(cfg);
    engine1.load_events(events);
    auto summary1 = engine1.run();

    HistoricalReplayEngine engine2(cfg);
    engine2.load_events(events);
    auto summary2 = engine2.run();

    HIST_TEST_ASSERT(summary1.cash_conserved && summary2.cash_conserved, "Cash conserved in both");
    HIST_TEST_ASSERT(summary1.inventory_conserved && summary2.inventory_conserved, "Inventory conserved in both");

    std::string ev_hash1 = Sha256::hash_string(engine1.get_events_content());
    std::string ev_hash2 = Sha256::hash_string(engine2.get_events_content());
    HIST_TEST_ASSERT(ev_hash1 == ev_hash2, "Event content SHA-256 must match exactly across replays");

    std::string fills_hash1 = Sha256::hash_string(engine1.get_fills_content());
    std::string fills_hash2 = Sha256::hash_string(engine2.get_fills_content());
    HIST_TEST_ASSERT(fills_hash1 == fills_hash2, "Fills content SHA-256 must match exactly across replays");

    std::string state_hash1 = Sha256::hash_string(engine1.get_state_content());
    std::string state_hash2 = Sha256::hash_string(engine2.get_state_content());
    HIST_TEST_ASSERT(state_hash1 == state_hash2, "State content SHA-256 must match exactly across replays");

    return true;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << "  Running MarketLab Milestone 3 Historical Test Suite\n";
    std::cout << "=========================================================\n";

    assert(test_exact_decimal_parsing());
    assert(test_bridge_protocol());
    assert(test_sequence_gap_detection());
    assert(test_feed_status_transitions());
    assert(test_reconstructed_book_levels());
    assert(test_top_of_book_tracking());
    assert(test_aggressive_liquidity_consumption());
    assert(test_restoring_consumed_liquidity());
    assert(test_reject_when_feed_invalid());
    assert(test_passive_queue_ahead_and_capping());
    assert(test_self_trade_prevention_historical());
    assert(test_double_entry_reconciliation());
    assert(test_replay_repeatability());

    std::cout << "=========================================================\n";
    std::cout << "  ALL 13 HISTORICAL REPLAY TESTS PASSED SUCCESSFULLY!\n";
    std::cout << "=========================================================\n";
    return 0;
}
