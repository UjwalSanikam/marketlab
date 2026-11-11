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
#include "marketlab/sha256.hpp"

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

bool test_file_hash_parity_and_lf_bytes() {
    std::cout << "[RUN] test_file_hash_parity_and_lf_bytes...\n";

    // Setup input commands
    std::vector<Command> commands = {
        NewOrderCommand{1, 1, Side::Buy, OrderType::Limit, 100, 9223372036854775807LL, TimeInForce::GTC},
        NewOrderCommand{2, 2, Side::Buy, OrderType::Limit, 100, 1, TimeInForce::GTC}
    };

    auto run_replay_to_disk = [](const std::vector<Command>& cmds,
                                 const std::string& ev_path,
                                 const std::string& st_path,
                                 std::string& out_mem_ev_hash,
                                 std::string& out_mem_st_hash) -> bool {
        OrderBook book;
        std::vector<Event> all_events;
        std::vector<Event> step_events;
        std::string mem_events_str;

        for (const auto& cmd : cmds) {
            step_events.clear();
            book.process(cmd, step_events);
            for (const auto& ev : step_events) {
                all_events.push_back(ev);
                mem_events_str += Serializer::to_json(ev) + "\n";
            }
        }

        // Write events in binary mode with explicit LF
        std::ofstream eofs(ev_path, std::ios::binary);
        if (!eofs.is_open()) return false;
        Serializer::write_events(eofs, all_events);
        eofs.flush();
        eofs.close();

        // Write state in binary mode with explicit LF
        auto snap = book.get_snapshot();
        std::string state_str = Serializer::to_json(snap) + "\n";
        std::ofstream sofs(st_path, std::ios::binary);
        if (!sofs.is_open()) return false;
        sofs.write(state_str.data(), state_str.size());
        sofs.flush();
        sofs.close();

        out_mem_ev_hash = Sha256::hash_string(mem_events_str);
        out_mem_st_hash = Sha256::hash_string(state_str);
        return true;
    };

    std::string ev1 = "test_run1_events.jsonl";
    std::string st1 = "test_run1_state.json";
    std::string ev2 = "test_run2_events.jsonl";
    std::string st2 = "test_run2_state.json";

    std::string mem_ev_hash1, mem_st_hash1;
    std::string mem_ev_hash2, mem_st_hash2;

    REG_ASSERT(run_replay_to_disk(commands, ev1, st1, mem_ev_hash1, mem_st_hash1), "Run 1 export failed");
    REG_ASSERT(run_replay_to_disk(commands, ev2, st2, mem_ev_hash2, mem_st_hash2), "Run 2 export failed");

    // 1. Check raw bytes on disk: must contain NO \r (0x0D) carriage returns
    auto check_no_crlf = [](const std::string& path) -> bool {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) return false;
        char c;
        while (ifs.get(c)) {
            if (c == '\r') return false; // Found CR!
        }
        return true;
    };

    REG_ASSERT(check_no_crlf(ev1), "Events file contains Windows CRLF carriage returns");
    REG_ASSERT(check_no_crlf(st1), "State file contains Windows CRLF carriage returns");

    // 2. Hash actual bytes on disk
    auto disk_ev_hash1 = Sha256::hash_file(ev1);
    auto disk_st_hash1 = Sha256::hash_file(st1);
    auto disk_ev_hash2 = Sha256::hash_file(ev2);
    auto disk_st_hash2 = Sha256::hash_file(st2);

    REG_ASSERT(disk_ev_hash1.has_value() && disk_st_hash1.has_value(), "Failed reading file hashes for run 1");
    REG_ASSERT(disk_ev_hash2.has_value() && disk_st_hash2.has_value(), "Failed reading file hashes for run 2");

    // 3. Compare disk file hash against in-memory canonical hash
    REG_ASSERT(*disk_ev_hash1 == mem_ev_hash1, "Disk events hash does not match in-memory hash");
    REG_ASSERT(*disk_st_hash1 == mem_st_hash1, "Disk state hash does not match in-memory hash");

    // 4. Verify exact match with scratch/review/overflow.jsonl canonical digests
    REG_ASSERT(*disk_ev_hash1 == "9ebfd2fd9cb1c1895fb642effd23e446f8cbaffdd9de2175f342e4b3842bbc71",
               "Events hash must match canonical digest");
    REG_ASSERT(*disk_st_hash1 == "80420a419497fc64066ccb846a30620ead3e7edbc4b5685b2120f363cc5d4fee",
               "State hash must match canonical digest");

    // 5. Verify reproducibility between run 1 and run 2
    REG_ASSERT(*disk_ev_hash1 == *disk_ev_hash2, "Run 1 and Run 2 event file hashes differ");
    REG_ASSERT(*disk_st_hash1 == *disk_st_hash2, "Run 1 and Run 2 state file hashes differ");

    // Cleanup
    std::remove(ev1.c_str());
    std::remove(st1.c_str());
    std::remove(ev2.c_str());
    std::remove(st2.c_str());

    std::cout << "[PASS] test_file_hash_parity_and_lf_bytes\n";
    return true;
}

bool test_cli_config_validation() {
    std::cout << "[RUN] test_cli_config_validation...\n";

    // Locate CLI binary
    std::vector<std::string> bin_candidates = {
        "marketlab.exe",
        "./marketlab.exe",
        ".\\marketlab.exe",
        "build/marketlab.exe",
        "./build/marketlab.exe",
        ".\\build\\marketlab.exe",
        "../build/marketlab.exe"
    };
    std::string exe;
    for (const auto& c : bin_candidates) {
        std::ifstream f(c);
        if (f.good()) {
            exe = c;
            break;
        }
    }
    REG_ASSERT(!exe.empty(), "Cannot find marketlab.exe executable");

    auto normalize_path = [](std::string p) -> std::string {
#ifdef _WIN32
        for (char& c : p) {
            if (c == '/') c = '\\';
        }
#endif
        return p;
    };

    exe = normalize_path(exe);

    auto run_cli = [&](std::string cfg_file) -> int {
        cfg_file = normalize_path(cfg_file);
        std::string cmd = exe + " sim --config " + cfg_file + " > NUL 2>&1";
        return std::system(cmd.c_str());
    };

    // 1. Reproduction: Non-JSON config must return non-zero exit code
    std::string invalid_cfg = "scratch/review/invalid-config.json";
    if (!std::ifstream(invalid_cfg).good()) {
        invalid_cfg = "../scratch/review/invalid-config.json";
    }
    REG_ASSERT(std::ifstream(invalid_cfg).good(), "Cannot find invalid-config.json");
    int rc_invalid = run_cli(invalid_cfg);
    REG_ASSERT(rc_invalid != 0, "sim with invalid-config.json must exit with non-zero code");

    // Helper to write temporary config and test CLI exit code
    auto test_temp_cfg = [&](const std::string& name, const std::string& content, bool expect_success) -> bool {
        {
            std::ofstream ofs(name, std::ios::binary);
            ofs << content;
        }
        int rc = run_cli(name);
        std::remove(name.c_str());
        if (expect_success) {
            return rc == 0;
        } else {
            return rc != 0;
        }
    };

    // 2. CLI rejects malformed JSON
    REG_ASSERT(test_temp_cfg("test_cli_malformed.json", "{ not_json: 123 }", false),
               "CLI must reject malformed JSON");

    // 3. CLI rejects non-object root
    REG_ASSERT(test_temp_cfg("test_cli_array.json", "[1, 2, 3]", false),
               "CLI must reject array root");

    // 4. CLI rejects trailing garbage
    REG_ASSERT(test_temp_cfg("test_cli_trailing.json", "{\"seed\": 42} trailing", false),
               "CLI must reject trailing garbage");

    // 5. CLI rejects duplicate keys
    REG_ASSERT(test_temp_cfg("test_cli_dup.json", "{\"seed\": 42, \"seed\": 43}", false),
               "CLI must reject duplicate keys");

    // 6. CLI rejects incorrect field types
    REG_ASSERT(test_temp_cfg("test_cli_wrong_type.json", "{\"seed\": \"bad\"}", false),
               "CLI must reject incorrect field types");

    // 7. CLI rejects unknown configuration keys
    REG_ASSERT(test_temp_cfg("test_cli_unknown.json", "{\"unknown_cli_key\": 123}", false),
               "CLI must reject unknown configuration keys");

    // 8. CLI rejects explicitly invalid values
    REG_ASSERT(test_temp_cfg("test_cli_invalid_val.json", "{\"instrument\": {\"tick_size\": 0}}", false),
               "CLI must reject invalid tick size");

    // 9. CLI accepts valid config with omitted optional fields (defaults applied)
    REG_ASSERT(test_temp_cfg("test_cli_empty.json", "{}", true),
               "CLI must accept empty object and apply defaults");

    // 10. CLI accepts complete valid config
    std::string valid_cfg = "data/demo_session.json";
    if (!std::ifstream(valid_cfg).good()) {
        valid_cfg = "../data/demo_session.json";
    }
    REG_ASSERT(std::ifstream(valid_cfg).good(), "Cannot find demo_session.json");
    int rc_valid = run_cli(valid_cfg);
    REG_ASSERT(rc_valid == 0, "CLI must accept valid demo_session.json with exit code 0");

    std::cout << "[PASS] test_cli_config_validation\n";
    return true;
}

int main() {
    std::cout << "Running MarketLab Hardening Regression Suite...\n\n";
    bool all_ok = true;
    all_ok &= test_overflow_hardening();
    all_ok &= test_malformed_input_hardening();
    all_ok &= test_boundary_quantities();
    all_ok &= test_file_hash_parity_and_lf_bytes();
    all_ok &= test_cli_config_validation();

    if (all_ok) {
        std::cout << "\nALL HARDENING REGRESSION TESTS PASSED SUCCESSFULLY!\n";
        return 0;
    } else {
        std::cerr << "\nSOME REGRESSION TESTS FAILED!\n";
        return 1;
    }
}
