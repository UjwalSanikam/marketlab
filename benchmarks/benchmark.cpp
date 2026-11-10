#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <iomanip>

#include "marketlab/order_book.hpp"
#include "marketlab/workload_generator.hpp"
#include "marketlab/invariants.hpp"

using namespace marketlab;

struct LatencyStats {
    double min_ns{0};
    double p50_ns{0};
    double p90_ns{0};
    double p99_ns{0};
    double p999_ns{0};
    double max_ns{0};
    double mean_ns{0};
};

LatencyStats compute_latency_stats(std::vector<uint64_t>& latencies) {
    if (latencies.empty()) return {};

    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();

    double sum = std::accumulate(latencies.begin(), latencies.end(), 0.0);

    LatencyStats stats;
    stats.min_ns = static_cast<double>(latencies.front());
    stats.max_ns = static_cast<double>(latencies.back());
    stats.mean_ns = sum / static_cast<double>(n);
    stats.p50_ns = static_cast<double>(latencies[static_cast<size_t>(n * 0.50)]);
    stats.p90_ns = static_cast<double>(latencies[static_cast<size_t>(n * 0.90)]);
    stats.p99_ns = static_cast<double>(latencies[static_cast<size_t>(n * 0.99)]);
    stats.p999_ns = static_cast<double>(latencies[static_cast<size_t>(n * 0.999)]);
    return stats;
}

int main(int argc, char* argv[]) {
    size_t command_count = 500000;
    uint64_t seed = 42;

    if (argc > 1) {
        command_count = std::stoull(argv[1]);
    }
    if (argc > 2) {
        seed = std::stoull(argv[2]);
    }

    std::cout << "=========================================================\n";
    std::cout << " MarketLab Milestone 1: In-Memory Matching Benchmark\n";
    std::cout << "=========================================================\n";
    std::cout << "Environment:\n";
#if defined(__clang__)
    std::cout << "  Compiler: Clang " << __clang_version__ << "\n";
#elif defined(__GNUC__)
    std::cout << "  Compiler: GCC " << __VERSION__ << "\n";
#elif defined(_MSC_VER)
    std::cout << "  Compiler: MSVC " << _MSC_VER << "\n";
#else
    std::cout << "  Compiler: Unknown\n";
#endif

#if defined(NDEBUG)
    std::cout << "  Build mode: Release (NDEBUG defined)\n";
#else
    std::cout << "  Build mode: Debug / RelWithDebInfo\n";
#endif
    std::cout << "  Target: Single-threaded limit order book (std::map + std::list)\n";
    std::cout << "  Seed: " << seed << "\n";
    std::cout << "  Commands: " << command_count << "\n";

    WorkloadConfig config;
    config.seed = seed;
    config.count = command_count;
    config.limit_order_pct = 70;
    config.market_order_pct = 10;
    config.cancel_pct = 20;
    config.ioc_limit_pct = 5;
    config.inject_anomalies = false; // Pure performance run

    std::cout << "\nGenerating in-memory workload...\n";
    WorkloadGenerator gen(config);
    auto commands = gen.generate();

    std::cout << "Workload command mix:\n";
    std::cout << "  - Limit orders:  ~" << config.limit_order_pct << "% (with " << config.ioc_limit_pct << "% IOC)\n";
    std::cout << "  - Market orders: ~" << config.market_order_pct << "%\n";
    std::cout << "  - Cancellations: ~" << config.cancel_pct << "%\n";

    // -------------------------------------------------------------
    // RUN 1: Un-instrumented Throughput Run
    // -------------------------------------------------------------
    std::cout << "\n---------------------------------------------------------\n";
    std::cout << " 1. UN-INSTRUMENTED THROUGHPUT MEASUREMENT\n";
    std::cout << " (Excludes per-command timer calls and file I/O overhead)\n";
    std::cout << "---------------------------------------------------------\n";

    OrderBook throughput_book;
    std::vector<Event> event_buffer;
    event_buffer.reserve(16);

    // Warm-up phase
    size_t warmup_count = std::min(size_t(10000), commands.size());
    for (size_t i = 0; i < warmup_count; ++i) {
        throughput_book.process(commands[i], event_buffer);
        event_buffer.clear();
    }
    throughput_book.reset();

    uint64_t total_events_emitted = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& cmd : commands) {
        throughput_book.process(cmd, event_buffer);
        total_events_emitted += event_buffer.size();
        event_buffer.clear();
    }
    auto t1 = std::chrono::steady_clock::now();

    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double duration_sec = static_cast<double>(duration_ns) / 1e9;
    double throughput_cps = static_cast<double>(commands.size()) / duration_sec;
    double events_per_sec = static_cast<double>(total_events_emitted) / duration_sec;

    std::cout << "  Execution time:        " << std::fixed << std::setprecision(4) << duration_sec << " s\n";
    std::cout << "  Throughput:            " << std::fixed << std::setprecision(0) << throughput_cps << " commands/sec\n";
    std::cout << "  Event generation rate: " << std::fixed << std::setprecision(0) << events_per_sec << " events/sec\n";
    std::cout << "  Total events:          " << total_events_emitted << "\n";
    std::cout << "  Active book size:      " << throughput_book.order_count() << " resting orders ("
              << throughput_book.bid_level_count() << " bid levels, "
              << throughput_book.ask_level_count() << " ask levels)\n";

    auto inv_check = InvariantChecker::check_book_invariants(throughput_book);
    std::cout << "  Book invariant check:  " << (inv_check.ok ? "PASS" : ("FAIL: " + inv_check.message)) << "\n";

    // -------------------------------------------------------------
    // RUN 2: Instrumented Latency Run
    // -------------------------------------------------------------
    std::cout << "\n---------------------------------------------------------\n";
    std::cout << " 2. INSTRUMENTED LATENCY MEASUREMENT\n";
    std::cout << " (High-resolution steady_clock sample per command)\n";
    std::cout << "---------------------------------------------------------\n";

    OrderBook latency_book;
    std::vector<uint64_t> latencies_ns;
    latencies_ns.resize(commands.size());

    // Warm-up phase
    for (size_t i = 0; i < warmup_count; ++i) {
        latency_book.process(commands[i], event_buffer);
        event_buffer.clear();
    }
    latency_book.reset();

    for (size_t i = 0; i < commands.size(); ++i) {
        auto cmd_t0 = std::chrono::steady_clock::now();
        latency_book.process(commands[i], event_buffer);
        auto cmd_t1 = std::chrono::steady_clock::now();
        latencies_ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(cmd_t1 - cmd_t0).count();
        event_buffer.clear();
    }

    auto stats = compute_latency_stats(latencies_ns);

    std::cout << "  Latency statistics (per command):\n";
    std::cout << "    Min:    " << std::setw(8) << std::fixed << std::setprecision(1) << stats.min_ns << " ns\n";
    std::cout << "    Mean:   " << std::setw(8) << std::fixed << std::setprecision(1) << stats.mean_ns << " ns\n";
    std::cout << "    p50:    " << std::setw(8) << std::fixed << std::setprecision(1) << stats.p50_ns << " ns ("
              << std::setprecision(3) << (stats.p50_ns / 1000.0) << " us)\n";
    std::cout << "    p90:    " << std::setw(8) << std::fixed << std::setprecision(1) << stats.p90_ns << " ns ("
              << std::setprecision(3) << (stats.p90_ns / 1000.0) << " us)\n";
    std::cout << "    p99:    " << std::setw(8) << std::fixed << std::setprecision(1) << stats.p99_ns << " ns ("
              << std::setprecision(3) << (stats.p99_ns / 1000.0) << " us)\n";
    std::cout << "    p99.9:  " << std::setw(8) << std::fixed << std::setprecision(1) << stats.p999_ns << " ns ("
              << std::setprecision(3) << (stats.p999_ns / 1000.0) << " us)\n";
    std::cout << "    Max:    " << std::setw(8) << std::fixed << std::setprecision(1) << stats.max_ns << " ns ("
              << std::setprecision(3) << (stats.max_ns / 1000.0) << " us)\n";

    std::cout << "\nLimitations:\n";
    std::cout << "  - Initial tree-map and heap-allocated list implementation; price ladder planned for milestone 6.\n";
    std::cout << "  - Individual latency timing includes clock read overhead (~15-25 ns on Windows QPC).\n";
    std::cout << "  - Single-threaded core execution without kernel bypass / network stack.\n";
    std::cout << "=========================================================\n";

    return 0;
}
