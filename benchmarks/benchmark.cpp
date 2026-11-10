#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <cmath>

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

struct OutcomeStats {
    size_t total_commands{0};
    size_t accepted_orders{0};
    size_t rejected_orders{0};
    size_t successful_cancels{0};
    size_t rejected_cancels{0};
    size_t remainder_cancels{0};
    size_t trades_executed{0};
    std::unordered_map<std::string, size_t> reject_reasons;
};

OutcomeStats collect_outcomes(const std::vector<Command>& commands, OrderBook& book) {
    OutcomeStats stats;
    stats.total_commands = commands.size();

    std::vector<Event> event_buf;
    event_buf.reserve(16);

    for (const auto& cmd : commands) {
        event_buf.clear();
        book.process(cmd, event_buf);

        bool is_cancel_cmd = std::holds_alternative<CancelOrderCommand>(cmd);

        for (const auto& ev : event_buf) {
            std::visit([&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, OrderAcceptedEvent>) {
                    stats.accepted_orders++;
                } else if constexpr (std::is_same_v<T, TradeEvent>) {
                    stats.trades_executed++;
                } else if constexpr (std::is_same_v<T, OrderCanceledEvent>) {
                    if (e.reason == CancelReason::UserRequested) {
                        stats.successful_cancels++;
                    } else {
                        stats.remainder_cancels++;
                    }
                } else if constexpr (std::is_same_v<T, OrderRejectedEvent>) {
                    stats.reject_reasons[std::string(to_string(e.reason))]++;
                    if (is_cancel_cmd) {
                        stats.rejected_cancels++;
                    } else {
                        stats.rejected_orders++;
                    }
                }
            }, ev);
        }
    }
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

    std::cout << "=========================================================================\n";
    std::cout << " MarketLab Milestone 1: In-Memory Service-Time Benchmark\n";
    std::cout << "=========================================================================\n";
    std::cout << "Target: In-memory order book service time (excludes network, queueing, I/O)\n";
    std::cout << "Environment:\n";
#if defined(__clang__)
    std::cout << "  Compiler:       Clang " << __clang_version__ << "\n";
#elif defined(__GNUC__)
    std::cout << "  Compiler:       GCC " << __VERSION__ << "\n";
#elif defined(_MSC_VER)
    std::cout << "  Compiler:       MSVC " << _MSC_VER << "\n";
#else
    std::cout << "  Compiler:       Unknown\n";
#endif

#if defined(NDEBUG)
    std::cout << "  Build mode:     Release (NDEBUG, -O3, -static)\n";
#else
    std::cout << "  Build mode:     Debug / RelWithDebInfo\n";
#endif
    std::cout << "  Core Matcher:   Single-threaded (std::map + std::list + std::unordered_map)\n";
    std::cout << "  Seed:           " << seed << "\n";
    std::cout << "  Command count:  " << command_count << "\n";

    // -------------------------------------------------------------
    // TIMER CALIBRATION & GRANULARITY MEASUREMENT
    // -------------------------------------------------------------
    std::cout << "\n-------------------------------------------------------------------------\n";
    std::cout << " 1. SYSTEM TIMER CALIBRATION\n";
    std::cout << "-------------------------------------------------------------------------\n";

    const size_t timer_test_iters = 100000;
    auto timer_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < timer_test_iters; ++i) {
        auto dummy = std::chrono::steady_clock::now();
        (void)dummy;
    }
    auto timer_end = std::chrono::steady_clock::now();
    double timer_call_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(timer_end - timer_start).count()
    ) / timer_test_iters;

    uint64_t min_non_zero_delta_ns = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i < 50000; ++i) {
        auto a = std::chrono::steady_clock::now();
        auto b = std::chrono::steady_clock::now();
        auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        if (delta > 0 && static_cast<uint64_t>(delta) < min_non_zero_delta_ns) {
            min_non_zero_delta_ns = static_cast<uint64_t>(delta);
        }
    }

    std::cout << "  Timer clock:       std::chrono::steady_clock (Windows QPC)\n";
    std::cout << "  Call overhead:     ~" << std::fixed << std::setprecision(1) << timer_call_ns << " ns/call\n";
    std::cout << "  Timer resolution:  ~" << min_non_zero_delta_ns << " ns minimum observable tick\n";
    std::cout << "  Interpretation:    Zero-duration latency samples (0.0 ns) occur when a command\n"
              << "                     completes faster than the timer resolution (< " << min_non_zero_delta_ns << " ns).\n";

    // -------------------------------------------------------------
    // WORKLOAD GENERATION & OUTCOME ACCOUNTING
    // -------------------------------------------------------------
    WorkloadConfig config;
    config.seed = seed;
    config.count = command_count;
    config.limit_order_pct = 70;
    config.market_order_pct = 10;
    config.cancel_pct = 20;
    config.ioc_limit_pct = 5;
    config.inject_anomalies = false;

    std::cout << "\n-------------------------------------------------------------------------\n";
    std::cout << " 2. WORKLOAD CONFIGURATION & COMMAND OUTCOMES\n";
    std::cout << "-------------------------------------------------------------------------\n";

    WorkloadGenerator gen(config);
    auto commands = gen.generate();

    OrderBook outcome_book;
    auto outcomes = collect_outcomes(commands, outcome_book);

    std::cout << "  Total commands:        " << outcomes.total_commands << "\n";
    std::cout << "  Accepted new orders:   " << outcomes.accepted_orders << "\n";
    std::cout << "  Successful cancels:    " << outcomes.successful_cancels << "\n";
    std::cout << "  Rejected cancels:      " << outcomes.rejected_cancels << "\n";
    std::cout << "  Remainder cancels:     " << outcomes.remainder_cancels << " (IOC / Market remainder)\n";
    std::cout << "  Trades executed:       " << outcomes.trades_executed << "\n";
    std::cout << "  Rejected new orders:   " << outcomes.rejected_orders << "\n";
    if (!outcomes.reject_reasons.empty()) {
        std::cout << "  Rejection breakdown:\n";
        for (const auto& [reason, count] : outcomes.reject_reasons) {
            std::cout << "    - " << reason << ": " << count << "\n";
        }
    }
    std::cout << "  Active book size:      " << outcome_book.order_count() << " resting orders ("
              << outcome_book.bid_level_count() << " bid levels, "
              << outcome_book.ask_level_count() << " ask levels)\n";

    auto inv_check = InvariantChecker::check_book_invariants(outcome_book);
    std::cout << "  Book invariant audit:  " << (inv_check.ok ? "PASS" : ("FAIL: " + inv_check.message)) << "\n";

    // -------------------------------------------------------------
    // RUN 3: REPEATED THROUGHPUT TRIALS
    // -------------------------------------------------------------
    std::cout << "\n-------------------------------------------------------------------------\n";
    std::cout << " 3. UN-INSTRUMENTED THROUGHPUT (5 REPEATED TRIALS)\n";
    std::cout << " (Pure in-memory service time, zero per-command timing overhead)\n";
    std::cout << "-------------------------------------------------------------------------\n";

    const size_t num_trials = 5;
    std::vector<double> trial_throughputs;
    std::vector<Event> event_buf;
    event_buf.reserve(16);

    for (size_t trial = 1; trial <= num_trials; ++trial) {
        OrderBook trial_book;

        // Warmup on a slice
        for (size_t i = 0; i < std::min(size_t(10000), commands.size()); ++i) {
            trial_book.process(commands[i], event_buf);
            event_buf.clear();
        }
        trial_book.reset();

        auto t0 = std::chrono::steady_clock::now();
        for (const auto& cmd : commands) {
            trial_book.process(cmd, event_buf);
            event_buf.clear();
        }
        auto t1 = std::chrono::steady_clock::now();

        auto dur_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        double dur_sec = static_cast<double>(dur_ns) / 1e9;
        double throughput = static_cast<double>(commands.size()) / dur_sec;
        trial_throughputs.push_back(throughput);

        std::cout << "  Trial " << trial << ": " << std::fixed << std::setprecision(4) << dur_sec << " s | "
                  << std::fixed << std::setprecision(0) << throughput << " commands/sec\n";
    }

    double sum_tp = std::accumulate(trial_throughputs.begin(), trial_throughputs.end(), 0.0);
    double mean_tp = sum_tp / num_trials;
    double min_tp = *std::min_element(trial_throughputs.begin(), trial_throughputs.end());
    double max_tp = *std::max_element(trial_throughputs.begin(), trial_throughputs.end());

    double variance = 0.0;
    for (double tp : trial_throughputs) {
        variance += (tp - mean_tp) * (tp - mean_tp);
    }
    double stddev_tp = std::sqrt(variance / num_trials);

    std::cout << "  ---------------------------------------------------\n";
    std::cout << "  Mean throughput:       " << std::fixed << std::setprecision(0) << mean_tp << " commands/sec\n";
    std::cout << "  Throughput range:      [" << min_tp << ", " << max_tp << "] commands/sec\n";
    std::cout << "  Standard deviation:    +/- " << stddev_tp << " commands/sec ("
              << std::setprecision(2) << (stddev_tp / mean_tp * 100.0) << "%)\n";

    // -------------------------------------------------------------
    // RUN 4: INSTRUMENTED LATENCY DISTRIBUTION
    // -------------------------------------------------------------
    std::cout << "\n-------------------------------------------------------------------------\n";
    std::cout << " 4. INSTRUMENTED SERVICE-TIME LATENCY DISTRIBUTION\n";
    std::cout << " (Per-command steady_clock sample; includes ~" << std::fixed << std::setprecision(1)
              << timer_call_ns << " ns clock overhead)\n";
    std::cout << "-------------------------------------------------------------------------\n";

    OrderBook latency_book;
    std::vector<uint64_t> latencies_ns(commands.size());

    // Warm-up
    for (size_t i = 0; i < std::min(size_t(10000), commands.size()); ++i) {
        latency_book.process(commands[i], event_buf);
        event_buf.clear();
    }
    latency_book.reset();

    for (size_t i = 0; i < commands.size(); ++i) {
        auto cmd_t0 = std::chrono::steady_clock::now();
        latency_book.process(commands[i], event_buf);
        auto cmd_t1 = std::chrono::steady_clock::now();
        latencies_ns[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(cmd_t1 - cmd_t0).count();
        event_buf.clear();
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

    std::cout << "\nMethodology & Constraints:\n";
    std::cout << "  - Measurements represent in-memory single-threaded service time.\n";
    std::cout << "  - Peak tail latency reflects maximum observed duration including OS thread scheduling\n"
              << "    jitter and memory management pauses.\n";
    std::cout << "  - Bounded flat price ladder implementation planned for milestone 6 to eliminate\n"
              << "    tree node dynamic allocations.\n";
    std::cout << "=========================================================================\n";

    return 0;
}
