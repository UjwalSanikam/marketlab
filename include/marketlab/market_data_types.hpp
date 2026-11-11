#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdint>
#include <charconv>

#include "types.hpp"
#include "sim_types.hpp"

namespace marketlab {

enum class FeedStatus : uint8_t {
    Uninitialized = 0,
    Synchronizing,
    Valid,
    Invalid
};

[[nodiscard]] constexpr std::string_view to_string(FeedStatus s) noexcept {
    switch (s) {
        case FeedStatus::Uninitialized: return "UNINITIALIZED";
        case FeedStatus::Synchronizing: return "SYNCHRONIZING";
        case FeedStatus::Valid: return "VALID";
        case FeedStatus::Invalid: return "INVALID";
    }
    return "UNKNOWN_FEED_STATUS";
}

struct InstrumentMetadata {
    std::string symbol{"BTCUSDT"};
    int price_scale{2};      // 2 decimal places: 1 tick = 0.01 USDT
    int qty_scale{5};        // 5 decimal places: 1 lot = 0.00001 BTC
    Price tick_size{1};      // 1 integer tick = 0.01
    Quantity lot_size{1};    // 1 integer lot = 0.00001
};

// Exact integer decimal parser without binary floating-point conversion
class DecimalParser {
public:
    // Parse decimal string like "73948.50" with scale 2 to integer ticks: 7394850
    [[nodiscard]] static std::optional<int64_t> parse_decimal(std::string_view sv, int target_scale) noexcept {
        // Trim whitespace
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) sv.remove_prefix(1);
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t')) sv.remove_suffix(1);
        if (sv.empty()) return std::nullopt;

        bool negative = false;
        if (sv.front() == '-') {
            negative = true;
            sv.remove_prefix(1);
            if (sv.empty()) return std::nullopt;
        } else if (sv.front() == '+') {
            sv.remove_prefix(1);
            if (sv.empty()) return std::nullopt;
        }

        size_t dot_pos = sv.find('.');
        std::string_view integer_part = (dot_pos == std::string_view::npos) ? sv : sv.substr(0, dot_pos);
        std::string_view frac_part = (dot_pos == std::string_view::npos) ? std::string_view{} : sv.substr(dot_pos + 1);

        if (integer_part.empty() && frac_part.empty()) return std::nullopt;

        int64_t int_val = 0;
        if (!integer_part.empty()) {
            for (char c : integer_part) {
                if (c < '0' || c > '9') return std::nullopt;
                if (__builtin_mul_overflow(int_val, 10, &int_val)) return std::nullopt;
                if (__builtin_add_overflow(int_val, (c - '0'), &int_val)) return std::nullopt;
            }
        }

        // Multiply integer part by 10^target_scale
        for (int i = 0; i < target_scale; ++i) {
            if (__builtin_mul_overflow(int_val, 10, &int_val)) return std::nullopt;
        }

        // Parse fraction digits up to target_scale
        int64_t frac_val = 0;
        int parsed_scale = 0;
        for (size_t i = 0; i < frac_part.size(); ++i) {
            char c = frac_part[i];
            if (c < '0' || c > '9') return std::nullopt;
            if (parsed_scale < target_scale) {
                frac_val = frac_val * 10 + (c - '0');
                ++parsed_scale;
            } else {
                // Excess fraction digits: must be zero!
                // If non-zero, it means value has precision finer than target scale (off-tick/off-lot)
                if (c != '0') return std::nullopt;
            }
        }

        // If fewer fraction digits than target_scale, pad with zeros
        while (parsed_scale < target_scale) {
            frac_val *= 10;
            ++parsed_scale;
        }

        int64_t total = 0;
        if (__builtin_add_overflow(int_val, frac_val, &total)) return std::nullopt;

        return negative ? -total : total;
    }
};

struct LevelUpdate {
    Price price{0};
    Quantity quantity{0}; // 0 = delete level, > 0 = set absolute size
};

struct NormalizedSnapshot {
    SimTime local_receipt_time_ns{0};
    uint64_t last_update_id{0};
    std::vector<LevelUpdate> bids;
    std::vector<LevelUpdate> asks;
};

struct NormalizedDepthUpdate {
    SimTime local_receipt_time_ns{0};
    uint64_t exchange_time_ms{0};
    uint64_t first_update_id{0};
    uint64_t final_update_id{0};
    std::vector<LevelUpdate> bids;
    std::vector<LevelUpdate> asks;
};

struct NormalizedTrade {
    SimTime local_receipt_time_ns{0};
    uint64_t exchange_time_ms{0};
    uint64_t trade_id{0};
    Price price{0};
    Quantity quantity{0};
    Side taker_side{Side::Buy}; // Buy = buyer is taker, Sell = seller is taker
};

struct NormalizedFeedStatus {
    SimTime local_receipt_time_ns{0};
    FeedStatus status{FeedStatus::Uninitialized};
    std::string reason;
};

} // namespace marketlab
