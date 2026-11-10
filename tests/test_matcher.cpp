#include <iostream>
#include <cassert>
#include <vector>
#include <string>

#include "marketlab/types.hpp"
#include "marketlab/order_book.hpp"
#include "marketlab/reference_matcher.hpp"
#include "marketlab/workload_generator.hpp"
#include "marketlab/invariants.hpp"
#include "marketlab/serializer.hpp"

using namespace marketlab;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
    } while (0)

// 1. Test FIFO at the same price
bool test_fifo_same_price() {
    OrderBook book;
    std::vector<Event> events;

    // Resting orders at price 100
    book.process(NewOrderCommand{1, 101, Side::Buy, OrderType::Limit, 100, 10, TimeInForce::GTC}, events);
    book.process(NewOrderCommand{2, 102, Side::Buy, OrderType::Limit, 100, 20, TimeInForce::GTC}, events);
    book.process(NewOrderCommand{3, 103, Side::Buy, OrderType::Limit, 100, 30, TimeInForce::GTC}, events);

    // Incoming sell limit matching partially against first order
    events.clear();
    book.process(NewOrderCommand{4, 104, Side::Sell, OrderType::Limit, 100, 5, TimeInForce::IOC}, events);

    TEST_ASSERT(events.size() == 2, "Expected Accepted + Trade event");
    auto* tr = std::get_if<TradeEvent>(&events[1]);
    TEST_ASSERT(tr != nullptr, "Expected TradeEvent");
    TEST_ASSERT(tr->maker_order_id == 101, "Expected first order 101 to fill first");
    TEST_ASSERT(tr->quantity == 5, "Trade qty should be 5");
    TEST_ASSERT(tr->maker_remaining_qty == 5, "Maker remaining qty should be 5");

    // Incoming sell matching remainder of first order and start of second
    events.clear();
    book.process(NewOrderCommand{5, 105, Side::Sell, OrderType::Limit, 100, 15, TimeInForce::IOC}, events);
    TEST_ASSERT(events.size() == 3, "Expected Accepted + 2 Trade events");
    auto* tr1 = std::get_if<TradeEvent>(&events[1]);
    auto* tr2 = std::get_if<TradeEvent>(&events[2]);
    TEST_ASSERT(tr1 && tr1->maker_order_id == 101 && tr1->quantity == 5 && tr1->maker_remaining_qty == 0,
                "Order 101 should be completely filled");
    TEST_ASSERT(tr2 && tr2->maker_order_id == 102 && tr2->quantity == 10 && tr2->maker_remaining_qty == 10,
                "Order 102 should be partially filled with 10 remaining");

    auto inv = InvariantChecker::check_book_invariants(book);
    TEST_ASSERT(inv.ok, inv.message);

    std::cout << "[PASS] test_fifo_same_price\n";
    return true;
}

// 2. Test Better-price priority
bool test_better_price_priority() {
    OrderBook book;
    std::vector<Event> events;

    // Resting bids: 101 arrives second at price 99, 102 arrives first at price 98, 103 arrives at 100
    book.process(NewOrderCommand{1, 102, Side::Buy, OrderType::Limit, 98, 10, TimeInForce::GTC}, events);
    book.process(NewOrderCommand{2, 101, Side::Buy, OrderType::Limit, 99, 10, TimeInForce::GTC}, events);
    book.process(NewOrderCommand{3, 103, Side::Buy, OrderType::Limit, 100, 10, TimeInForce::GTC}, events);

    // Incoming sell at 97 should fill order 103 first (highest bid), then 101, then 102
    events.clear();
    book.process(NewOrderCommand{4, 104, Side::Sell, OrderType::Limit, 97, 25, TimeInForce::IOC}, events);

    TEST_ASSERT(events.size() == 4, "Expected Accepted + 3 Trades");
    auto* t1 = std::get_if<TradeEvent>(&events[1]);
    auto* t2 = std::get_if<TradeEvent>(&events[2]);
    auto* t3 = std::get_if<TradeEvent>(&events[3]);

    TEST_ASSERT(t1 && t1->maker_order_id == 103 && t1->price == 100 && t1->quantity == 10, "Best price 100 fills first");
    TEST_ASSERT(t2 && t2->maker_order_id == 101 && t2->price == 99 && t2->quantity == 10, "Second best price 99 fills second");
    TEST_ASSERT(t3 && t3->maker_order_id == 102 && t3->price == 98 && t3->quantity == 5, "Third best price 98 fills last for 5");

    auto inv = InvariantChecker::check_book_invariants(book);
    TEST_ASSERT(inv.ok, inv.message);

    std::cout << "[PASS] test_better_price_priority\n";
    return true;
}

// 3. Test Sweeps across multiple price levels and empty price level removal
bool test_sweeps_and_empty_level_removal() {
    OrderBook book;
    std::vector<Event> events;

    // Asks at 101, 102, 103
    book.process(NewOrderCommand{1, 201, Side::Sell, OrderType::Limit, 101, 10, TimeInForce::GTC}, events);
    book.process(NewOrderCommand{2, 202, Side::Sell, OrderType::Limit, 102, 10, TimeInForce::GTC}, events);
    book.process(NewOrderCommand{3, 203, Side::Sell, OrderType::Limit, 103, 10, TimeInForce::GTC}, events);

    TEST_ASSERT(book.ask_level_count() == 3, "Expected 3 ask levels");

    // Aggressive buy order sweeping levels 101 and 102 completely, and 5 from 103
    events.clear();
    book.process(NewOrderCommand{4, 204, Side::Buy, OrderType::Limit, 103, 25, TimeInForce::GTC}, events);

    // Levels 101 and 102 should be completely removed!
    TEST_ASSERT(book.ask_level_count() == 1, "Expected 1 ask level remaining (103)");
    TEST_ASSERT(book.best_ask() == 103, "Best ask should now be 103");
    TEST_ASSERT(book.total_ask_quantity() == 5, "Total ask quantity should be 5");

    auto inv = InvariantChecker::check_book_invariants(book);
    TEST_ASSERT(inv.ok, inv.message);

    std::cout << "[PASS] test_sweeps_and_empty_level_removal\n";
    return true;
}

// 4. Test Partial fill followed by cancellation
bool test_partial_fill_then_cancel() {
    OrderBook book;
    std::vector<Event> events;

    // Maker order for 50 at 100
    book.process(NewOrderCommand{1, 301, Side::Buy, OrderType::Limit, 100, 50, TimeInForce::GTC}, events);

    // Sell 20 at 100
    events.clear();
    book.process(NewOrderCommand{2, 302, Side::Sell, OrderType::Limit, 100, 20, TimeInForce::IOC}, events);
    TEST_ASSERT(book.total_bid_quantity() == 30, "30 should remain on book");

    // Now cancel the remaining 30 of maker order 301
    events.clear();
    book.process(CancelOrderCommand{3, 301}, events);

    TEST_ASSERT(events.size() == 1, "Expected 1 Canceled event");
    auto* ce = std::get_if<OrderCanceledEvent>(&events[0]);
    TEST_ASSERT(ce && ce->order_id == 301 && ce->canceled_quantity == 30 && ce->reason == CancelReason::UserRequested,
                "Cancel event should report remaining quantity 30");

    TEST_ASSERT(book.order_count() == 0, "Book should be empty");
    TEST_ASSERT(book.bid_level_count() == 0, "Level should be deleted");

    // Second cancel on 301 should reject as already filled/cancelled
    events.clear();
    book.process(CancelOrderCommand{4, 301}, events);
    TEST_ASSERT(events.size() == 1, "Expected rejected event");
    auto* re = std::get_if<OrderRejectedEvent>(&events[0]);
    TEST_ASSERT(re && re->reason == RejectReason::OrderAlreadyFilledOrCancelled,
                "Should reject as OrderAlreadyFilledOrCancelled");

    auto inv = InvariantChecker::check_book_invariants(book);
    TEST_ASSERT(inv.ok, inv.message);

    std::cout << "[PASS] test_partial_fill_then_cancel\n";
    return true;
}

// 5. Test Empty-book market orders
bool test_empty_book_market_order() {
    OrderBook book;
    std::vector<Event> events;

    // Market Buy on empty book: accepted then immediately canceled remainder
    book.process(NewOrderCommand{1, 401, Side::Buy, OrderType::Market, 0, 100, TimeInForce::IOC}, events);

    TEST_ASSERT(events.size() == 2, "Expected Accepted + Canceled");
    auto* ae = std::get_if<OrderAcceptedEvent>(&events[0]);
    auto* ce = std::get_if<OrderCanceledEvent>(&events[1]);
    TEST_ASSERT(ae && ae->order_id == 401, "Order accepted");
    TEST_ASSERT(ce && ce->order_id == 401 && ce->canceled_quantity == 100 && ce->reason == CancelReason::MarketRemainder,
                "Order canceled remainder");
    TEST_ASSERT(book.order_count() == 0, "Book remains empty");

    auto inv = InvariantChecker::check_book_invariants(book);
    TEST_ASSERT(inv.ok, inv.message);

    std::cout << "[PASS] test_empty_book_market_order\n";
    return true;
}

// 6. Test GTC versus IOC behavior
bool test_gtc_vs_ioc() {
    OrderBook book;
    std::vector<Event> events;

    // Sell limit 10 at 100
    book.process(NewOrderCommand{1, 501, Side::Sell, OrderType::Limit, 100, 10, TimeInForce::GTC}, events);

    // Buy IOC limit 25 at 100: fills 10 at 100, cancels remaining 15!
    events.clear();
    book.process(NewOrderCommand{2, 502, Side::Buy, OrderType::Limit, 100, 25, TimeInForce::IOC}, events);

    TEST_ASSERT(events.size() == 3, "Expected Accepted + Trade + Canceled");
    auto* ae = std::get_if<OrderAcceptedEvent>(&events[0]);
    auto* te = std::get_if<TradeEvent>(&events[1]);
    auto* ce = std::get_if<OrderCanceledEvent>(&events[2]);

    TEST_ASSERT(ae && ae->order_id == 502, "IOC order accepted");
    TEST_ASSERT(te && te->quantity == 10 && te->price == 100, "Trade 10 at 100");
    TEST_ASSERT(ce && ce->order_id == 502 && ce->canceled_quantity == 15 && ce->reason == CancelReason::IocRemainder,
                "IOC remainder 15 canceled");

    // Book should be completely empty now
    TEST_ASSERT(book.order_count() == 0, "Book should be empty");

    auto inv = InvariantChecker::check_book_invariants(book);
    TEST_ASSERT(inv.ok, inv.message);

    std::cout << "[PASS] test_gtc_vs_ioc\n";
    return true;
}

// 7. Test Duplicate IDs and invalid commands
bool test_duplicates_and_invalid_commands() {
    OrderBook book;
    std::vector<Event> events;

    // Submit valid order
    book.process(NewOrderCommand{1, 601, Side::Buy, OrderType::Limit, 100, 10, TimeInForce::GTC}, events);
    TEST_ASSERT(events.size() == 1 && std::holds_alternative<OrderAcceptedEvent>(events[0]), "Accepted");

    // Resubmit duplicate order ID 601
    events.clear();
    book.process(NewOrderCommand{2, 601, Side::Sell, OrderType::Limit, 105, 10, TimeInForce::GTC}, events);
    TEST_ASSERT(events.size() == 1, "Expected single event");
    auto* r1 = std::get_if<OrderRejectedEvent>(&events[0]);
    TEST_ASSERT(r1 && r1->reason == RejectReason::DuplicateOrderId, "Must reject duplicate ID");

    // Invalid non-positive price for limit order
    events.clear();
    book.process(NewOrderCommand{3, 602, Side::Buy, OrderType::Limit, 0, 10, TimeInForce::GTC}, events);
    auto* r2 = std::get_if<OrderRejectedEvent>(&events[0]);
    TEST_ASSERT(r2 && r2->reason == RejectReason::InvalidPrice, "Must reject zero price for limit");

    // Invalid quantity (zero)
    events.clear();
    book.process(NewOrderCommand{4, 603, Side::Buy, OrderType::Limit, 100, 0, TimeInForce::GTC}, events);
    auto* r3 = std::get_if<OrderRejectedEvent>(&events[0]);
    TEST_ASSERT(r3 && r3->reason == RejectReason::InvalidQuantity, "Must reject zero quantity");

    // Market order with non-zero price
    events.clear();
    book.process(NewOrderCommand{5, 604, Side::Buy, OrderType::Market, 100, 10, TimeInForce::IOC}, events);
    auto* r4 = std::get_if<OrderRejectedEvent>(&events[0]);
    TEST_ASSERT(r4 && r4->reason == RejectReason::InvalidPrice, "Must reject market order with non-zero price");

    // Cancel unknown order
    events.clear();
    book.process(CancelOrderCommand{6, 999999}, events);
    auto* r5 = std::get_if<OrderRejectedEvent>(&events[0]);
    TEST_ASSERT(r5 && r5->reason == RejectReason::UnknownOrder, "Must reject unknown cancel order");

    std::cout << "[PASS] test_duplicates_and_invalid_commands\n";
    return true;
}

// 8. Differential test against ReferenceMatcher with seeded randomized stream
bool test_differential_randomized(uint64_t seed, size_t command_count) {
    WorkloadConfig config;
    config.seed = seed;
    config.count = command_count;
    config.inject_anomalies = true;

    WorkloadGenerator gen(config);
    auto commands = gen.generate();

    OrderBook engine;
    ReferenceMatcher ref;
    InvariantChecker::LifecycleAuditor engine_auditor;

    for (size_t i = 0; i < commands.size(); ++i) {
        const auto& cmd = commands[i];

        std::vector<Event> engine_events;
        engine.process(cmd, engine_events);

        std::vector<Event> ref_events;
        ref.process(cmd, ref_events);

        // Verify event counts
        if (engine_events.size() != ref_events.size()) {
            std::cerr << "Command " << i << " event count mismatch: engine="
                      << engine_events.size() << " ref=" << ref_events.size() << "\n";
            return false;
        }

        // Verify each event identically matches
        for (size_t e = 0; e < engine_events.size(); ++e) {
            std::string eng_json = Serializer::to_json(engine_events[e]);
            std::string ref_json = Serializer::to_json(ref_events[e]);
            if (eng_json != ref_json) {
                std::cerr << "Event mismatch on command " << i << ", event " << e << ":\n"
                          << "  Engine: " << eng_json << "\n"
                          << "  Ref:    " << ref_json << "\n";
                return false;
            }
            engine_auditor.record_event(engine_events[e]);
        }

        // Check book invariants after each command
        auto inv = InvariantChecker::check_book_invariants(engine);
        if (!inv.ok) {
            std::cerr << "Invariant violation on command " << i << ": " << inv.message << "\n";
            return false;
        }
    }

    // Verify snapshot parity
    auto eng_snap = engine.get_snapshot();
    auto ref_snap = ref.get_snapshot();

    if (eng_snap != ref_snap) {
        std::cerr << "Final snapshot mismatch!\n";
        std::cerr << "Engine snapshot:\n" << Serializer::to_ascii_book(eng_snap) << "\n";
        std::cerr << "Ref snapshot:\n" << Serializer::to_ascii_book(ref_snap) << "\n";
        return false;
    }

    // Verify complete lifecycle reconciliation
    auto reconcil = engine_auditor.verify_reconciliation(engine);
    if (!reconcil.ok) {
        std::cerr << "Reconciliation failure: " << reconcil.message << "\n";
        return false;
    }

    std::cout << "[PASS] test_differential_randomized (" << command_count << " commands, seed=" << seed << ")\n";
    return true;
}

int main() {
    std::cout << "Running MarketLab Milestone 1 test suite...\n";

    bool all_ok = true;
    all_ok &= test_fifo_same_price();
    all_ok &= test_better_price_priority();
    all_ok &= test_sweeps_and_empty_level_removal();
    all_ok &= test_partial_fill_then_cancel();
    all_ok &= test_empty_book_market_order();
    all_ok &= test_gtc_vs_ioc();
    all_ok &= test_duplicates_and_invalid_commands();

    // Differential tests across multiple random seeds
    all_ok &= test_differential_randomized(42, 2000);
    all_ok &= test_differential_randomized(1337, 2000);
    all_ok &= test_differential_randomized(202611, 2000);

    if (all_ok) {
        std::cout << "\nALL TESTS PASSED SUCCESSFULLY!\n";
        return 0;
    } else {
        std::cerr << "\nSOME TESTS FAILED!\n";
        return 1;
    }
}
