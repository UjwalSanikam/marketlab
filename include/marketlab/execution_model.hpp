#pragma once

#include <string>
#include <string_view>
#include <cstdint>
#include <algorithm>

#include "types.hpp"

namespace marketlab {

enum class QueuePositionModel : uint8_t {
    Conservative = 0, // 100% queue-ahead (back of queue) - Baseline
    Optimistic = 1,   // 0% queue-ahead (front of queue)
    Proportional = 2  // Configurable queue-ahead percentage (queue_position_pct)
};

[[nodiscard]] constexpr std::string_view to_string(QueuePositionModel m) noexcept {
    switch (m) {
        case QueuePositionModel::Conservative: return "CONSERVATIVE";
        case QueuePositionModel::Optimistic: return "OPTIMISTIC";
        case QueuePositionModel::Proportional: return "PROPORTIONAL";
    }
    return "UNKNOWN_QUEUE_MODEL";
}

[[nodiscard]] inline QueuePositionModel parse_queue_model(std::string_view s) noexcept {
    if (s == "OPTIMISTIC" || s == "optimistic" || s == "front") return QueuePositionModel::Optimistic;
    if (s == "PROPORTIONAL" || s == "proportional" || s == "fractional" || s == "mid") return QueuePositionModel::Proportional;
    return QueuePositionModel::Conservative;
}

enum class AggressiveImpactModel : uint8_t {
    None = 0,          // Baseline: visible book prices, zero impact
    FixedSlippage = 1, // Fixed tick penalty added to fill price
    LinearImpact = 2   // Price impact proportional to filled quantity
};

[[nodiscard]] constexpr std::string_view to_string(AggressiveImpactModel m) noexcept {
    switch (m) {
        case AggressiveImpactModel::None: return "NONE";
        case AggressiveImpactModel::FixedSlippage: return "FIXED_SLIPPAGE";
        case AggressiveImpactModel::LinearImpact: return "LINEAR_IMPACT";
    }
    return "UNKNOWN_IMPACT_MODEL";
}

[[nodiscard]] inline AggressiveImpactModel parse_impact_model(std::string_view s) noexcept {
    if (s == "FIXED_SLIPPAGE" || s == "fixed_slippage" || s == "slippage") return AggressiveImpactModel::FixedSlippage;
    if (s == "LINEAR_IMPACT" || s == "linear_impact" || s == "linear") return AggressiveImpactModel::LinearImpact;
    return AggressiveImpactModel::None;
}

struct ExecutionModelConfig {
    QueuePositionModel queue_model{QueuePositionModel::Conservative};
    int queue_position_pct{100}; // 0 = front of queue, 100 = back of queue (default: 100)

    AggressiveImpactModel impact_model{AggressiveImpactModel::None};
    Price fixed_slippage_ticks{0}; // Extra ticks paid for aggressive fills (default: 0)
    int64_t impact_ticks_per_lot{0}; // Linear impact scaling: ticks per 100k lots (1.0 base unit)

    [[nodiscard]] Quantity calculate_initial_queue_ahead(Quantity visible_depth) const noexcept {
        if (visible_depth <= 0) return 0;
        switch (queue_model) {
            case QueuePositionModel::Conservative:
                return visible_depth;
            case QueuePositionModel::Optimistic:
                return 0;
            case QueuePositionModel::Proportional: {
                int pct = std::clamp(queue_position_pct, 0, 100);
                return static_cast<Quantity>((static_cast<__int128_t>(visible_depth) * pct) / 100);
            }
        }
        return visible_depth;
    }

    [[nodiscard]] Price apply_aggressive_impact(Price base_price, Side side, Quantity fill_qty) const noexcept {
        Price impact_ticks = 0;
        if (impact_model == AggressiveImpactModel::FixedSlippage) {
            impact_ticks = fixed_slippage_ticks;
        } else if (impact_model == AggressiveImpactModel::LinearImpact) {
            if (impact_ticks_per_lot > 0 && fill_qty > 0) {
                // impact_ticks_per_lot is ticks per 100,000 lots (1 BTC)
                impact_ticks = static_cast<Price>((static_cast<__int128_t>(fill_qty) * impact_ticks_per_lot) / 100000);
            }
        }

        if (side == Side::Buy) {
            return base_price + impact_ticks;
        } else {
            return std::max<Price>(1, base_price - impact_ticks);
        }
    }
};

} // namespace marketlab
