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
    std::cout << "        --seed <num>       RNG seed (default: 42)\n";
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
        std::ofstream ofs(out_path);
        if (!ofs.is_open()) {
            std::cerr << "Error: cannot open output file: " << out_path << "\n";
            return 1;
        }
        Serializer::write_commands(ofs, commands);
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
        std::ofstream eofs(events_out_path);
        if (!eofs.is_open()) {
            std::cerr << "Error: cannot open events output file: " << events_out_path << "\n";
            return 1;
        }
        Serializer::write_events(eofs, all_events);
        std::cout << "Exported " << all_events.size() << " events to " << events_out_path << "\n";
    }

    auto snapshot = book.get_snapshot();
    std::string state_json = Serializer::to_json(snapshot);

    if (!state_out_path.empty()) {
        std::ofstream sofs(state_out_path);
        if (!sofs.is_open()) {
            std::cerr << "Error: cannot open state output file: " << state_out_path << "\n";
            return 1;
        }
        sofs << state_json << "\n";
        std::cout << "Exported final state to " << state_out_path << "\n";
    }

    // Compute canonical SHA-256 hashes
    std::string events_hash = Sha256::hash_string(events_stream_content);
    std::string state_hash = Sha256::hash_string(state_json + "\n");
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
    } else if (subcommand == "--help" || subcommand == "-h" || subcommand == "help") {
        print_help();
        return 0;
    } else {
        std::cerr << "Unknown subcommand: " << subcommand << "\n\n";
        print_help();
        return 1;
    }
}
