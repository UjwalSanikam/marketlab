#pragma once

#include <cstdint>
#include <optional>
#include <limits>
#include <stdexcept>
#include "types.hpp"

namespace marketlab {

class CheckedMath {
public:
    // Safely add two int64 quantities, returning nullopt on overflow or negative result
    [[nodiscard]] static constexpr std::optional<Quantity> safe_add_quantity(Quantity a, Quantity b) noexcept {
        if (a < 0 || b < 0) return std::nullopt;
        if (a > std::numeric_limits<Quantity>::max() - b) return std::nullopt;
        return a + b;
    }

    // Safely subtract quantity b from a (a >= b >= 0)
    [[nodiscard]] static constexpr std::optional<Quantity> safe_sub_quantity(Quantity a, Quantity b) noexcept {
        if (a < 0 || b < 0 || a < b) return std::nullopt;
        return a - b;
    }

    // Safely multiply price and quantity to calculate notional
    [[nodiscard]] static constexpr std::optional<int64_t> safe_mul_notional(Price price, Quantity qty) noexcept {
        if (price < 0 || qty < 0) return std::nullopt;
        if (price == 0 || qty == 0) return 0;
        if (price > std::numeric_limits<int64_t>::max() / qty) return std::nullopt;
        return price * qty;
    }

    // Verify price aligns with tick size and is strictly positive
    [[nodiscard]] static constexpr bool is_valid_price(Price price, Price tick_size = 1) noexcept {
        if (price <= 0 || tick_size <= 0) return false;
        return (price % tick_size) == 0;
    }

    // Verify quantity aligns with lot size and is strictly positive
    [[nodiscard]] static constexpr bool is_valid_quantity(Quantity qty, Quantity lot_size = 1) noexcept {
        if (qty <= 0 || lot_size <= 0) return false;
        return (qty % lot_size) == 0;
    }
};

} // namespace marketlab
