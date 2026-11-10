#pragma once

#include <cstdint>
#include <map>
#include <list>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>

#include "types.hpp"
#include "commands.hpp"
#include "events.hpp"
#include "checked_math.hpp"

namespace marketlab {

struct RestingOrder {
    OrderId order_id{0};
    SeqNum seq_num{0};
    Side side{Side::Buy};
    Price price{0};
    Quantity initial_quantity{0};
    Quantity remaining_quantity{0};
    TimeInForce tif{TimeInForce::GTC};
};

struct PriceLevel {
    Price price{0};
    Quantity total_quantity{0};
    std::list<RestingOrder> orders;
};

struct LevelInfo {
    Price price{0};
    Quantity quantity{0};
    size_t order_count{0};

    [[nodiscard]] constexpr bool operator==(const LevelInfo& other) const noexcept = default;
};

struct BookSnapshot {
    std::vector<LevelInfo> bids; // Highest price first
    std::vector<LevelInfo> asks; // Lowest price first
    Quantity total_bid_quantity{0};
    Quantity total_ask_quantity{0};

    [[nodiscard]] bool operator==(const BookSnapshot& other) const noexcept = default;
};

enum class StoredOrderStatus : uint8_t {
    Active = 1,
    Filled = 2,
    Canceled = 3
};

class OrderBook {
public:
    explicit OrderBook(InstrumentConfig config = {}) : config_(std::move(config)) {}

    // Process a command and append generated events to the output vector
    void process(const Command& cmd, std::vector<Event>& out_events);

    // Convenience overload returning events by value
    [[nodiscard]] std::vector<Event> process(const Command& cmd) {
        std::vector<Event> events;
        process(cmd, events);
        return events;
    }

    // Inspect book state
    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] std::optional<Price> spread() const noexcept;
    [[nodiscard]] Quantity total_bid_quantity() const noexcept { return total_bid_qty_; }
    [[nodiscard]] Quantity total_ask_quantity() const noexcept { return total_ask_qty_; }
    [[nodiscard]] size_t order_count() const noexcept { return order_index_.size(); }
    [[nodiscard]] size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] size_t ask_level_count() const noexcept { return asks_.size(); }

    [[nodiscard]] BookSnapshot get_snapshot(size_t max_depth = 0) const;

    // Reset book and state completely
    void reset();

    [[nodiscard]] const InstrumentConfig& config() const noexcept { return config_; }

    // Direct access for testing/invariant checking
    [[nodiscard]] const std::map<Price, PriceLevel, std::greater<Price>>& bids() const noexcept { return bids_; }
    [[nodiscard]] const std::map<Price, PriceLevel, std::less<Price>>& asks() const noexcept { return asks_; }
    [[nodiscard]] const std::unordered_map<OrderId, StoredOrderStatus>& order_history() const noexcept { return order_status_; }

private:
    struct IndexEntry {
        Price price{0};
        Side side{Side::Buy};
        std::list<RestingOrder>::iterator iter;
    };

    void process_new_order(const NewOrderCommand& cmd, std::vector<Event>& out_events);
    void process_cancel(const CancelOrderCommand& cmd, std::vector<Event>& out_events);

    void remove_order_internal(OrderId order_id, IndexEntry entry);

    uint64_t next_event_id() noexcept { return ++next_event_id_; }

    InstrumentConfig config_;
    uint64_t next_event_id_{0};

    // Price levels
    std::map<Price, PriceLevel, std::greater<Price>> bids_; // Highest bid first
    std::map<Price, PriceLevel, std::less<Price>> asks_;    // Lowest ask first

    Quantity total_bid_qty_{0};
    Quantity total_ask_qty_{0};

    // Active order index for fast cancellation
    std::unordered_map<OrderId, IndexEntry> order_index_;

    // Order lifecycle history (for duplicate prevention and unknown/filled cancel rejection)
    std::unordered_map<OrderId, StoredOrderStatus> order_status_;
};

} // namespace marketlab
