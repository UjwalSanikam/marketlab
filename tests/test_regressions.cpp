#include <iostream>
#include <fstream>
#include <sstream>
#include <cassert>
#include <string>
#include <vector>

#include "marketlab/order_book.hpp"
#include "marketlab/reference_matcher.hpp"
#include "marketlab/serializer.hpp"
#include "marketlab/invariants.hpp"

using namespace marketlab;

#define REG_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "REGRESSION FAILED: " << msg << " at " << __FILE__ << ":" << __LINE__ << "\n"; \
            return false; \
        } \
    } while (0)

bool test_overflow_hardening() {
    std::cout << "[RUN] test_overflow_hardening...\n";
    std::ifstream ifs("scratch/review/overflow.jsonl");
    if (!ifs.is_open()) {
        ifs.open("../scratch/review/overflow.jsonl");
    }
    std::string overflow_content;
    if (ifs.is_open()) {
        std::stringstream ss;
        ss << ifs.rdbuf();
        overflow_content = ss.str();
    } else {
        overflow_content =
            "{\"type\":\"NEW\",\"seq\":1,\"id\":1,\"side\":\"BUY\",\"order_type\":\"LIMIT\",\"price\":100,\"qty\":9223372036854775807,\"tif\":\"GTC\"}\n"
            "{\"type\":\"NEW\",\"seq\":2,\"id\":2,\"side\":\"BUY\",\"order_type\":\"LIMIT\",\"price\":100,\"qty\":1,\"tif\":\"GTC\"}\n";
    }

    std::istringstream iss(overflow_content);
    auto parse_res = Serializer::read_commands_checked(iss);
    REG_ASSERT(parse_res.success, "Failed to parse overflow.jsonl");
    REG_ASSERT(parse_res.commands.size() == 2, "Expected 2 commands");

    OrderBook book;
    ReferenceMatcher ref;
    std::vector<Event> book_events;
    std::vector<Event> ref_events;

    // Command 1: Order 1 for max int64
    book.process(parse_res.commands[0], book_events);
    ref.process(parse_res.commands[0], ref_events);

    REG_ASSERT(book_events.size() == 1, "Order 1 should be accepted");
    REG_ASSERT(std::holds_alternative<OrderAcceptedEvent>(book_events[0]), "Expected accepted event");
    REG_ASSERT(book.total_bid_quantity() == 9223372036854775807LL, "Total bid qty should be max int64");

    // Command 2: Order 2 for 1 lot at price 100
    // Must be rejected deterministically with QUANTITY_OVERFLOW!
    book_events.clear();
    ref_events.clear();
    book.process(parse_res.commands[1], book_events);
    ref.process(parse_res.commands[1], ref_events);

    REG_ASSERT(book_events.size() == 1, "Expected single rejection event");
    auto* re = std::get_if<OrderRejectedEvent>(&book_events[0]);
    REG_ASSERT(re != nullptr, "Must be OrderRejectedEvent");
    REG_ASSERT(re->reason == RejectReason::QuantityOverflow, "Rejection reason must be QUANTITY_OVERFLOW");

    // Reference matcher parity
    REG_ASSERT(ref_events.size() == 1, "Reference matcher must also reject");
    auto* ref_re = std::get_if<OrderRejectedEvent>(&ref_events[0]);
    REG_ASSERT(ref_re != nullptr && ref_re->reason == RejectReason::QuantityOverflow,
               "Reference matcher must also reject with QUANTITY_OVERFLOW");

    // Ensure total quantity never wrapped to negative!
    REG_ASSERT(book.total_bid_quantity() == 9223372036854775807LL, "Total bid qty must remain positive max int64");
    auto inv = InvariantChecker::check_book_invariants(book);
    REG_ASSERT(inv.ok, inv.message);

    std::cout << "[PASS] test_overflow_hardening\n";
    return true;
}

bool test_malformed_input_hardening() {
    std::cout << "[RUN] test_malformed_input_hardening...\n";

    // 1. Line 1 with invalid TIF: must fail with line 1
    std::string malformed_tif = "{\"type\":\"NEW\",\"seq\":1,\"id\":1,\"side\":\"BUY\",\"order_type\":\"LIMIT\",\"price\":100,\"qty\":10,\"tif\":\"INVALID\"}\n";
    std::istringstream iss1(malformed_tif);
    auto res1 = Serializer::read_commands_checked(iss1);
    REG_ASSERT(!res1.success, "Must fail to parse invalid TIF");
    REG_ASSERT(res1.error->line_number == 1, "Error must identify line 1");

    // 2. Line 2 with non-command junk: must fail with line 2
    std::string mixed_file = "{\"type\":\"NEW\",\"seq\":1,\"id\":1,\"side\":\"BUY\",\"order_type\":\"LIMIT\",\"price\":100,\"qty\":10,\"tif\":\"GTC\"}\nthis is not a command\n";
    std::istringstream iss2(mixed_file);
    auto res2 = Serializer::read_commands_checked(iss2);
    REG_ASSERT(!res2.success, "Must fail on malformed command line");
    REG_ASSERT(res2.error->line_number == 2, "Error must identify line 2");

    // 3. Ignored blank lines and comment lines
    std::string comments_file = "# This is a comment\n\n   \n{\"type\":\"NEW\",\"seq\":1,\"id\":1,\"side\":\"BUY\",\"order_type\":\"LIMIT\",\"price\":100,\"qty\":10,\"tif\":\"GTC\"}\n# Another comment\n";
    std::istringstream iss3(comments_file);
    auto res3 = Serializer::read_commands_checked(iss3);
    REG_ASSERT(res3.success, "Comments and blank lines must parse cleanly");
    REG_ASSERT(res3.commands.size() == 1, "Only valid command should be retained");

    // 4. Trailing numeric junk in JSON
    std::string trailing_junk = "{\"type\":\"NEW\",\"seq\":1,\"id\":1,\"side\":\"BUY\",\"order_type\":\"LIMIT\",\"price\":100abc,\"qty\":10,\"tif\":\"GTC\"}\n";
    std::istringstream iss4(trailing_junk);
    auto res4 = Serializer::read_commands_checked(iss4);
    REG_ASSERT(!res4.success, "Must reject trailing numeric junk");

    // 5. Floating point value in integer field
    std::string float_val = "{\"type\":\"NEW\",\"seq\":1,\"id\":1,\"side\":\"BUY\",\"order_type\":\"LIMIT\",\"price\":100.5,\"qty\":10,\"tif\":\"GTC\"}\n";
    std::istringstream iss5(float_val);
    auto res5 = Serializer::read_commands_checked(iss5);
    REG_ASSERT(!res5.success, "Must reject floating point numbers");

    std::cout << "[PASS] test_malformed_input_hardening\n";
    return true;
}

bool test_boundary_quantities() {
    std::cout << "[RUN] test_boundary_quantities...\n";

    OrderBook book;
    std::vector<Event> events;

    // Positive max quantity
    book.process(NewOrderCommand{1, 10, Side::Sell, OrderType::Limit, 200, 9223372036854775807LL, TimeInForce::GTC}, events);
    REG_ASSERT(events.size() == 1 && std::holds_alternative<OrderAcceptedEvent>(events[0]), "Accepted max qty ask");
    REG_ASSERT(book.total_ask_quantity() == 9223372036854775807LL, "Ask book holds max qty");

    // Matching partially against max quantity: buy 100 at 200
    events.clear();
    book.process(NewOrderCommand{2, 11, Side::Buy, OrderType::Limit, 200, 100, TimeInForce::IOC}, events);
    REG_ASSERT(events.size() == 2, "Accepted + Trade");
    auto* tr = std::get_if<TradeEvent>(&events[1]);
    REG_ASSERT(tr && tr->quantity == 100, "Trade 100");
    REG_ASSERT(book.total_ask_quantity() == (9223372036854775807LL - 100), "Ask book reduced accurately");

    auto inv = InvariantChecker::check_book_invariants(book);
    REG_ASSERT(inv.ok, inv.message);

    std::cout << "[PASS] test_boundary_quantities\n";
    return true;
}

int main() {
    std::cout << "Running MarketLab Hardening Regression Suite...\n\n";
    bool all_ok = true;
    all_ok &= test_overflow_hardening();
    all_ok &= test_malformed_input_hardening();
    all_ok &= test_boundary_quantities();

    if (all_ok) {
        std::cout << "\nALL HARDENING REGRESSION TESTS PASSED SUCCESSFULLY!\n";
        return 0;
    } else {
        std::cerr << "\nSOME REGRESSION TESTS FAILED!\n";
        return 1;
    }
}
