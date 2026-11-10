#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <ostream>

namespace marketlab {

using Price = int64_t;       // Integer price in ticks
using Quantity = int64_t;    // Integer quantity in lots
using OrderId = uint64_t;    // Unique order ID
using SeqNum = uint64_t;     // Sequence number of input command

enum class Side : uint8_t {
    Buy = 1,
    Sell = 2
};

[[nodiscard]] constexpr std::string_view to_string(Side side) noexcept {
    switch (side) {
        case Side::Buy: return "BUY";
        case Side::Sell: return "SELL";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::optional<Side> parse_side(std::string_view sv) noexcept {
    if (sv == "BUY" || sv == "buy" || sv == "B" || sv == "b") return Side::Buy;
    if (sv == "SELL" || sv == "sell" || sv == "S" || sv == "s") return Side::Sell;
    return std::nullopt;
}

enum class OrderType : uint8_t {
    Limit = 1,
    Market = 2
};

[[nodiscard]] constexpr std::string_view to_string(OrderType ot) noexcept {
    switch (ot) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::Market: return "MARKET";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::optional<OrderType> parse_order_type(std::string_view sv) noexcept {
    if (sv == "LIMIT" || sv == "limit" || sv == "L") return OrderType::Limit;
    if (sv == "MARKET" || sv == "market" || sv == "M") return OrderType::Market;
    return std::nullopt;
}

enum class TimeInForce : uint8_t {
    GTC = 1, // Good 'Til Cancelled
    IOC = 2  // Immediate Or Cancel
};

[[nodiscard]] constexpr std::string_view to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
    }
    return "UNKNOWN";
}

[[nodiscard]] constexpr std::optional<TimeInForce> parse_time_in_force(std::string_view sv) noexcept {
    if (sv == "GTC" || sv == "gtc") return TimeInForce::GTC;
    if (sv == "IOC" || sv == "ioc") return TimeInForce::IOC;
    return std::nullopt;
}

enum class RejectReason : uint8_t {
    None = 0,
    DuplicateOrderId,
    InvalidPrice,
    InvalidQuantity,
    UnknownOrder,
    OrderAlreadyFilledOrCancelled,
    QuantityOverflow,
    InvalidTimeInForce
};

[[nodiscard]] constexpr std::string_view to_string(RejectReason rr) noexcept {
    switch (rr) {
        case RejectReason::None: return "NONE";
        case RejectReason::DuplicateOrderId: return "DUPLICATE_ORDER_ID";
        case RejectReason::InvalidPrice: return "INVALID_PRICE";
        case RejectReason::InvalidQuantity: return "INVALID_QUANTITY";
        case RejectReason::UnknownOrder: return "UNKNOWN_ORDER";
        case RejectReason::OrderAlreadyFilledOrCancelled: return "ORDER_ALREADY_FILLED_OR_CANCELLED";
        case RejectReason::QuantityOverflow: return "QUANTITY_OVERFLOW";
        case RejectReason::InvalidTimeInForce: return "INVALID_TIME_IN_FORCE";
    }
    return "UNKNOWN_REASON";
}

[[nodiscard]] constexpr std::optional<RejectReason> parse_reject_reason(std::string_view sv) noexcept {
    if (sv == "NONE") return RejectReason::None;
    if (sv == "DUPLICATE_ORDER_ID") return RejectReason::DuplicateOrderId;
    if (sv == "INVALID_PRICE") return RejectReason::InvalidPrice;
    if (sv == "INVALID_QUANTITY") return RejectReason::InvalidQuantity;
    if (sv == "UNKNOWN_ORDER") return RejectReason::UnknownOrder;
    if (sv == "ORDER_ALREADY_FILLED_OR_CANCELLED") return RejectReason::OrderAlreadyFilledOrCancelled;
    if (sv == "QUANTITY_OVERFLOW") return RejectReason::QuantityOverflow;
    if (sv == "INVALID_TIME_IN_FORCE") return RejectReason::InvalidTimeInForce;
    return std::nullopt;
}

enum class CancelReason : uint8_t {
    UserRequested = 1,
    IocRemainder = 2,
    MarketRemainder = 3
};

[[nodiscard]] constexpr std::string_view to_string(CancelReason cr) noexcept {
    switch (cr) {
        case CancelReason::UserRequested: return "USER_REQUESTED";
        case CancelReason::IocRemainder: return "IOC_REMAINDER";
        case CancelReason::MarketRemainder: return "MARKET_REMAINDER";
    }
    return "UNKNOWN_CANCEL_REASON";
}

[[nodiscard]] constexpr std::optional<CancelReason> parse_cancel_reason(std::string_view sv) noexcept {
    if (sv == "USER_REQUESTED") return CancelReason::UserRequested;
    if (sv == "IOC_REMAINDER") return CancelReason::IocRemainder;
    if (sv == "MARKET_REMAINDER") return CancelReason::MarketRemainder;
    return std::nullopt;
}

struct InstrumentConfig {
    std::string symbol = "BTC-USDT";
    Price tick_size = 1;     // Minimum price increment
    Quantity lot_size = 1;   // Minimum quantity increment
};

} // namespace marketlab
