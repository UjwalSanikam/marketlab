#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <limits>

#include "types.hpp"

namespace marketlab {

using SimTime = uint64_t; // Virtual time in nanoseconds
using AccountId = std::string;

// Safe time addition guarding against uint64_t overflow
[[nodiscard]] constexpr std::optional<SimTime> safe_add_time(SimTime base, SimTime delay) noexcept {
    if (base > std::numeric_limits<SimTime>::max() - delay) {
        return std::nullopt;
    }
    return base + delay;
}

enum class RiskRejectReason : uint8_t {
    None = 0,
    OrderQuantityExceedsMax,
    OrderNotionalExceedsMax,
    InsufficientAvailableCash,
    InsufficientAvailableInventory,
    ShortSellingProhibited,
    MaxInventoryExceeded,
    StaleMarketData,
    SelfTradeProhibited,
    UnauthorizedOrderCancellation,
    MarketBuyProtectionPriceRequired,
    TimeArithmeticOverflow
};

[[nodiscard]] constexpr std::string_view to_string(RiskRejectReason r) noexcept {
    switch (r) {
        case RiskRejectReason::None: return "NONE";
        case RiskRejectReason::OrderQuantityExceedsMax: return "ORDER_QUANTITY_EXCEEDS_MAX";
        case RiskRejectReason::OrderNotionalExceedsMax: return "ORDER_NOTIONAL_EXCEEDS_MAX";
        case RiskRejectReason::InsufficientAvailableCash: return "INSUFFICIENT_AVAILABLE_CASH";
        case RiskRejectReason::InsufficientAvailableInventory: return "INSUFFICIENT_AVAILABLE_INVENTORY";
        case RiskRejectReason::ShortSellingProhibited: return "SHORT_SELLING_PROHIBITED";
        case RiskRejectReason::MaxInventoryExceeded: return "MAX_INVENTORY_EXCEEDED";
        case RiskRejectReason::StaleMarketData: return "STALE_MARKET_DATA";
        case RiskRejectReason::SelfTradeProhibited: return "SELF_TRADE_PROHIBITED";
        case RiskRejectReason::UnauthorizedOrderCancellation: return "UNAUTHORIZED_ORDER_CANCELLATION";
        case RiskRejectReason::MarketBuyProtectionPriceRequired: return "MARKET_BUY_PROTECTION_PRICE_REQUIRED";
        case RiskRejectReason::TimeArithmeticOverflow: return "TIME_ARITHMETIC_OVERFLOW";
    }
    return "UNKNOWN_RISK_REASON";
}

enum class TerminationReason : uint8_t {
    None = 0,
    TimeHorizonReached,
    EventLimitReached,
    AllEventsProcessed,
    RiskBreachHalt,
    StrategyTerminated
};

[[nodiscard]] constexpr std::string_view to_string(TerminationReason tr) noexcept {
    switch (tr) {
        case TerminationReason::None: return "NONE";
        case TerminationReason::TimeHorizonReached: return "TIME_HORIZON_REACHED";
        case TerminationReason::EventLimitReached: return "EVENT_LIMIT_REACHED";
        case TerminationReason::AllEventsProcessed: return "ALL_EVENTS_PROCESSED";
        case TerminationReason::RiskBreachHalt: return "RISK_BREACH_HALT";
        case TerminationReason::StrategyTerminated: return "STRATEGY_TERMINATED";
    }
    return "UNKNOWN_TERMINATION";
}

struct FeeConfig {
    int64_t maker_fee_bps{0}; // Basis points (1 bps = 0.01% = 0.0001)
    int64_t taker_fee_bps{0}; // Basis points
    AccountId fee_account_id{"FEE_ACCOUNT"};

    // Calculate fee for a given trade notional: floor(notional * bps / 10000)
    [[nodiscard]] int64_t calculate_maker_fee(int64_t notional) const noexcept {
        if (maker_fee_bps <= 0 || notional <= 0) return 0;
        return (notional * maker_fee_bps) / 10000;
    }

    [[nodiscard]] int64_t calculate_taker_fee(int64_t notional) const noexcept {
        if (taker_fee_bps <= 0 || notional <= 0) return 0;
        return (notional * taker_fee_bps) / 10000;
    }

    // Conservative fee reservation for order admission
    [[nodiscard]] int64_t conservative_taker_fee_reservation(int64_t max_notional) const noexcept {
        if (taker_fee_bps <= 0 || max_notional <= 0) return 0;
        // Conservative: add 1 unit for rounding up to ensure full reservation
        return ((max_notional * taker_fee_bps) + 9999) / 10000;
    }
};

struct LatencyConfig {
    SimTime market_data_delay{0};       // Delay from observation generated to strategy delivery
    SimTime strategy_delay{0};          // Strategy computation duration
    SimTime order_entry_delay{0};       // Delay from strategy submission to runtime admission
    SimTime execution_report_delay{0};  // Delay from match/fill to strategy report delivery
};

} // namespace marketlab
