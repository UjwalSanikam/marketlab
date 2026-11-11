#pragma once

#include <cstdint>
#include <queue>
#include <vector>
#include <functional>
#include <stdexcept>
#include <optional>
#include <iostream>

#include "sim_types.hpp"

namespace marketlab {

enum class EventPriority : uint8_t {
    MarketDataDelivery = 1,
    StrategyWakeup = 2,
    OrderAdmission = 3,
    ExecutionReportDelivery = 4,
    BackgroundArrival = 5
};

struct ScheduledEvent {
    SimTime time{0};
    EventPriority priority{EventPriority::OrderAdmission};
    uint64_t sequence_id{0};
    std::function<void(SimTime)> callback;

    // Custom comparator for min-heap priority queue
    // Earliest time first; if time equal, lowest priority tier; if equal, lowest sequence_id (FIFO)
    bool operator>(const ScheduledEvent& other) const noexcept {
        if (time != other.time) return time > other.time;
        if (priority != other.priority) return static_cast<uint8_t>(priority) > static_cast<uint8_t>(other.priority);
        return sequence_id > other.sequence_id;
    }
};

class VirtualScheduler {
public:
    VirtualScheduler() = default;

    [[nodiscard]] SimTime now() const noexcept { return current_time_; }
    [[nodiscard]] size_t pending_event_count() const noexcept { return queue_.size(); }
    [[nodiscard]] uint64_t total_events_processed() const noexcept { return events_processed_; }

    // Schedule an event at virtual timestamp 'at_time'
    bool schedule(SimTime at_time, EventPriority priority, std::function<void(SimTime)> callback) {
        if (at_time < current_time_) {
            // Cannot schedule in the past
            return false;
        }
        uint64_t seq = ++next_seq_id_;
        queue_.push(ScheduledEvent{at_time, priority, seq, std::move(callback)});
        return true;
    }

    // Schedule an event with a relative delay from now()
    bool schedule_after(SimTime delay, EventPriority priority, std::function<void(SimTime)> callback) {
        auto at_time = safe_add_time(current_time_, delay);
        if (!at_time) return false;
        return schedule(*at_time, priority, std::move(callback));
    }

    // Step a single event from the queue
    bool step_one(SimTime max_time) {
        if (queue_.empty()) return false;
        if (queue_.top().time > max_time) return false;

        ScheduledEvent ev = std::move(const_cast<ScheduledEvent&>(queue_.top()));
        queue_.pop();

        current_time_ = ev.time;
        ++events_processed_;

        if (ev.callback) {
            ev.callback(current_time_);
        }
        return true;
    }

    // Reset scheduler state
    void reset() {
        while (!queue_.empty()) queue_.pop();
        current_time_ = 0;
        next_seq_id_ = 0;
        events_processed_ = 0;
    }

private:
    SimTime current_time_{0};
    uint64_t next_seq_id_{0};
    uint64_t events_processed_{0};

    std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, std::greater<ScheduledEvent>> queue_;
};

} // namespace marketlab
