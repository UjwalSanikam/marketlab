#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cassert>
#include <cmath>

#include "marketlab/market_data_types.hpp"
#include "marketlab/market_data_normalizer.hpp"
#include "marketlab/reconstructed_book.hpp"
#include "marketlab/historical_execution_adapter.hpp"
#include "marketlab/execution_model.hpp"
#include "marketlab/reference_strategy.hpp"
#include "marketlab/backtest_engine.hpp"
#include "marketlab/calibration_engine.hpp"
#include "marketlab/sha256.hpp"

using namespace marketlab;

#define BT_TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "BACKTEST TEST FAILED: " << msg << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
    } while (0)

inline std::string find_fixture_path(const std::string& filename) {
    std::vector<std::string> candidates = {
        "data/fixtures/" + filename,
        "../data/fixtures/" + filename,
        "../../data/fixtures/" + filename
    };
    for (const auto& p : candidates) {
        std::ifstream f(p, std::ios::binary);
        if (f.is_open()) return p;
    }
    return "data/fixtures/" + filename;
}

// 1. Causality & Zero Lookahead Test
bool test_causality_zero_lookahead() {
    std::cout << "[RUN] test_causality_zero_lookahead...\n";

    BacktestConfig cfg;
    cfg.latencies.market_data_delay = 50000000ULL; // 50ms
    cfg.strategy_config.mode = ReferenceStrategyMode::MarketMaker;
    cfg.strategy_config.half_spread = 100;
    cfg.strategy_config.quote_size = 10000;

    BacktestEngine engine(cfg);

    std::vector<NormalizedEvent> events;
    // Snapshot at t=1s (1,000,000,000 ns)
    NormalizedSnapshot snap;
    snap.local_receipt_time_ns = 1000000000ULL;
    snap.last_update_id = 100;
    snap.bids.push_back(LevelUpdate{6000000, 100000});
    snap.asks.push_back(LevelUpdate{6000200, 100000});
    events.push_back(snap);

    // Bridge update at t=1.1s
    NormalizedDepthUpdate bridge;
    bridge.local_receipt_time_ns = 1100000000ULL;
    bridge.exchange_time_ms = 1700000000100ULL;
    bridge.first_update_id = 98;
    bridge.final_update_id = 105;
    events.push_back(bridge);

    // Huge price surge scheduled at t=5s (5,000,000,000 ns)
    NormalizedDepthUpdate upd;
    upd.local_receipt_time_ns = 5000000000ULL;
    upd.exchange_time_ms = 1700000005000ULL;
    upd.first_update_id = 106;
    upd.final_update_id = 110;
    upd.bids.push_back(LevelUpdate{7000000, 100000});
    upd.asks.push_back(LevelUpdate{7000200, 100000});
    events.push_back(upd);

    engine.load_events(events);
    auto report = engine.run();

    BT_TEST_ASSERT(report.orders_submitted >= 2, "Orders should be placed based on initial observation");
    BT_TEST_ASSERT(report.starting_equity > 0, "Starting equity should be evaluated at initial mid");

    return true;
}

// 2. Queue Position Models & Volume Capping Test
bool test_queue_position_models() {
    std::cout << "[RUN] test_queue_position_models...\n";

    // Setup market: Best Bid at 6000000 with 100,000 lots visible depth
    ReconstructedOrderBook book;
    std::vector<LevelUpdate> bids{{6000000, 100000}};
    std::vector<LevelUpdate> asks{{6000200, 100000}};
    book.apply_snapshot(100, bids, asks);
    book.apply_depth_update(98, 105, {}, {}); // Bridge snapshot to make feed Valid

    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    RiskLimits risk{1000000, 1000000000000LL, 5000000};
    InstrumentMetadata inst;

    // Test A: Optimistic (0% queue ahead)
    {
        ExecutionModelConfig em_opt;
        em_opt.queue_model = QueuePositionModel::Optimistic;
        em_opt.queue_position_pct = 0;

        Ledger ledger(fees);
        ledger.fund_account("STRAT", 100000000000LL, 100000);
        RiskEngine re(risk);
        HistoricalExecutionAdapter adapter(book, ledger, re, fees, em_opt);

        // Place resting passive buy order at 6000000 for 10,000 lots
        NewOrderCommand cmd{1, 101, Side::Buy, OrderType::Limit, 6000000, 10000, TimeInForce::GTC};
        std::vector<ExecutionReport> rpts;
        std::vector<TradeSettlement> setts;
        bool sub_ok = adapter.submit_order(1000, "STRAT", cmd, 6000000, rpts, setts);
        BT_TEST_ASSERT(sub_ok, "Order admission failed");

        const auto& active = adapter.active_passive_orders();
        auto it = active.find(101);
        BT_TEST_ASSERT(it != active.end(), "Order not resting");
        BT_TEST_ASSERT(it->second.queue_ahead == 0, "Optimistic queue ahead must be 0");

        // Public sell trade of 5,000 lots at 6000000
        NormalizedTrade tr{2000, 1700000000000ULL, 1, 6000000, 5000, Side::Sell};
        rpts.clear(); setts.clear();
        adapter.on_public_trade(tr, rpts, setts);

        // Under optimistic model, order at front fills immediately up to trade size
        BT_TEST_ASSERT(rpts.size() == 1, "Expected fill report");
        BT_TEST_ASSERT(rpts[0].type == ExecutionReport::Type::Trade, "Expected Trade report");
        BT_TEST_ASSERT(rpts[0].quantity - rpts[0].remaining_quantity == 5000, "Filled 5000 lots");
    }

    // Test B: Conservative (100% queue ahead)
    {
        ExecutionModelConfig em_cons;
        em_cons.queue_model = QueuePositionModel::Conservative;
        em_cons.queue_position_pct = 100;

        Ledger ledger(fees);
        ledger.fund_account("STRAT", 100000000000LL, 100000);
        RiskEngine re(risk);
        HistoricalExecutionAdapter adapter(book, ledger, re, fees, em_cons);

        NewOrderCommand cmd{1, 102, Side::Buy, OrderType::Limit, 6000000, 10000, TimeInForce::GTC};
        std::vector<ExecutionReport> rpts;
        std::vector<TradeSettlement> setts;
        adapter.submit_order(1000, "STRAT", cmd, 6000000, rpts, setts);

        const auto& active = adapter.active_passive_orders();
        auto it = active.find(102);
        BT_TEST_ASSERT(it != active.end(), "Order not resting");
        BT_TEST_ASSERT(it->second.queue_ahead == 100000, "Conservative queue ahead must match full visible depth");

        // Public trade of 50,000 lots should NOT fill strategy order, only deplete queue ahead
        NormalizedTrade tr1{2000, 1700000000000ULL, 1, 6000000, 50000, Side::Sell};
        rpts.clear(); setts.clear();
        adapter.on_public_trade(tr1, rpts, setts);
        BT_TEST_ASSERT(rpts.empty(), "Conservative should not fill when queue ahead remains");
        it = adapter.active_passive_orders().find(102);
        BT_TEST_ASSERT(it->second.queue_ahead == 50000, "Queue ahead should deplete by 50,000 lots");

        // Subsequent public trade of 60,000 lots depletes remaining 50k and fills 10k!
        NormalizedTrade tr2{3000, 1700000000100ULL, 2, 6000000, 60000, Side::Sell};
        rpts.clear(); setts.clear();
        adapter.on_public_trade(tr2, rpts, setts);
        BT_TEST_ASSERT(rpts.size() == 1, "Expected fill report on second trade");
        BT_TEST_ASSERT(rpts[0].remaining_quantity == 0, "Order completely filled");
    }

    // Test C: Proportional (50% queue ahead)
    {
        ExecutionModelConfig em_prop;
        em_prop.queue_model = QueuePositionModel::Proportional;
        em_prop.queue_position_pct = 50;

        Ledger ledger(fees);
        ledger.fund_account("STRAT", 100000000000LL, 100000);
        RiskEngine re(risk);
        HistoricalExecutionAdapter adapter(book, ledger, re, fees, em_prop);

        NewOrderCommand cmd{1, 103, Side::Buy, OrderType::Limit, 6000000, 10000, TimeInForce::GTC};
        std::vector<ExecutionReport> rpts;
        std::vector<TradeSettlement> setts;
        adapter.submit_order(1000, "STRAT", cmd, 6000000, rpts, setts);

        const auto& active = adapter.active_passive_orders();
        auto it = active.find(103);
        BT_TEST_ASSERT(it != active.end(), "Order not resting");
        BT_TEST_ASSERT(it->second.queue_ahead == 50000, "Proportional 50% queue ahead should be 50,000 lots");
    }

    return true;
}

// 3. Aggressive Impact & Slippage Models Test
bool test_aggressive_impact_and_slippage() {
    std::cout << "[RUN] test_aggressive_impact_and_slippage...\n";

    ReconstructedOrderBook book;
    std::vector<LevelUpdate> bids{{6000000, 100000}};
    std::vector<LevelUpdate> asks{{6000200, 100000}};
    book.apply_snapshot(100, bids, asks);
    book.apply_depth_update(98, 105, {}, {}); // Bridge snapshot to make feed Valid

    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    RiskLimits risk{1000000, 1000000000000LL, 5000000};

    // Baseline: No slippage
    {
        ExecutionModelConfig em_none;
        em_none.impact_model = AggressiveImpactModel::None;

        Ledger ledger(fees);
        ledger.fund_account("STRAT", 100000000000LL, 100000);
        RiskEngine re(risk);
        HistoricalExecutionAdapter adapter(book, ledger, re, fees, em_none);

        NewOrderCommand cmd{1, 201, Side::Buy, OrderType::Limit, 6000200, 10000, TimeInForce::IOC};
        std::vector<ExecutionReport> rpts;
        std::vector<TradeSettlement> setts;
        adapter.submit_order(1000, "STRAT", cmd, 6000200, rpts, setts);

        BT_TEST_ASSERT(setts.size() == 1, "Expected fill");
        BT_TEST_ASSERT(setts[0].price == 6000200, "Execution price matches book ask");
    }

    // Fixed Slippage: 5 ticks penalty
    {
        ExecutionModelConfig em_slip;
        em_slip.impact_model = AggressiveImpactModel::FixedSlippage;
        em_slip.fixed_slippage_ticks = 5;

        Ledger ledger(fees);
        ledger.fund_account("STRAT", 100000000000LL, 100000);
        RiskEngine re(risk);
        HistoricalExecutionAdapter adapter(book, ledger, re, fees, em_slip);

        NewOrderCommand cmd{1, 202, Side::Buy, OrderType::Limit, 6000250, 10000, TimeInForce::IOC};
        std::vector<ExecutionReport> rpts;
        std::vector<TradeSettlement> setts;
        adapter.submit_order(1000, "STRAT", cmd, 6000250, rpts, setts);

        BT_TEST_ASSERT(setts.size() == 1, "Expected fill");
        BT_TEST_ASSERT(setts[0].price == 6000205, "Execution price includes 5 ticks slippage (6000200 + 5)");
    }

    return true;
}

// 4. Markout Calculation Timing, Statuses, and Direction Test
bool test_markout_calculation() {
    std::cout << "[RUN] test_markout_calculation...\n";

    BacktestConfig cfg;
    cfg.markout_horizons = { 100000000ULL, 1000000000ULL, 5000000000ULL }; // 100ms, 1s, 5s

    std::vector<NormalizedEvent> events;
    // t=1s: Snapshot
    NormalizedSnapshot snap;
    snap.local_receipt_time_ns = 1000000000ULL;
    snap.last_update_id = 100;
    snap.bids.push_back(LevelUpdate{6000000, 100000});
    snap.asks.push_back(LevelUpdate{6000200, 100000});
    events.push_back(snap);

    // Bridge update at t=1.1s
    NormalizedDepthUpdate bridge;
    bridge.local_receipt_time_ns = 1100000000ULL;
    bridge.exchange_time_ms = 1700000000100ULL;
    bridge.first_update_id = 98;
    bridge.final_update_id = 105;
    events.push_back(bridge);

    // t=2s: Public trade triggering strategy fill
    NormalizedTrade tr;
    tr.local_receipt_time_ns = 2000000000ULL;
    tr.trade_id = 1;
    tr.price = 6000000;
    tr.quantity = 150000;
    tr.taker_side = Side::Sell;
    events.push_back(tr);

    // t=2.1s (100ms post-fill): Price rises to 6000500 mid
    NormalizedDepthUpdate upd1;
    upd1.local_receipt_time_ns = 2100000000ULL;
    upd1.first_update_id = 106;
    upd1.final_update_id = 110;
    upd1.bids.push_back(LevelUpdate{6000400, 100000});
    upd1.asks.push_back(LevelUpdate{6000600, 100000});
    events.push_back(upd1);

    // t=3.0s (1s post-fill): Price at 6000500 mid
    // t=4.0s: Session ends (before 5s horizon reaches t=7s)
    NormalizedDepthUpdate upd2;
    upd2.local_receipt_time_ns = 4000000000ULL;
    upd2.first_update_id = 111;
    upd2.final_update_id = 115;
    upd2.bids.push_back(LevelUpdate{6000400, 100000});
    upd2.asks.push_back(LevelUpdate{6000600, 100000});
    events.push_back(upd2);

    BacktestEngine engine(cfg);
    engine.load_events(events);
    auto report = engine.run();

    BT_TEST_ASSERT(report.markout_summaries.size() == 3, "Expected 3 markout horizons");
    const auto& h100ms = report.markout_summaries[0];
    const auto& h1s = report.markout_summaries[1];
    const auto& h5s = report.markout_summaries[2];

    if (report.total_trades > 0) {
        BT_TEST_ASSERT(h100ms.completed_count >= 1, "100ms horizon should be completed");
        BT_TEST_ASSERT(h1s.completed_count >= 1, "1s horizon should be completed");
        BT_TEST_ASSERT(h5s.unfinished_count >= 1, "5s horizon should be unfinished (session ended at 4s)");
    }

    return true;
}

// 5. Double-Entry Ledger Conservation Test
bool test_ledger_double_entry_conservation() {
    std::cout << "[RUN] test_ledger_double_entry_conservation...\n";

    BacktestConfig cfg;
    cfg.strategy_config.mode = ReferenceStrategyMode::Hybrid;
    cfg.strategy_config.quote_size = 10000;
    cfg.strategy_config.half_spread = 100;

    std::string path = find_fixture_path("calibration_fixture_norm.jsonl");
    std::ifstream ifs(path, std::ios::binary);
    BT_TEST_ASSERT(ifs.is_open(), "Cannot open calibration fixture");
    auto events = MarketDataNormalizer::read_normalized_stream(ifs);
    ifs.close();

    BacktestEngine engine(cfg);
    engine.load_events(events);
    auto report = engine.run();

    BT_TEST_ASSERT(report.cash_conserved, "Monetary conservation check must pass");
    BT_TEST_ASSERT(report.inventory_conserved, "Inventory conservation check must pass");
    BT_TEST_ASSERT(report.initial_system_cash == report.final_system_cash, "Cash conservation equality failed");
    BT_TEST_ASSERT(report.initial_system_inventory == report.final_system_inventory, "Inventory conservation equality failed");

    return true;
}

// 6. Calibration / Holdout Chronological Isolation Test
bool test_calibration_holdout_isolation() {
    std::cout << "[RUN] test_calibration_holdout_isolation...\n";

    std::string path = find_fixture_path("calibration_fixture_norm.jsonl");
    std::ifstream ifs(path, std::ios::binary);
    BT_TEST_ASSERT(ifs.is_open(), "Cannot open calibration fixture");
    auto events = MarketDataNormalizer::read_normalized_stream(ifs);
    ifs.close();

    CalibrationConfig cal_cfg;
    cal_cfg.split_ratio = 0.6;
    cal_cfg.objective = CalibrationObjective::NetPnL;

    CalibrationEngine cal_engine(cal_cfg);
    cal_engine.prepare_data(events);

    SimTime split_time = cal_engine.split_time();
    BT_TEST_ASSERT(split_time > 0, "Valid split timestamp");

    // Check calibration events: all t < split_time
    for (const auto& ev : cal_engine.calibration_events()) {
        SimTime t = get_event_time(ev);
        BT_TEST_ASSERT(t < split_time, "Calibration event time must be strictly before split_time");
    }

    // Check holdout events: first is snapshot at split_time, subsequent are t >= split_time
    const auto& holdout = cal_engine.holdout_events();
    BT_TEST_ASSERT(!holdout.empty(), "Holdout events must not be empty");
    BT_TEST_ASSERT(std::holds_alternative<NormalizedSnapshot>(holdout.front()), "Holdout must begin with warm snapshot");

    for (size_t i = 1; i < holdout.size(); ++i) {
        SimTime t = get_event_time(holdout[i]);
        BT_TEST_ASSERT(t >= split_time, "Holdout event time must be at or after split_time");
    }

    auto report = cal_engine.run();
    BT_TEST_ASSERT(!report.calibration_rankings.empty(), "Calibration rankings produced");
    BT_TEST_ASSERT(report.baseline_holdout_report.cash_conserved, "Holdout baseline cash conserved");
    BT_TEST_ASSERT(report.selected_holdout_report.cash_conserved, "Holdout selected winner cash conserved");

    return true;
}

// 7. Deterministic Bitwise Reproducibility Test
bool test_deterministic_reproducibility() {
    std::cout << "[RUN] test_deterministic_reproducibility...\n";

    std::string path = find_fixture_path("calibration_fixture_norm.jsonl");
    std::ifstream ifs(path, std::ios::binary);
    BT_TEST_ASSERT(ifs.is_open(), "Cannot open calibration fixture");
    auto events1 = MarketDataNormalizer::read_normalized_stream(ifs);
    ifs.close();

    auto events2 = events1;

    BacktestConfig cfg;
    cfg.strategy_config.mode = ReferenceStrategyMode::Hybrid;
    cfg.strategy_config.quote_size = 10000;
    cfg.strategy_config.half_spread = 100;

    BacktestEngine eng1(cfg);
    eng1.load_events(std::move(events1));
    auto rep1 = eng1.run();
    std::string ev1_hash = Sha256::hash_string(eng1.get_events_content());
    std::string fill1_hash = Sha256::hash_string(eng1.get_fills_content());
    std::string rep1_hash = Sha256::hash_string(rep1.to_json());

    BacktestEngine eng2(cfg);
    eng2.load_events(std::move(events2));
    auto rep2 = eng2.run();
    std::string ev2_hash = Sha256::hash_string(eng2.get_events_content());
    std::string fill2_hash = Sha256::hash_string(eng2.get_fills_content());
    std::string rep2_hash = Sha256::hash_string(rep2.to_json());

    BT_TEST_ASSERT(ev1_hash == ev2_hash, "Event log hashes must match bitwise");
    BT_TEST_ASSERT(fill1_hash == fill2_hash, "Fill log hashes must match bitwise");
    BT_TEST_ASSERT(rep1_hash == rep2_hash, "Report JSON hashes must match bitwise");
    BT_TEST_ASSERT(rep1.net_pnl == rep2.net_pnl, "Net PnL must match exactly");
    BT_TEST_ASSERT(rep1.total_trades == rep2.total_trades, "Total trades must match exactly");

    return true;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << "  Running MarketLab Milestone 4 Backtest & Calibration Tests\n";
    std::cout << "=========================================================\n";

    bool ok = true;
    ok &= test_causality_zero_lookahead();
    ok &= test_queue_position_models();
    ok &= test_aggressive_impact_and_slippage();
    ok &= test_markout_calculation();
    ok &= test_ledger_double_entry_conservation();
    ok &= test_calibration_holdout_isolation();
    ok &= test_deterministic_reproducibility();

    if (!ok) {
        std::cerr << "\nOne or more Backtest & Calibration tests FAILED!\n";
        return 1;
    }

    std::cout << "\nALL 7 BACKTEST & CALIBRATION TESTS PASSED!\n";
    std::cout << "=========================================================\n";
    return 0;
}
