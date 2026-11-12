#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <iomanip>

#include "marketlab/types.hpp"
#include "marketlab/order_book.hpp"
#include "marketlab/reference_matcher.hpp"
#include "marketlab/workload_generator.hpp"
#include "marketlab/serializer.hpp"
#include "marketlab/invariants.hpp"
#include "marketlab/sha256.hpp"
#include "marketlab/simulation_engine.hpp"
#include "marketlab/market_data_types.hpp"
#include "marketlab/market_data_normalizer.hpp"
#include "marketlab/reconstructed_book.hpp"
#include "marketlab/historical_execution_adapter.hpp"
#include "marketlab/historical_replay_engine.hpp"
#include "marketlab/execution_model.hpp"
#include "marketlab/backtest_report.hpp"
#include "marketlab/reference_strategy.hpp"
#include "marketlab/backtest_engine.hpp"
#include "marketlab/calibration_engine.hpp"

using namespace marketlab;

void print_help() {
    std::cout << "MarketLab CLI - Deterministic Matching Engine & Research Platform\n\n";
    std::cout << "Usage:\n";
    std::cout << "  marketlab demo\n";
    std::cout << "      Run built-in demonstration showcasing FIFO priority, partial fills, cancel, and overflow guards.\n\n";
    std::cout << "  marketlab generate [options]\n";
    std::cout << "      Generate a synthetic deterministic command stream.\n";
    std::cout << "      Options:\n";
    std::cout << "        --seed <num>       RNG seed (default: 42)\n";
    std::cout << "        --count <num>      Number of commands (default: 1000)\n";
    std::cout << "        --output <file>    Output file path (default: stdout)\n";
    std::cout << "        --anomalies        Inject anomalies (duplicates, invalid prices/qtys)\n\n";
    std::cout << "  marketlab replay [options]\n";
    std::cout << "      Replay a saved command stream deterministically.\n";
    std::cout << "      Options:\n";
    std::cout << "        --input <file>     Input commands file path (required)\n";
    std::cout << "        --events-out <f>   Export events to file\n";
    std::cout << "        --state-out <f>    Export final book state as JSON\n";
    std::cout << "        --ascii            Print final ASCII book state to stdout\n";
    std::cout << "        --verify           Run full invariant audit\n\n";
    std::cout << "  marketlab test-differential [options]\n";
    std::cout << "      Run differential tests against naive reference matcher.\n";
    std::cout << "      Options:\n";
    std::cout << "        --seed <num>       RNG seed (default: 42)\n";
    std::cout << "        --count <num>      Number of commands (default: 5000)\n\n";
    std::cout << "  marketlab benchmark [options]\n";
    std::cout << "      Run in-memory service-time benchmark.\n";
    std::cout << "      Options:\n";
    std::cout << "        --commands <num>   Command count (default: 500000)\n";
    std::cout << "        --seed <num>       RNG seed (default: 42)\n\n";
    std::cout << "  marketlab sim [options]\n";
    std::cout << "      Run a deterministic simulated trading session with strategy execution, risk checks, and portfolio accounting.\n";
    std::cout << "      Options:\n";
    std::cout << "        --config <file>    Simulation config JSON file (default: built-in demo config)\n";
    std::cout << "        --events-out <f>   Export canonical events log (JSONL)\n";
    std::cout << "        --fills-out <f>    Export canonical fills/trades log (JSONL)\n";
    std::cout << "        --state-out <f>    Export final portfolio & book state (JSON)\n";
    std::cout << "        --manifest-out <f> Export simulation run manifest (JSON)\n";
    std::cout << "        --verify           Enforce cash and inventory conservation checks (exit 1 if failed)\n";
    std::cout << "        --demo             Run interactive verbose demo session\n\n";
    std::cout << "  marketlab reconstruct [options]\n";
    std::cout << "      Normalize a raw market-data capture stream into canonical integer ticks/lots.\n";
    std::cout << "      Options:\n";
    std::cout << "        --input <file>     Raw JSONL capture file (required)\n";
    std::cout << "        --output <file>    Normalized JSONL output file (required)\n\n";
    std::cout << "  marketlab replay-hist [options]\n";
    std::cout << "      Replay a normalized market-data stream through historical execution and portfolio accounting.\n";
    std::cout << "      Options:\n";
    std::cout << "        --input <file>     Normalized JSONL stream (required)\n";
    std::cout << "        --events-out <f>   Export normalized events log\n";
    std::cout << "        --fills-out <f>    Export fills/execution report log\n";
    std::cout << "        --state-out <f>    Export final book & portfolio state\n";
    std::cout << "        --manifest-out <f> Export deterministic replay manifest\n";
    std::cout << "        --verify           Enforce cash/inventory conservation (exit 1 if failed)\n\n";
    std::cout << "  marketlab demo-hist\n";
    std::cout << "      Run interactive historical execution demo (reconstruction, aggressive IOC, passive queue-ahead).\n\n";
    std::cout << "  marketlab backtest [options]\n";
    std::cout << "      Run event-driven strategy backtesting against normalized historical market data.\n";
    std::cout << "      Options:\n";
    std::cout << "        --input <file>        Normalized JSONL stream (required)\n";
    std::cout << "        --strategy-mode <m>   Strategy mode: MARKET_MAKER, TAKER, HYBRID (default: HYBRID)\n";
    std::cout << "        --quote-size <lots>   Quote size per order (default: 10000)\n";
    std::cout << "        --half-spread <ticks> Half spread in price ticks (default: 200)\n";
    std::cout << "        --skew <ticks>        Reservation price skew per lot (default: 1)\n";
    std::cout << "        --queue-model <m>     Passive queue model: CONSERVATIVE, OPTIMISTIC, PROPORTIONAL\n";
    std::cout << "        --queue-pct <0-100>   Queue ahead percentage for proportional model (default: 100)\n";
    std::cout << "        --impact-model <m>    Aggressive impact model: NONE, FIXED_SLIPPAGE, LINEAR_IMPACT\n";
    std::cout << "        --slippage-ticks <t>  Fixed slippage ticks paid on aggressive fills\n";
    std::cout << "        --impact-ticks <t>    Linear impact scaling ticks per lot\n";
    std::cout << "        --events-out <f>      Export canonical events log\n";
    std::cout << "        --fills-out <f>       Export canonical fills log\n";
    std::cout << "        --state-out <f>       Export final book & portfolio state\n";
    std::cout << "        --report-out <f>      Export backtest summary report JSON\n";
    std::cout << "        --manifest-out <f>    Export deterministic backtest manifest\n";
    std::cout << "        --verify              Enforce cash/inventory conservation (exit 1 if failed)\n\n";
    std::cout << "  marketlab calibrate [options]\n";
    std::cout << "      Calibrate execution and strategy parameters using chronological train/holdout split.\n";
    std::cout << "      Options:\n";
    std::cout << "        --input <file>        Normalized JSONL stream (required)\n";
    std::cout << "        --split <ratio>       Chronological split ratio (default: 0.6)\n";
    std::cout << "        --objective <obj>     Objective: net_pnl, gross_pnl, fill_rate, min_drawdown\n";
    std::cout << "        --report-out <f>      Export calibration report JSON\n";
    std::cout << "        --verify              Enforce conservation and audit holdout consistency\n\n";
    std::cout << "  marketlab capture [options]\n";
    std::cout << "      Run live Binance Spot market-data collector.\n";
    std::cout << "      Options:\n";
    std::cout << "        --symbol <sym>     Trading pair symbol (default: BTCUSDT)\n";
    std::cout << "        --output <file>    Output file for raw JSONL capture\n";
    std::cout << "        --duration <sec>   Capture duration limit in seconds\n";
    std::cout << "        --limit <num>      Maximum messages to capture\n";
}

int run_demo() {
    std::cout << "=========================================================\n";
    std::cout << "  MarketLab Built-In Interactive Demonstration\n";
    std::cout << "=========================================================\n\n";

    OrderBook book;
    std::vector<Event> events;

    auto print_and_process = [&](const std::string& desc, const Command& cmd) {
        std::cout << ">>> " << desc << "\n";
        std::cout << "Command: " << Serializer::to_json(cmd) << "\n";
        events.clear();
        book.process(cmd, events);
        std::cout << "Events emitted (" << events.size() << "):\n";
        for (const auto& ev : events) {
            std::cout << "  " << Serializer::to_json(ev) << "\n";
        }
        std::cout << "\n";
    };

    std::cout << "--- 1. Resting Orders & Book Construction ---\n";
    print_and_process("Add Resting Buy Order A @ 100 qty 10 (GTC)",
                      NewOrderCommand{1, 1001, Side::Buy, OrderType::Limit, 100, 10, TimeInForce::GTC});

    print_and_process("Add Resting Buy Order B @ 100 qty 20 (GTC, same price, later time)",
                      NewOrderCommand{2, 1002, Side::Buy, OrderType::Limit, 100, 20, TimeInForce::GTC});

    print_and_process("Add Resting Buy Order C @ 99 qty 30 (GTC, lower price)",
                      NewOrderCommand{3, 1003, Side::Buy, OrderType::Limit, 99, 30, TimeInForce::GTC});

    print_and_process("Add Resting Sell Order D @ 102 qty 15 (GTC)",
                      NewOrderCommand{4, 1004, Side::Sell, OrderType::Limit, 102, 15, TimeInForce::GTC});

    print_and_process("Add Resting Sell Order E @ 103 qty 25 (GTC)",
                      NewOrderCommand{5, 1005, Side::Sell, OrderType::Limit, 103, 25, TimeInForce::GTC});

    std::cout << "Current Book State:\n" << Serializer::to_ascii_book(book.get_snapshot()) << "\n";

    std::cout << "--- 2. Price-Time (FIFO) Priority Execution ---\n";
    std::cout << "Incoming Sell order for 15 @ 100 matches against 1001 (fills 10) then 1002 (fills 5).\n";
    print_and_process("Incoming Sell Order F @ 100 qty 15 (IOC)",
                      NewOrderCommand{6, 1006, Side::Sell, OrderType::Limit, 100, 15, TimeInForce::IOC});

    std::cout << "Current Book State (Notice Order A fully filled, Order B partially filled with 15 remaining):\n";
    std::cout << Serializer::to_ascii_book(book.get_snapshot()) << "\n";

    std::cout << "--- 3. Multi-Level Sweep & Partial Fill ---\n";
    std::cout << "Incoming Market Buy for 30 shares: sweeps 102 (15 shares) and 103 (15 shares).\n";
    print_and_process("Incoming Market Buy Order G qty 30",
                      NewOrderCommand{7, 1007, Side::Buy, OrderType::Market, 0, 30, TimeInForce::IOC});

    std::cout << "Current Book State (Level 102 deleted, Level 103 has 10 remaining):\n";
    std::cout << Serializer::to_ascii_book(book.get_snapshot()) << "\n";

    std::cout << "--- 4. Cancellation by Order ID ---\n";
    std::cout << "Canceling Order B (id 1002, remaining qty 15).\n";
    print_and_process("Cancel Order 1002", CancelOrderCommand{8, 1002});

    std::cout << "Current Book State (Level 100 now empty and removed):\n";
    std::cout << Serializer::to_ascii_book(book.get_snapshot()) << "\n";

    std::cout << "--- 5. Deterministic Rejection & Overflow Guards ---\n";
    print_and_process("Attempt to cancel already canceled Order 1002", CancelOrderCommand{9, 1002});
    print_and_process("Submit duplicate Order ID 1001",
                      NewOrderCommand{10, 1001, Side::Buy, OrderType::Limit, 100, 5, TimeInForce::GTC});

    // Overflow guard demo
    print_and_process("Submit massive Buy Order near max int64 (fits within capacity)",
                      NewOrderCommand{11, 2001, Side::Buy, OrderType::Limit, 95, 9223372036854775000LL, TimeInForce::GTC});
    print_and_process("Submit Buy Order that would cause aggregate overflow (must be rejected upfront)",
                      NewOrderCommand{12, 2002, Side::Buy, OrderType::Limit, 95, 1000000000LL, TimeInForce::GTC});

    auto inv = InvariantChecker::check_book_invariants(book);
    std::cout << "Final Invariant Audit: " << (inv.ok ? "ALL INVARIANTS PASS" : ("FAILED: " + inv.message)) << "\n";
    std::cout << "=========================================================\n";
    return 0;
}

int run_generate(int argc, char* argv[]) {
    WorkloadConfig config;
    std::string out_path;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            config.seed = std::stoull(argv[++i]);
        } else if (arg == "--count" && i + 1 < argc) {
            config.count = std::stoull(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            out_path = argv[++i];
        } else if (arg == "--anomalies") {
            config.inject_anomalies = true;
        }
    }

    WorkloadGenerator gen(config);
    auto commands = gen.generate();

    if (!out_path.empty()) {
        std::ofstream ofs(out_path, std::ios::binary);
        if (!ofs.is_open()) {
            std::cerr << "Error: cannot open output file: " << out_path << "\n";
            return 1;
        }
        Serializer::write_commands(ofs, commands);
        ofs.flush();
        std::cout << "Successfully generated " << commands.size()
                  << " commands (seed=" << config.seed << ") to " << out_path << "\n";
    } else {
        Serializer::write_commands(std::cout, commands);
    }
    return 0;
}

int run_replay(int argc, char* argv[]) {
    std::string in_path;
    std::string events_out_path;
    std::string state_out_path;
    bool ascii_view = false;
    bool verify = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            in_path = argv[++i];
        } else if (arg == "--events-out" && i + 1 < argc) {
            events_out_path = argv[++i];
        } else if (arg == "--state-out" && i + 1 < argc) {
            state_out_path = argv[++i];
        } else if (arg == "--ascii") {
            ascii_view = true;
        } else if (arg == "--verify") {
            verify = true;
        }
    }

    if (in_path.empty()) {
        std::cerr << "Error: --input <file> is required for replay.\n";
        return 1;
    }

    std::ifstream ifs(in_path);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open input file: " << in_path << "\n";
        return 1;
    }

    auto parse_res = Serializer::read_commands_checked(ifs);
    if (!parse_res.success) {
        std::cerr << "Error: Malformed command on line " << parse_res.error->line_number
                  << ": " << parse_res.error->message << "\n";
        std::cerr << "Offending line: \"" << parse_res.error->line_content << "\"\n";
        return 2;
    }

    const auto& commands = parse_res.commands;
    std::cout << "Loaded " << commands.size() << " commands from " << in_path << "\n";

    OrderBook book;
    std::vector<Event> all_events;
    std::vector<Event> step_events;
    InvariantChecker::LifecycleAuditor auditor;

    std::string events_stream_content;

    auto t0 = std::chrono::steady_clock::now();
    for (const auto& cmd : commands) {
        step_events.clear();
        book.process(cmd, step_events);

        for (const auto& ev : step_events) {
            std::string ev_json = Serializer::to_json(ev);
            events_stream_content += ev_json + "\n";
            if (!events_out_path.empty()) {
                all_events.push_back(ev);
            }
            if (verify) {
                auditor.record_event(ev);
            }
        }

        if (verify) {
            auto inv = InvariantChecker::check_book_invariants(book);
            if (!inv.ok) {
                std::cerr << "Invariant violation during replay on command seq " << get_seq_num(cmd)
                          << ": " << inv.message << "\n";
                return 1;
            }
        }
    }
    auto t1 = std::chrono::steady_clock::now();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "Replay completed in " << elapsed_us << " us ("
              << (commands.size() * 1000000.0 / (elapsed_us > 0 ? elapsed_us : 1)) << " cmds/sec)\n";

    if (!events_out_path.empty()) {
        std::ofstream eofs(events_out_path, std::ios::binary);
        if (!eofs.is_open()) {
            std::cerr << "Error: cannot open events output file: " << events_out_path << "\n";
            return 1;
        }
        Serializer::write_events(eofs, all_events);
        eofs.flush();
        std::cout << "Exported " << all_events.size() << " events to " << events_out_path << "\n";
    }

    auto snapshot = book.get_snapshot();
    std::string state_json = Serializer::to_json(snapshot);
    std::string state_content = state_json + "\n";

    if (!state_out_path.empty()) {
        std::ofstream sofs(state_out_path, std::ios::binary);
        if (!sofs.is_open()) {
            std::cerr << "Error: cannot open state output file: " << state_out_path << "\n";
            return 1;
        }
        sofs.write(state_content.data(), state_content.size());
        sofs.flush();
        std::cout << "Exported final state to " << state_out_path << "\n";
    }

    // Compute canonical SHA-256 hashes of exact exported bytes
    std::string events_hash;
    if (!events_out_path.empty()) {
        auto h = Sha256::hash_file(events_out_path);
        events_hash = h ? *h : Sha256::hash_string(events_stream_content);
    } else {
        events_hash = Sha256::hash_string(events_stream_content);
    }

    std::string state_hash;
    if (!state_out_path.empty()) {
        auto h = Sha256::hash_file(state_out_path);
        state_hash = h ? *h : Sha256::hash_string(state_content);
    } else {
        state_hash = Sha256::hash_string(state_content);
    }

    std::cout << "Canonical Events SHA-256: " << events_hash << "\n";
    std::cout << "Canonical State  SHA-256: " << state_hash << "\n";

    if (ascii_view) {
        std::cout << "\n" << Serializer::to_ascii_book(snapshot) << "\n";
    }

    if (verify) {
        auto rec = auditor.verify_reconciliation(book);
        if (!rec.ok) {
            std::cerr << "Reconciliation failure: " << rec.message << "\n";
            return 1;
        }
        std::cout << "Audit verification: ALL INVARIANTS AND RECONCILIATIONS PASSED!\n";
    }

    return 0;
}

int run_test_differential(int argc, char* argv[]) {
    uint64_t seed = 42;
    size_t count = 5000;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--seed" && i + 1 < argc) {
            seed = std::stoull(argv[++i]);
        } else if (arg == "--count" && i + 1 < argc) {
            count = std::stoull(argv[++i]);
        }
    }

    std::cout << "Running differential test: seed=" << seed << ", count=" << count << "...\n";

    WorkloadConfig config;
    config.seed = seed;
    config.count = count;
    config.inject_anomalies = true;

    WorkloadGenerator gen(config);
    auto commands = gen.generate();

    OrderBook engine;
    ReferenceMatcher ref;
    InvariantChecker::LifecycleAuditor auditor;

    for (size_t i = 0; i < commands.size(); ++i) {
        const auto& cmd = commands[i];
        std::vector<Event> eng_events, ref_events;
        engine.process(cmd, eng_events);
        ref.process(cmd, ref_events);

        if (eng_events.size() != ref_events.size()) {
            std::cerr << "MISMATCH: Command " << i << " event counts differ (eng="
                      << eng_events.size() << ", ref=" << ref_events.size() << ")\n";
            return 1;
        }

        for (size_t e = 0; e < eng_events.size(); ++e) {
            std::string eng_str = Serializer::to_json(eng_events[e]);
            std::string ref_str = Serializer::to_json(ref_events[e]);
            if (eng_str != ref_str) {
                std::cerr << "MISMATCH: Command " << i << " event " << e << " differ:\n"
                          << "  Engine: " << eng_str << "\n"
                          << "  Ref:    " << ref_str << "\n";
                return 1;
            }
            auditor.record_event(eng_events[e]);
        }

        auto inv = InvariantChecker::check_book_invariants(engine);
        if (!inv.ok) {
            std::cerr << "INVARIANT FAIL on command " << i << ": " << inv.message << "\n";
            return 1;
        }
    }

    if (engine.get_snapshot() != ref.get_snapshot()) {
        std::cerr << "MISMATCH: Final book snapshots differ between Engine and Reference Matcher!\n";
        return 1;
    }

    auto rec = auditor.verify_reconciliation(engine);
    if (!rec.ok) {
        std::cerr << "RECONCILIATION FAIL: " << rec.message << "\n";
        return 1;
    }

    std::cout << "DIFFERENTIAL TEST PASSED! Engine and Reference Matcher produced 100% identical outputs.\n";
    return 0;
}

int run_sim(int argc, char* argv[]) {
    std::string config_path;
    std::string events_out_path;
    std::string fills_out_path;
    std::string state_out_path;
    std::string manifest_out_path;
    bool verify = false;
    bool demo_mode = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--events-out" && i + 1 < argc) {
            events_out_path = argv[++i];
        } else if (arg == "--fills-out" && i + 1 < argc) {
            fills_out_path = argv[++i];
        } else if (arg == "--state-out" && i + 1 < argc) {
            state_out_path = argv[++i];
        } else if (arg == "--manifest-out" && i + 1 < argc) {
            manifest_out_path = argv[++i];
        } else if (arg == "--verify") {
            verify = true;
        } else if (arg == "--demo") {
            demo_mode = true;
        }
    }

    std::string config_content;
    SimulationConfig config;

    if (!config_path.empty()) {
        std::ifstream cfs(config_path);
        if (!cfs.is_open()) {
            std::cerr << "Error: cannot open configuration file: " << config_path << "\n";
            return 1;
        }
        std::stringstream buffer;
        buffer << cfs.rdbuf();
        config_content = buffer.str();
        auto [cfg_opt, err] = SimulationConfig::from_json_strict(config_content);
        if (!cfg_opt) {
            std::cerr << "Configuration error in " << config_path << ": " << err << "\n";
            return 1;
        }
        config = *cfg_opt;
    } else {
        std::ifstream cfs("data/demo_session.json");
        if (cfs.is_open()) {
            std::stringstream buffer;
            buffer << cfs.rdbuf();
            config_content = buffer.str();
            auto [cfg_opt, err] = SimulationConfig::from_json_strict(config_content);
            if (!cfg_opt) {
                std::cerr << "Configuration error in data/demo_session.json: " << err << "\n";
                return 1;
            }
            config = *cfg_opt;
        } else {
            config_content = config.to_json();
        }
    }

    if (demo_mode) {
        std::cout << "=========================================================\n";
        std::cout << "  MarketLab Milestone 2 Simulation Interactive Demo\n";
        std::cout << "=========================================================\n";
        std::cout << "Strategy: InventoryAwareMarketMaker (Account: " << config.strategy_account_id << ")\n";
        std::cout << "Starting Balances: " << config.strategy_starting_cash << " cash, "
                  << config.strategy_starting_inventory << " inventory\n";
        std::cout << "Latencies: MD=" << (config.latencies.market_data_delay / 1000000.0) << "ms, Strat="
                  << (config.latencies.strategy_delay / 1000000.0) << "ms, OE="
                  << (config.latencies.order_entry_delay / 1000000.0) << "ms, ER="
                  << (config.latencies.execution_report_delay / 1000000.0) << "ms\n";
        std::cout << "Simulating deterministic trading session...\n\n";
    }

    SimulationEngine engine(config);
    engine.setup_synthetic_scenario();

    auto t0 = std::chrono::steady_clock::now();
    auto summary = engine.run();
    auto t1 = std::chrono::steady_clock::now();

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    // Export canonical files if requested
    if (!events_out_path.empty()) {
        if (!engine.export_events_binary(events_out_path)) {
            std::cerr << "Error: Failed to export events to " << events_out_path << "\n";
            return 1;
        }
    }
    if (!fills_out_path.empty()) {
        if (!engine.export_fills_binary(fills_out_path)) {
            std::cerr << "Error: Failed to export fills to " << fills_out_path << "\n";
            return 1;
        }
    }
    if (!state_out_path.empty()) {
        if (!engine.export_state_binary(state_out_path)) {
            std::cerr << "Error: Failed to export state to " << state_out_path << "\n";
            return 1;
        }
    }
    if (!manifest_out_path.empty()) {
        if (!engine.export_manifest_binary(manifest_out_path, config_content)) {
            std::cerr << "Error: Failed to export manifest to " << manifest_out_path << "\n";
            return 1;
        }
    }

    // Compute canonical SHA-256 hashes of exact bytes
    std::string events_hash = !events_out_path.empty()
        ? Sha256::hash_file(events_out_path).value_or("")
        : Sha256::hash_string(engine.get_events_content());

    std::string fills_hash = !fills_out_path.empty()
        ? Sha256::hash_file(fills_out_path).value_or("")
        : Sha256::hash_string(engine.get_fills_content());

    std::string state_hash = !state_out_path.empty()
        ? Sha256::hash_file(state_out_path).value_or("")
        : Sha256::hash_string(engine.get_state_content());

    std::string manifest_hash = !manifest_out_path.empty()
        ? Sha256::hash_file(manifest_out_path).value_or("")
        : Sha256::hash_string(engine.get_manifest_content(config_content));

    std::cout << "=========================================================\n";
    std::cout << "  MarketLab Simulation Session Summary\n";
    std::cout << "=========================================================\n";
    std::cout << "Simulated Time:      " << (summary.simulated_time_ns / 1000000000.0) << " s ("
              << summary.simulated_time_ns << " ns)\n";
    std::cout << "Termination Reason:  " << to_string(summary.termination_reason) << "\n";
    std::cout << "Total Events:        " << summary.total_events_processed << " events in "
              << elapsed_us << " us service time\n\n";

    std::cout << "--- Invariant & Conservation Auditing ---\n";
    std::cout << "Cash Conservation:      " << (summary.cash_conserved ? "[PASS]" : "[FAIL]")
              << " (Initial: " << summary.initial_total_cash << ", Final: " << summary.final_total_cash << ")\n";
    std::cout << "Inventory Conservation: " << (summary.inventory_conserved ? "[PASS]" : "[FAIL]")
              << " (Initial: " << summary.initial_total_inventory << ", Final: " << summary.final_total_inventory << ")\n\n";

    std::cout << "--- Strategy Performance (Account: " << config.strategy_account_id << ") ---\n";
    std::cout << "Orders Submitted:    " << summary.strategy_orders_submitted << "\n";
    std::cout << "Orders Accepted:     " << summary.strategy_orders_accepted << "\n";
    std::cout << "Risk Rejections:     " << summary.strategy_risk_rejections << "\n";
    std::cout << "Trades Executed:     " << summary.strategy_trades << "\n";
    std::cout << "Final Cash:          " << summary.strategy_final_cash << "\n";
    std::cout << "Final Inventory:     " << summary.strategy_final_inventory << " lots\n";
    std::cout << "Realized PnL:        " << summary.strategy_realized_pnl << "\n";
    if (summary.strategy_unrealized_pnl) {
        std::cout << "Unrealized PnL:      " << *summary.strategy_unrealized_pnl << "\n";
        std::cout << "Total PnL:           " << *summary.strategy_total_pnl << "\n";
    }
    std::cout << "Trading Fees Paid:   " << summary.strategy_fees_paid << "\n\n";

    std::cout << "--- Exchange & Ledger Totals ---\n";
    std::cout << "Total Trades:        " << summary.total_trades << "\n";
    std::cout << "Total Volume Traded: " << summary.total_volume_traded << " lots\n";
    std::cout << "Total Notional:      " << summary.total_notional_traded << "\n";
    std::cout << "Total Fees Accrued:  " << summary.total_fees_collected << " (Account: " << config.fees.fee_account_id << ")\n\n";

    std::cout << "--- Canonical SHA-256 Hashes ---\n";
    std::cout << "Events SHA-256:   " << events_hash;
    if (!events_out_path.empty()) std::cout << "  (" << events_out_path << ")";
    std::cout << "\n";
    std::cout << "Fills SHA-256:    " << fills_hash;
    if (!fills_out_path.empty()) std::cout << "  (" << fills_out_path << ")";
    std::cout << "\n";
    std::cout << "State SHA-256:    " << state_hash;
    if (!state_out_path.empty()) std::cout << "  (" << state_out_path << ")";
    std::cout << "\n";
    std::cout << "Manifest SHA-256: " << manifest_hash;
    if (!manifest_out_path.empty()) std::cout << "  (" << manifest_out_path << ")";
    std::cout << "\n";
    std::cout << "=========================================================\n";

    if (verify) {
        if (!summary.cash_conserved || !summary.inventory_conserved) {
            std::cerr << "Verification error: Conservation check failed!\n";
            return 1;
        }
        std::cout << "Audit verification: ALL MONETARY & INVENTORY CONSERVATION CHECKS PASSED!\n";
    }

    return 0;
}

int run_reconstruct(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: marketlab reconstruct --input <raw_capture> --output <normalized_stream>\n";
            return 0;
        } else {
            std::cerr << "Unknown reconstruct argument: " << arg << "\n";
            return 1;
        }
    }

    if (input_path.empty() || output_path.empty()) {
        std::cerr << "Error: Both --input and --output are required for 'reconstruct'.\n";
        return 1;
    }

    std::ifstream ifs(input_path, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: Could not open input file: " << input_path << "\n";
        return 1;
    }

    MarketDataNormalizer normalizer;
    std::vector<std::string> errors;
    auto events = normalizer.normalize_stream(ifs, &errors);
    ifs.close();

    if (!errors.empty()) {
        std::cerr << "Warnings/Errors during normalization:\n";
        for (const auto& err : errors) {
            std::cerr << "  " << err << "\n";
        }
    }

    std::ofstream ofs(output_path, std::ios::binary);
    if (!ofs.is_open()) {
        std::cerr << "Error: Could not open output file for writing: " << output_path << "\n";
        return 1;
    }

    MarketDataNormalizer::write_normalized_stream(ofs, events);
    ofs.close();

    auto hash_opt = Sha256::hash_file(output_path);
    std::string hash = hash_opt.value_or("ERROR");

    std::cout << "Reconstructed " << events.size() << " normalized events.\n";
    std::cout << "Output: " << output_path << "\n";
    std::cout << "Normalized SHA-256: " << hash << "\n";
    return 0;
}

int run_replay_hist(int argc, char* argv[]) {
    std::string input_path;
    std::string events_out_path;
    std::string fills_out_path;
    std::string state_out_path;
    std::string manifest_out_path;
    bool verify = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "--events-out" && i + 1 < argc) {
            events_out_path = argv[++i];
        } else if (arg == "--fills-out" && i + 1 < argc) {
            fills_out_path = argv[++i];
        } else if (arg == "--state-out" && i + 1 < argc) {
            state_out_path = argv[++i];
        } else if (arg == "--manifest-out" && i + 1 < argc) {
            manifest_out_path = argv[++i];
        } else if (arg == "--verify") {
            verify = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: marketlab replay-hist --input <normalized_stream> [options]\n";
            return 0;
        } else {
            std::cerr << "Unknown replay-hist argument: " << arg << "\n";
            return 1;
        }
    }

    if (input_path.empty()) {
        std::cerr << "Error: --input is required for 'replay-hist'.\n";
        return 1;
    }

    std::ifstream ifs(input_path, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: Could not open input file: " << input_path << "\n";
        return 1;
    }

    auto events = MarketDataNormalizer::read_normalized_stream(ifs);
    ifs.close();

    std::string raw_input_hash = Sha256::hash_file(input_path).value_or("");

    HistoricalReplayConfig cfg;
    HistoricalReplayEngine engine(cfg);
    engine.load_events(std::move(events));

    auto start_time = std::chrono::steady_clock::now();
    HistoricalReplaySummary summary = engine.run();
    auto end_time = std::chrono::steady_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

    // Export outputs if requested
    if (!events_out_path.empty()) {
        if (!engine.export_events_binary(events_out_path)) {
            std::cerr << "Error: Failed to write events export to " << events_out_path << "\n";
            return 1;
        }
    }
    if (!fills_out_path.empty()) {
        if (!engine.export_fills_binary(fills_out_path)) {
            std::cerr << "Error: Failed to write fills export to " << fills_out_path << "\n";
            return 1;
        }
    }
    if (!state_out_path.empty()) {
        if (!engine.export_state_binary(state_out_path)) {
            std::cerr << "Error: Failed to write state export to " << state_out_path << "\n";
            return 1;
        }
    }
    if (!manifest_out_path.empty()) {
        if (!engine.export_manifest_binary(manifest_out_path, raw_input_hash)) {
            std::cerr << "Error: Failed to write manifest export to " << manifest_out_path << "\n";
            return 1;
        }
    }

    std::string events_hash = !events_out_path.empty()
        ? Sha256::hash_file(events_out_path).value_or("")
        : Sha256::hash_string(engine.get_events_content());

    std::string fills_hash = !fills_out_path.empty()
        ? Sha256::hash_file(fills_out_path).value_or("")
        : Sha256::hash_string(engine.get_fills_content());

    std::string state_hash = !state_out_path.empty()
        ? Sha256::hash_file(state_out_path).value_or("")
        : Sha256::hash_string(engine.get_state_content());

    std::string manifest_hash = !manifest_out_path.empty()
        ? Sha256::hash_file(manifest_out_path).value_or("")
        : Sha256::hash_string(engine.get_manifest_content(raw_input_hash));

    std::cout << "=========================================================\n";
    std::cout << "  MarketLab Historical Replay Summary\n";
    std::cout << "=========================================================\n";
    std::cout << "Feed Status:          " << to_string(summary.final_feed_status) << "\n";
    std::cout << "Final Update ID:      " << summary.final_update_id << "\n";
    std::cout << "Order Book Levels:    Bids=" << summary.bid_depth_levels << ", Asks=" << summary.ask_depth_levels << "\n";
    if (summary.best_bid && summary.best_ask) {
        std::cout << "Top of Book:          Bid=" << *summary.best_bid << " | Ask=" << *summary.best_ask << "\n";
    }
    std::cout << "Events Processed:     " << summary.total_events_processed << " in " << elapsed_us << " us\n";
    std::cout << "Simulated Time:       " << (summary.simulated_time_ns / 1000000000.0) << " s\n\n";

    std::cout << "--- Strategy Execution (Account: " << cfg.strategy_account_id << ") ---\n";
    std::cout << "Orders Submitted:     " << summary.strategy_orders_submitted << "\n";
    std::cout << "Orders Accepted:      " << summary.strategy_orders_accepted << "\n";
    std::cout << "Risk Rejections:      " << summary.strategy_risk_rejections << "\n";
    std::cout << "Aggressive Trades:    " << summary.strategy_aggressive_trades << "\n";
    std::cout << "Passive Trades:       " << summary.strategy_passive_trades << "\n";
    std::cout << "Total Volume Traded:  " << summary.strategy_volume_traded << " lots\n";
    std::cout << "Realized PnL:         " << summary.strategy_realized_pnl << "\n";
    if (summary.strategy_total_pnl) {
        std::cout << "Total PnL (w/ Mark):  " << *summary.strategy_total_pnl << "\n";
    }
    std::cout << "Fees Paid:            " << summary.strategy_fees_paid << "\n\n";

    std::cout << "--- Double-Entry Ledger Conservation ---\n";
    std::cout << "Cash Conservation:      " << (summary.cash_conserved ? "[PASS]" : "[FAIL]")
              << " (" << summary.initial_total_cash << " == " << summary.final_total_cash << ")\n";
    std::cout << "Inventory Conservation: " << (summary.inventory_conserved ? "[PASS]" : "[FAIL]")
              << " (" << summary.initial_total_inventory << " == " << summary.final_total_inventory << ")\n\n";

    std::cout << "--- Canonical SHA-256 Hashes ---\n";
    std::cout << "Events SHA-256:   " << events_hash;
    if (!events_out_path.empty()) std::cout << "  (" << events_out_path << ")";
    std::cout << "\n";
    std::cout << "Fills SHA-256:    " << fills_hash;
    if (!fills_out_path.empty()) std::cout << "  (" << fills_out_path << ")";
    std::cout << "\n";
    std::cout << "State SHA-256:    " << state_hash;
    if (!state_out_path.empty()) std::cout << "  (" << state_out_path << ")";
    std::cout << "\n";
    std::cout << "Manifest SHA-256: " << manifest_hash;
    if (!manifest_out_path.empty()) std::cout << "  (" << manifest_out_path << ")";
    std::cout << "\n=========================================================\n";

    if (verify) {
        if (!summary.cash_conserved || !summary.inventory_conserved) {
            std::cerr << "Verification error: Monetary or inventory conservation failed!\n";
            return 1;
        }
        std::cout << "Audit verification: ALL CONSERVATION CHECKS PASSED!\n";
    }

    return 0;
}

int run_backtest(int argc, char* argv[]) {
    std::string input_path;
    std::string config_path;
    std::string events_out_path;
    std::string fills_out_path;
    std::string state_out_path;
    std::string report_out_path;
    std::string manifest_out_path;
    bool verify = false;

    BacktestConfig cfg;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--strategy-mode" && i + 1 < argc) {
            std::string sm = argv[++i];
            if (sm == "TAKER" || sm == "taker") cfg.strategy_config.mode = ReferenceStrategyMode::Taker;
            else if (sm == "MARKET_MAKER" || sm == "market_maker" || sm == "MM") cfg.strategy_config.mode = ReferenceStrategyMode::MarketMaker;
            else cfg.strategy_config.mode = ReferenceStrategyMode::Hybrid;
        } else if (arg == "--quote-size" && i + 1 < argc) {
            cfg.strategy_config.quote_size = std::stoll(argv[++i]);
        } else if (arg == "--half-spread" && i + 1 < argc) {
            cfg.strategy_config.half_spread = std::stoll(argv[++i]);
        } else if (arg == "--skew" && i + 1 < argc) {
            cfg.strategy_config.skew_per_lot = std::stoll(argv[++i]);
        } else if (arg == "--rebalance-threshold" && i + 1 < argc) {
            cfg.strategy_config.rebalance_threshold = std::stoll(argv[++i]);
        } else if (arg == "--queue-model" && i + 1 < argc) {
            cfg.execution_model.queue_model = parse_queue_model(argv[++i]);
        } else if (arg == "--queue-pct" && i + 1 < argc) {
            cfg.execution_model.queue_position_pct = std::stoi(argv[++i]);
        } else if (arg == "--impact-model" && i + 1 < argc) {
            cfg.execution_model.impact_model = parse_impact_model(argv[++i]);
        } else if (arg == "--slippage-ticks" && i + 1 < argc) {
            cfg.execution_model.fixed_slippage_ticks = std::stoll(argv[++i]);
        } else if (arg == "--impact-ticks" && i + 1 < argc) {
            cfg.execution_model.impact_ticks_per_lot = std::stoll(argv[++i]);
        } else if (arg == "--maker-fee" && i + 1 < argc) {
            cfg.fees.maker_fee_bps = std::stoll(argv[++i]);
        } else if (arg == "--taker-fee" && i + 1 < argc) {
            cfg.fees.taker_fee_bps = std::stoll(argv[++i]);
        } else if (arg == "--starting-cash" && i + 1 < argc) {
            cfg.strategy_starting_cash = std::stoll(argv[++i]);
        } else if (arg == "--starting-inv" && i + 1 < argc) {
            cfg.strategy_starting_inventory = std::stoll(argv[++i]);
        } else if (arg == "--events-out" && i + 1 < argc) {
            events_out_path = argv[++i];
        } else if (arg == "--fills-out" && i + 1 < argc) {
            fills_out_path = argv[++i];
        } else if (arg == "--state-out" && i + 1 < argc) {
            state_out_path = argv[++i];
        } else if (arg == "--report-out" && i + 1 < argc) {
            report_out_path = argv[++i];
        } else if (arg == "--manifest-out" && i + 1 < argc) {
            manifest_out_path = argv[++i];
        } else if (arg == "--verify") {
            verify = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: marketlab backtest --input <normalized_stream> [options]\n";
            return 0;
        } else {
            std::cerr << "Unknown backtest argument: " << arg << "\n";
            return 1;
        }
    }

    if (input_path.empty()) {
        std::cerr << "Error: --input <file> is required for backtest.\n";
        return 1;
    }

    std::ifstream ifs(input_path, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open input file: " << input_path << "\n";
        return 1;
    }

    auto events = MarketDataNormalizer::read_normalized_stream(ifs);
    ifs.close();

    if (events.empty()) {
        std::cerr << "Error: no valid normalized events read from " << input_path << "\n";
        return 1;
    }

    BacktestEngine engine(cfg);
    engine.load_events(std::move(events));

    auto t0 = std::chrono::high_resolution_clock::now();
    BacktestReport report = engine.run();
    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    // Print summary to stdout
    std::cout << report.to_string() << "\n";

    // Write canonical export files in explicit binary mode (UTF-8/LF)
    auto write_bin = [](const std::string& path, const std::string& content) -> bool {
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs.is_open()) return false;
        ofs.write(content.data(), content.size());
        return ofs.good();
    };

    if (!events_out_path.empty()) {
        if (!write_bin(events_out_path, engine.get_events_content())) {
            std::cerr << "Error: failed to write events output file: " << events_out_path << "\n";
            return 1;
        }
    }

    if (!fills_out_path.empty()) {
        if (!write_bin(fills_out_path, engine.get_fills_content())) {
            std::cerr << "Error: failed to write fills output file: " << fills_out_path << "\n";
            return 1;
        }
    }

    if (!state_out_path.empty()) {
        if (!write_bin(state_out_path, engine.get_state_content())) {
            std::cerr << "Error: failed to write state output file: " << state_out_path << "\n";
            return 1;
        }
    }

    std::string report_json = report.to_json();
    if (!report_out_path.empty()) {
        if (!write_bin(report_out_path, report_json)) {
            std::cerr << "Error: failed to write report output file: " << report_out_path << "\n";
            return 1;
        }
    }

    std::string events_hash = !events_out_path.empty()
        ? Sha256::hash_file(events_out_path).value_or("")
        : Sha256::hash_string(engine.get_events_content());

    std::string fills_hash = !fills_out_path.empty()
        ? Sha256::hash_file(fills_out_path).value_or("")
        : Sha256::hash_string(engine.get_fills_content());

    std::string state_hash = !state_out_path.empty()
        ? Sha256::hash_file(state_out_path).value_or("")
        : Sha256::hash_string(engine.get_state_content());

    std::string report_hash = !report_out_path.empty()
        ? Sha256::hash_file(report_out_path).value_or("")
        : Sha256::hash_string(report_json);

    std::string manifest_content = engine.get_manifest_content(report, events_hash, fills_hash, state_hash, report_hash);
    if (!manifest_out_path.empty()) {
        if (!write_bin(manifest_out_path, manifest_content)) {
            std::cerr << "Error: failed to write manifest output file: " << manifest_out_path << "\n";
            return 1;
        }
    }

    std::string manifest_hash = !manifest_out_path.empty()
        ? Sha256::hash_file(manifest_out_path).value_or("")
        : Sha256::hash_string(manifest_content);

    std::cout << "--- Canonical SHA-256 Hashes ---\n";
    std::cout << "Events SHA-256:   " << events_hash;
    if (!events_out_path.empty()) std::cout << "  (" << events_out_path << ")";
    std::cout << "\n";
    std::cout << "Fills SHA-256:    " << fills_hash;
    if (!fills_out_path.empty()) std::cout << "  (" << fills_out_path << ")";
    std::cout << "\n";
    std::cout << "State SHA-256:    " << state_hash;
    if (!state_out_path.empty()) std::cout << "  (" << state_out_path << ")";
    std::cout << "\n";
    std::cout << "Report SHA-256:   " << report_hash;
    if (!report_out_path.empty()) std::cout << "  (" << report_out_path << ")";
    std::cout << "\n";
    std::cout << "Manifest SHA-256: " << manifest_hash;
    if (!manifest_out_path.empty()) std::cout << "  (" << manifest_out_path << ")";
    std::cout << "\n";
    std::cout << "Runtime:          " << elapsed_us << " us\n";
    std::cout << "=========================================================\n";

    if (verify) {
        if (!report.cash_conserved || !report.inventory_conserved) {
            std::cerr << "Verification error: Monetary or inventory conservation failed!\n";
            return 1;
        }
        std::cout << "Audit verification: ALL CONSERVATION CHECKS PASSED!\n";
    }

    return 0;
}

int run_calibrate(int argc, char* argv[]) {
    std::string input_path;
    std::string config_path;
    std::string report_out_path;
    double split_ratio = 0.6;
    CalibrationObjective objective = CalibrationObjective::NetPnL;
    bool verify = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--input" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--split" && i + 1 < argc) {
            split_ratio = std::stod(argv[++i]);
        } else if (arg == "--objective" && i + 1 < argc) {
            auto obj_opt = parse_calibration_objective(argv[++i]);
            if (obj_opt) objective = *obj_opt;
        } else if (arg == "--report-out" && i + 1 < argc) {
            report_out_path = argv[++i];
        } else if (arg == "--verify") {
            verify = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: marketlab calibrate --input <normalized_stream> [options]\n";
            return 0;
        } else {
            std::cerr << "Unknown calibrate argument: " << arg << "\n";
            return 1;
        }
    }

    if (input_path.empty()) {
        std::cerr << "Error: --input <file> is required for calibrate.\n";
        return 1;
    }

    std::ifstream ifs(input_path, std::ios::binary);
    if (!ifs.is_open()) {
        std::cerr << "Error: cannot open input file: " << input_path << "\n";
        return 1;
    }

    auto events = MarketDataNormalizer::read_normalized_stream(ifs);
    ifs.close();

    if (events.empty()) {
        std::cerr << "Error: no valid normalized events read from " << input_path << "\n";
        return 1;
    }

    CalibrationConfig cal_cfg;
    cal_cfg.split_ratio = split_ratio;
    cal_cfg.objective = objective;

    // Baseline candidate
    ParameterCandidate base_cand;
    base_cand.label = "Baseline (Conservative, 200 spread, 10k quote)";
    base_cand.strategy_config.mode = ReferenceStrategyMode::Hybrid;
    base_cand.strategy_config.quote_size = 10000;
    base_cand.strategy_config.half_spread = 200;
    base_cand.strategy_config.skew_per_lot = 1;
    base_cand.strategy_config.rebalance_threshold = 30000;
    base_cand.execution_model.queue_model = QueuePositionModel::Conservative;
    base_cand.execution_model.queue_position_pct = 100;
    base_cand.execution_model.impact_model = AggressiveImpactModel::None;
    cal_cfg.baseline_candidate = base_cand;

    // Candidate grid
    cal_cfg.candidate_grid.push_back(base_cand);

    // Candidate 1: Tight spread
    ParameterCandidate c1 = base_cand;
    c1.label = "Candidate 1 (Tight Spread: 100 ticks)";
    c1.strategy_config.half_spread = 100;
    cal_cfg.candidate_grid.push_back(c1);

    // Candidate 2: Wide spread
    ParameterCandidate c2 = base_cand;
    c2.label = "Candidate 2 (Wide Spread: 300 ticks)";
    c2.strategy_config.half_spread = 300;
    cal_cfg.candidate_grid.push_back(c2);

    // Candidate 3: Large quote size
    ParameterCandidate c3 = base_cand;
    c3.label = "Candidate 3 (Large Size: 20k quote)";
    c3.strategy_config.quote_size = 20000;
    cal_cfg.candidate_grid.push_back(c3);

    // Candidate 4: Optimistic queue position (sensitivity)
    ParameterCandidate c4 = base_cand;
    c4.label = "Candidate 4 (Optimistic Queue: 0% ahead)";
    c4.execution_model.queue_model = QueuePositionModel::Optimistic;
    c4.execution_model.queue_position_pct = 0;
    cal_cfg.candidate_grid.push_back(c4);

    // Candidate 5: Proportional queue position (sensitivity)
    ParameterCandidate c5 = base_cand;
    c5.label = "Candidate 5 (Proportional Queue: 50% ahead)";
    c5.execution_model.queue_model = QueuePositionModel::Proportional;
    c5.execution_model.queue_position_pct = 50;
    cal_cfg.candidate_grid.push_back(c5);

    // Candidate 6: Fixed slippage (sensitivity)
    ParameterCandidate c6 = base_cand;
    c6.label = "Candidate 6 (Fixed Slippage: 2 ticks)";
    c6.execution_model.impact_model = AggressiveImpactModel::FixedSlippage;
    c6.execution_model.fixed_slippage_ticks = 2;
    cal_cfg.candidate_grid.push_back(c6);

    CalibrationEngine cal_engine(cal_cfg);
    cal_engine.prepare_data(events);

    auto t0 = std::chrono::high_resolution_clock::now();
    CalibrationReport report = cal_engine.run();
    auto t1 = std::chrono::high_resolution_clock::now();
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    std::cout << report.to_string() << "\n";
    std::cout << "Calibration Time:       " << elapsed_us << " us\n";

    if (!report_out_path.empty()) {
        std::ofstream ofs(report_out_path, std::ios::binary);
        if (ofs.is_open()) {
            std::string r_json = report.to_json();
            ofs.write(r_json.data(), r_json.size());
            std::cout << "Report exported to:     " << report_out_path << "\n";
        }
    }

    if (verify) {
        if (!report.baseline_holdout_report.cash_conserved || !report.baseline_holdout_report.inventory_conserved ||
            !report.selected_holdout_report.cash_conserved || !report.selected_holdout_report.inventory_conserved) {
            std::cerr << "Verification error: Holdout session conservation failed!\n";
            return 1;
        }
        std::cout << "Audit verification: ALL CONSERVATION CHECKS PASSED!\n";
    }

    return 0;
}

int run_demo_hist() {
    std::cout << "=========================================================\n";
    std::cout << "  MarketLab Milestone 3 Historical Execution Demo\n";
    std::cout << "=========================================================\n\n";

    std::cout << "--- 1. Order Book Reconstruction Protocol (Binance Spot BTCUSDT) ---\n";
    ReconstructedOrderBook book;
    std::cout << "Initial Feed Status: " << to_string(book.status()) << "\n";

    // 1. Snapshot
    std::cout << "Applying REST depth snapshot (lastUpdateId=100)...\n";
    std::vector<LevelUpdate> snap_bids{{6000000, 150000}, {5999000, 200000}};
    std::vector<LevelUpdate> snap_asks{{6001000, 100000}, {6002000, 300000}};
    book.apply_snapshot(100, snap_bids, snap_asks);
    std::cout << "Feed Status: " << to_string(book.status()) << " (Awaiting bridge update)\n";

    // 2. Drop stale update
    std::cout << "Received depth update [U=90, u=95] <= 100: Dropped as stale.\n";
    bool res_stale = book.apply_depth_update(90, 95, {}, {});
    std::cout << "Update applied: " << (res_stale ? "YES" : "NO (Dropped)") << "\n";

    // 3. Bridge update
    std::cout << "Received depth update [U=98, u=105] bridging 101: Successfully bridged!\n";
    std::vector<LevelUpdate> upd1_bids{{6000000, 180000}};
    std::vector<LevelUpdate> upd1_asks{{6001000, 120000}};
    book.apply_depth_update(98, 105, upd1_bids, upd1_asks);
    std::cout << "Feed Status: " << to_string(book.status()) << " | Top: Bid="
              << *book.best_bid() << " (" << book.get_bid_qty(*book.best_bid()) << ") | Ask="
              << *book.best_ask() << " (" << book.get_ask_qty(*book.best_ask()) << ")\n\n";

    std::cout << "--- 2. Historical Execution Adapter: Conservative Model ---\n";
    FeeConfig fees{10, 20, "EXCHANGE_FEES"};
    Ledger ledger(fees);
    RiskLimits risk;
    risk.max_order_notional = 10000000000000LL;
    risk.max_order_qty = 10000000LL;
    risk.max_inventory = 10000000LL;
    RiskEngine risk_engine(risk);
    HistoricalExecutionAdapter adapter(book, ledger, risk_engine, fees);

    AccountId strat_id = "DEMO_STRATEGY";
    ledger.fund_account(strat_id, 2000000000000LL, 100000LL);

    std::cout << "Strategy funded: Cash=" << ledger.get_account(strat_id)->cash
              << ", Inventory=" << ledger.get_account(strat_id)->inventory << "\n";

    // Aggressive order test
    std::cout << "\n>>> Submitting Aggressive IOC Buy for 150000 lots (sweeping through 6001000)...\n";
    std::vector<ExecutionReport> reports;
    std::vector<TradeSettlement> settlements;
    NewOrderCommand buy_cmd{1, 1001, Side::Buy, OrderType::Limit, 6002000, 150000, TimeInForce::IOC};
    adapter.submit_order(1000, strat_id, buy_cmd, 6002000, reports, settlements);

    for (const auto& rpt : reports) {
        std::cout << "  ExecutionReport: " << ((rpt.type == ExecutionReport::Type::Trade) ? "TRADE" :
                     (rpt.type == ExecutionReport::Type::Accepted) ? "ACCEPTED" : "CANCELED")
                  << " | Order " << rpt.order_id << " | Price: " << rpt.price
                  << " | Filled: " << (rpt.quantity - rpt.remaining_quantity) << "/" << rpt.quantity << "\n";
    }

    // Passive order with queue-ahead test
    std::cout << "\n>>> Submitting Passive Resting Buy at 6000000 for 50000 lots...\n";
    reports.clear();
    settlements.clear();
    NewOrderCommand passive_cmd{2, 1002, Side::Buy, OrderType::Limit, 6000000, 50000, TimeInForce::GTC};
    adapter.submit_order(2000, strat_id, passive_cmd, 6000000, reports, settlements);
    const auto& active = adapter.active_passive_orders();
    auto it = active.find(1002);
    if (it != active.end()) {
        std::cout << "  Passive Order 1002 placed: Resting at " << it->second.price
                  << ", initial queue_ahead = " << it->second.queue_ahead << " lots\n";
    }

    // Public trade depleting queue ahead
    std::cout << "\n>>> Public Trade occurs at 6000000 for 100000 lots (seller is taker)...\n";
    reports.clear();
    settlements.clear();
    NormalizedTrade trade1{3000, 1700000000000ULL, 1, 6000000, 100000, Side::Sell};
    adapter.on_public_trade(trade1, reports, settlements);
    it = adapter.active_passive_orders().find(1002);
    if (it != active.end()) {
        std::cout << "  Queue ahead depleted to: " << it->second.queue_ahead << " lots (No fills yet)\n";
    }

    // Subsequent public trade exhausting queue ahead and filling passive order
    std::cout << "\n>>> Second Public Trade occurs at 6000000 for 120000 lots...\n";
    reports.clear();
    settlements.clear();
    NormalizedTrade trade2{4000, 1700000000100ULL, 2, 6000000, 120000, Side::Sell};
    adapter.on_public_trade(trade2, reports, settlements);
    for (const auto& rpt : reports) {
        std::cout << "  ExecutionReport: TRADE on Order " << rpt.order_id
                  << " | Filled: " << (rpt.quantity - rpt.remaining_quantity) << "/" << rpt.quantity << "\n";
    }

    // Audit double-entry conservation
    std::cout << "\n--- 3. Authoritative Double-Entry Ledger Verification ---\n";
    auto audit = ledger.verify_all_invariants(2000000000000LL + 100000000000LL, 100000LL + 1000000000LL);
    std::cout << "Double-entry Conservation Status: " << (audit.ok ? "[PASSED]" : "[FAILED]") << "\n";
    if (!audit.ok) {
        std::cout << "Audit message: " << audit.message << "\n";
    }
    std::cout << "Strategy Final Cash:      " << ledger.get_account(strat_id)->cash << "\n";
    std::cout << "Strategy Final Inventory: " << ledger.get_account(strat_id)->inventory << " lots\n";
    std::cout << "Strategy Realized PnL:    " << ledger.get_account(strat_id)->realized_pnl << "\n";
    std::cout << "Historical Market Cash:   " << ledger.get_account("HISTORICAL_MARKET")->cash << "\n";
    std::cout << "Exchange Fee Pool Cash:   " << ledger.get_account("EXCHANGE_FEES")->cash << "\n\n";

    std::cout << "Historical Replay & Execution Demo completed successfully!\n";
    std::cout << "=========================================================\n";
    return 0;
}

int run_capture(int argc, char* argv[]) {
    std::string cmd = ".venv\\Scripts\\python.exe -m marketlab.capture";
    std::ifstream venv_check(".venv\\Scripts\\python.exe");
    if (!venv_check.is_open()) {
        cmd = "python -m marketlab.capture";
    }
    venv_check.close();

    for (int i = 2; i < argc; ++i) {
        cmd += " ";
        cmd += argv[i];
    }
    std::cout << "Starting market data capture: " << cmd << "\n";
    return std::system(cmd.c_str());
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string subcommand = argv[1];

    if (subcommand == "demo") {
        return run_demo();
    } else if (subcommand == "generate") {
        return run_generate(argc, argv);
    } else if (subcommand == "replay") {
        return run_replay(argc, argv);
    } else if (subcommand == "test-differential") {
        return run_test_differential(argc, argv);
    } else if (subcommand == "sim") {
        return run_sim(argc, argv);
    } else if (subcommand == "reconstruct") {
        return run_reconstruct(argc, argv);
    } else if (subcommand == "replay-hist") {
        return run_replay_hist(argc, argv);
    } else if (subcommand == "demo-hist") {
        return run_demo_hist();
    } else if (subcommand == "backtest") {
        return run_backtest(argc, argv);
    } else if (subcommand == "calibrate") {
        return run_calibrate(argc, argv);
    } else if (subcommand == "capture") {
        return run_capture(argc, argv);
    } else if (subcommand == "--help" || subcommand == "-h" || subcommand == "help") {
        print_help();
        return 0;
    } else {
        std::cerr << "Unknown subcommand: " << subcommand << "\n\n";
        print_help();
        return 1;
    }
}
