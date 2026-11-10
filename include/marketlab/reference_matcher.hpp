#pragma once

#include <vector>
#include <unordered_map>
#include <optional>
#include <algorithm>

#include "types.hpp"
#include "commands.hpp"
#include "events.hpp"
#include "checked_math.hpp"
#include "order_book.hpp" // for BookSnapshot

namespace marketlab {

struct ReferenceRestingOrder {
    OrderId order_id{0};
    SeqNum seq_num{0};
    Side side{Side::Buy};
    Price price{0};
    Quantity initial_quantity{0};
    Quantity remaining_quantity{0};
    TimeInForce tif{TimeInForce::GTC};
    uint64_t arrival_order{0}; // For strict FIFO tie-breaking
};

// Deliberately simple, brute-force reference matching engine for differential testing
class ReferenceMatcher {
public:
    explicit ReferenceMatcher(InstrumentConfig config = {}) : config_(std::move(config)) {}

    void process(const Command& cmd, std::vector<Event>& out_events);

    [[nodiscard]] std::vector<Event> process(const Command& cmd) {
        std::vector<Event> events;
        process(cmd, events);
        return events;
    }

    [[nodiscard]] BookSnapshot get_snapshot(size_t max_depth = 0) const;

    void reset();

    [[nodiscard]] const InstrumentConfig& config() const noexcept { return config_; }
    [[nodiscard]] size_t order_count() const noexcept { return resting_orders_.size(); }

private:
    void process_new_order(const NewOrderCommand& cmd, std::vector<Event>& out_events);
    void process_cancel(const CancelOrderCommand& cmd, std::vector<Event>& out_events);

    uint64_t next_event_id() noexcept { return ++next_event_id_; }

    InstrumentConfig config_;
    uint64_t next_event_id_{0};
    uint64_t arrival_counter_{0};

    // Flat list of resting orders - matched via naive linear search
    std::vector<ReferenceRestingOrder> resting_orders_;

    // Order status history for rejection parity
    std::unordered_map<OrderId, StoredOrderStatus> order_status_;
};

} // namespace marketlab
