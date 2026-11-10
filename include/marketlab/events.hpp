#pragma once

#include <cstdint>
#include <variant>
#include "types.hpp"

namespace marketlab {

struct OrderAcceptedEvent {
    SeqNum seq_num{0};
    uint64_t event_id{0};
    OrderId order_id{0};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price{0};
    Quantity quantity{0};
    TimeInForce tif{TimeInForce::GTC};

    [[nodiscard]] constexpr bool operator==(const OrderAcceptedEvent& other) const noexcept = default;
};

struct OrderRejectedEvent {
    SeqNum seq_num{0};
    uint64_t event_id{0};
    OrderId order_id{0};
    RejectReason reason{RejectReason::None};

    [[nodiscard]] constexpr bool operator==(const OrderRejectedEvent& other) const noexcept = default;
};

struct TradeEvent {
    SeqNum seq_num{0};
    uint64_t event_id{0};
    OrderId taker_order_id{0};
    OrderId maker_order_id{0};
    Price price{0};                    // Resting maker price
    Quantity quantity{0};              // Traded lot quantity
    Side taker_side{Side::Buy};        // Aggressor side
    Quantity maker_remaining_qty{0};   // Remaining lots on maker order after fill

    [[nodiscard]] constexpr bool operator==(const TradeEvent& other) const noexcept = default;
};

struct OrderCanceledEvent {
    SeqNum seq_num{0};
    uint64_t event_id{0};
    OrderId order_id{0};
    Quantity canceled_quantity{0};
    CancelReason reason{CancelReason::UserRequested};

    [[nodiscard]] constexpr bool operator==(const OrderCanceledEvent& other) const noexcept = default;
};

using Event = std::variant<
    OrderAcceptedEvent,
    OrderRejectedEvent,
    TradeEvent,
    OrderCanceledEvent
>;

[[nodiscard]] constexpr SeqNum get_event_seq_num(const Event& ev) noexcept {
    return std::visit([](const auto& e) noexcept { return e.seq_num; }, ev);
}

[[nodiscard]] constexpr uint64_t get_event_id(const Event& ev) noexcept {
    return std::visit([](const auto& e) noexcept { return e.event_id; }, ev);
}

} // namespace marketlab
