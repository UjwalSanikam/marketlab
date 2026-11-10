#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>

#include "order_book.hpp"
#include "events.hpp"

namespace marketlab {

struct InvariantResult {
    bool ok{true};
    std::string message;

    [[nodiscard]] static InvariantResult pass() noexcept { return {true, ""}; }
    [[nodiscard]] static InvariantResult fail(std::string msg) { return {false, std::move(msg)}; }
};

class InvariantChecker {
public:
    // Check all structural invariants on the current state of an OrderBook
    [[nodiscard]] static InvariantResult check_book_invariants(const OrderBook& book);

    // Reconcile complete lifecycle of orders against observed events and remaining book state
    class LifecycleAuditor {
    public:
        void record_event(const Event& ev);
        [[nodiscard]] InvariantResult verify_reconciliation(const OrderBook& book) const;

        void reset();

    private:
        struct OrderAudit {
            Quantity accepted_qty{0};
            Quantity filled_qty{0};
            Quantity canceled_qty{0};
            bool accepted{false};
        };

        std::unordered_map<OrderId, OrderAudit> audits_;
    };
};

} // namespace marketlab
