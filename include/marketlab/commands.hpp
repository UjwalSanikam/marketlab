#pragma once

#include <cstdint>
#include <variant>
#include "types.hpp"

namespace marketlab {

struct NewOrderCommand {
    SeqNum seq_num{0};
    OrderId order_id{0};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    Price price{0};          // Must be 0 for Market orders, > 0 for Limit
    Quantity quantity{0};    // Must be > 0
    TimeInForce tif{TimeInForce::GTC};

    [[nodiscard]] constexpr bool operator==(const NewOrderCommand& other) const noexcept = default;
};

struct CancelOrderCommand {
    SeqNum seq_num{0};
    OrderId order_id{0};

    [[nodiscard]] constexpr bool operator==(const CancelOrderCommand& other) const noexcept = default;
};

using Command = std::variant<NewOrderCommand, CancelOrderCommand>;

[[nodiscard]] constexpr SeqNum get_seq_num(const Command& cmd) noexcept {
    return std::visit([](const auto& c) noexcept { return c.seq_num; }, cmd);
}

[[nodiscard]] constexpr OrderId get_order_id(const Command& cmd) noexcept {
    return std::visit([](const auto& c) noexcept { return c.order_id; }, cmd);
}

} // namespace marketlab
