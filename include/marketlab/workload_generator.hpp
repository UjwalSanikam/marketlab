#pragma once

#include <vector>
#include <cstdint>
#include <random>

#include "types.hpp"
#include "commands.hpp"

namespace marketlab {

struct WorkloadConfig {
    uint64_t seed{42};
    size_t count{1000};
    Price mid_price{10000};
    Price tick_size{1};
    Quantity lot_size{1};

    // Probabilities (percentages summing to 100)
    int limit_order_pct{70};
    int market_order_pct{10};
    int cancel_pct{20};
    int ioc_limit_pct{5};      // % of limit orders that are IOC

    // Price spread and drift
    int max_price_offset_ticks{25};
    int max_random_walk_step{2};

    // Quantity range
    Quantity min_quantity{1};
    Quantity max_quantity{100};

    // Inject invalid/edge-case commands occasionally for differential testing
    bool inject_anomalies{false};
};

class WorkloadGenerator {
public:
    explicit WorkloadGenerator(WorkloadConfig config = {}) : config_(config), rng_(config.seed) {}

    [[nodiscard]] std::vector<Command> generate();

    void reseed(uint64_t seed) {
        config_.seed = seed;
        rng_.seed(seed);
    }

private:
    WorkloadConfig config_;
    std::mt19937_64 rng_;
};

} // namespace marketlab
