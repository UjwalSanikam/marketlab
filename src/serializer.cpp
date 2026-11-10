#include "marketlab/serializer.hpp"
#include <sstream>
#include <iomanip>
#include <regex>

namespace marketlab {

namespace {

// Helper to extract a string value for a key in a JSON object: "key":"value" or "key":value
std::optional<std::string> extract_json_value(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"\\s*:\\s*(\"[^\"]*\"|[-+]?[0-9]+|true|false)";
    std::regex re(pattern);
    std::smatch match;
    if (std::regex_search(json, match, re) && match.size() > 1) {
        std::string val = match[1].str();
        if (val.front() == '"' && val.back() == '"') {
            return val.substr(1, val.size() - 2);
        }
        return val;
    }
    return std::nullopt;
}

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

} // namespace

std::string Serializer::to_json(const Command& cmd) {
    std::ostringstream ss;
    std::visit([&ss](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, NewOrderCommand>) {
            ss << "{\"type\":\"NEW\",\"seq\":" << c.seq_num
               << ",\"id\":" << c.order_id
               << ",\"side\":\"" << to_string(c.side)
               << "\",\"order_type\":\"" << to_string(c.type)
               << "\",\"price\":" << c.price
               << ",\"qty\":" << c.quantity
               << ",\"tif\":\"" << to_string(c.tif) << "\"}";
        } else if constexpr (std::is_same_v<T, CancelOrderCommand>) {
            ss << "{\"type\":\"CANCEL\",\"seq\":" << c.seq_num
               << ",\"id\":" << c.order_id << "}";
        }
    }, cmd);
    return ss.str();
}

std::string Serializer::to_json(const Event& ev) {
    std::ostringstream ss;
    std::visit([&ss](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, OrderAcceptedEvent>) {
            ss << "{\"event\":\"ACCEPTED\",\"seq\":" << e.seq_num
               << ",\"event_id\":" << e.event_id
               << ",\"id\":" << e.order_id
               << ",\"side\":\"" << to_string(e.side)
               << "\",\"type\":\"" << to_string(e.type)
               << "\",\"price\":" << e.price
               << ",\"qty\":" << e.quantity
               << ",\"tif\":\"" << to_string(e.tif) << "\"}";
        } else if constexpr (std::is_same_v<T, OrderRejectedEvent>) {
            ss << "{\"event\":\"REJECTED\",\"seq\":" << e.seq_num
               << ",\"event_id\":" << e.event_id
               << ",\"id\":" << e.order_id
               << ",\"reason\":\"" << to_string(e.reason) << "\"}";
        } else if constexpr (std::is_same_v<T, TradeEvent>) {
            ss << "{\"event\":\"TRADE\",\"seq\":" << e.seq_num
               << ",\"event_id\":" << e.event_id
               << ",\"taker_id\":" << e.taker_order_id
               << ",\"maker_id\":" << e.maker_order_id
               << ",\"price\":" << e.price
               << ",\"qty\":" << e.quantity
               << ",\"taker_side\":\"" << to_string(e.taker_side)
               << "\",\"maker_rem\":" << e.maker_remaining_qty << "}";
        } else if constexpr (std::is_same_v<T, OrderCanceledEvent>) {
            ss << "{\"event\":\"CANCELED\",\"seq\":" << e.seq_num
               << ",\"event_id\":" << e.event_id
               << ",\"id\":" << e.order_id
               << ",\"canceled_qty\":" << e.canceled_quantity
               << ",\"reason\":\"" << to_string(e.reason) << "\"}";
        }
    }, ev);
    return ss.str();
}

std::string Serializer::to_json(const BookSnapshot& snap) {
    std::ostringstream ss;
    ss << "{\"total_bid_qty\":" << snap.total_bid_quantity
       << ",\"total_ask_qty\":" << snap.total_ask_quantity
       << ",\"best_bid\":" << (snap.bids.empty() ? "null" : std::to_string(snap.bids.front().price))
       << ",\"best_ask\":" << (snap.asks.empty() ? "null" : std::to_string(snap.asks.front().price))
       << ",\"bids\":[";
    for (size_t i = 0; i < snap.bids.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"price\":" << snap.bids[i].price
           << ",\"qty\":" << snap.bids[i].quantity
           << ",\"orders\":" << snap.bids[i].order_count << "}";
    }
    ss << "],\"asks\":[";
    for (size_t i = 0; i < snap.asks.size(); ++i) {
        if (i > 0) ss << ",";
        ss << "{\"price\":" << snap.asks[i].price
           << ",\"qty\":" << snap.asks[i].quantity
           << ",\"orders\":" << snap.asks[i].order_count << "}";
    }
    ss << "]}";
    return ss.str();
}

std::string Serializer::to_ascii_book(const BookSnapshot& snap) {
    std::ostringstream ss;
    ss << "================== ORDER BOOK SNAPSHOT ==================\n";
    ss << "Total Bid Qty: " << snap.total_bid_quantity << " (" << snap.bids.size() << " levels) | "
       << "Total Ask Qty: " << snap.total_ask_quantity << " (" << snap.asks.size() << " levels)\n";

    if (!snap.bids.empty() && !snap.asks.empty()) {
        ss << "Best Bid: " << snap.bids.front().price
           << " | Best Ask: " << snap.asks.front().price
           << " | Spread: " << (snap.asks.front().price - snap.bids.front().price) << "\n";
    } else {
        ss << "Best Bid: " << (snap.bids.empty() ? "NONE" : std::to_string(snap.bids.front().price))
           << " | Best Ask: " << (snap.asks.empty() ? "NONE" : std::to_string(snap.asks.front().price)) << "\n";
    }
    ss << "---------------------------------------------------------\n";
    ss << "  SIDE  |   PRICE   |   QUANTITY   |  ORDERS\n";
    ss << "---------------------------------------------------------\n";

    // Show asks in reverse order (highest first down to lowest ask)
    for (auto it = snap.asks.rbegin(); it != snap.asks.rend(); ++it) {
        ss << "  ASK   | " << std::setw(9) << it->price
           << " | " << std::setw(12) << it->quantity
           << " | " << std::setw(7) << it->order_count << "\n";
    }

    if (snap.asks.empty()) {
        ss << "  ASK   |      --- NO ASKS ---     |\n";
    }

    ss << " -------+-----------+--------------+--------\n";

    for (const auto& level : snap.bids) {
        ss << "  BID   | " << std::setw(9) << level.price
           << " | " << std::setw(12) << level.quantity
           << " | " << std::setw(7) << level.order_count << "\n";
    }

    if (snap.bids.empty()) {
        ss << "  BID   |      --- NO BIDS ---     |\n";
    }

    ss << "=========================================================\n";
    return ss.str();
}

std::optional<Command> Serializer::parse_command_line(const std::string& line) {
    std::string s = trim(line);
    if (s.empty() || s.front() == '#') return std::nullopt;

    // Check if JSON format
    if (s.front() == '{') {
        auto type_str = extract_json_value(s, "type");
        if (!type_str) return std::nullopt;

        auto seq_str = extract_json_value(s, "seq");
        auto id_str = extract_json_value(s, "id");
        if (!seq_str || !id_str) return std::nullopt;

        SeqNum seq = std::stoull(*seq_str);
        OrderId id = std::stoull(*id_str);

        if (*type_str == "NEW") {
            auto side_str = extract_json_value(s, "side");
            auto otype_str = extract_json_value(s, "order_type");
            auto price_str = extract_json_value(s, "price");
            auto qty_str = extract_json_value(s, "qty");
            auto tif_str = extract_json_value(s, "tif");

            if (!side_str || !otype_str || !price_str || !qty_str) return std::nullopt;

            auto side_opt = parse_side(*side_str);
            auto otype_opt = parse_order_type(*otype_str);
            if (!side_opt || !otype_opt) return std::nullopt;

            TimeInForce tif = TimeInForce::GTC;
            if (tif_str) {
                auto tif_opt = parse_time_in_force(*tif_str);
                if (tif_opt) tif = *tif_opt;
            }

            Price price = std::stoll(*price_str);
            Quantity qty = std::stoll(*qty_str);

            return NewOrderCommand{seq, id, *side_opt, *otype_opt, price, qty, tif};
        } else if (*type_str == "CANCEL") {
            return CancelOrderCommand{seq, id};
        }
        return std::nullopt;
    }

    // Space-delimited format:
    // NEW <seq> <id> <side> <type> <price> <qty> [tif]
    // CANCEL <seq> <id>
    std::istringstream iss(s);
    std::string type_token;
    if (!(iss >> type_token)) return std::nullopt;

    if (type_token == "NEW") {
        SeqNum seq;
        OrderId id;
        std::string side_str, otype_str;
        Price price;
        Quantity qty;
        std::string tif_str = "GTC";

        if (!(iss >> seq >> id >> side_str >> otype_str >> price >> qty)) {
            return std::nullopt;
        }

        iss >> tif_str;

        auto side_opt = parse_side(side_str);
        auto otype_opt = parse_order_type(otype_str);
        auto tif_opt = parse_time_in_force(tif_str);

        if (!side_opt || !otype_opt || !tif_opt) return std::nullopt;

        return NewOrderCommand{seq, id, *side_opt, *otype_opt, price, qty, *tif_opt};
    } else if (type_token == "CANCEL") {
        SeqNum seq;
        OrderId id;
        if (!(iss >> seq >> id)) return std::nullopt;
        return CancelOrderCommand{seq, id};
    }

    return std::nullopt;
}

std::vector<Command> Serializer::read_commands(std::istream& is) {
    std::vector<Command> commands;
    std::string line;
    while (std::getline(is, line)) {
        auto cmd = parse_command_line(line);
        if (cmd) {
            commands.push_back(*cmd);
        }
    }
    return commands;
}

void Serializer::write_commands(std::ostream& os, const std::vector<Command>& commands) {
    for (const auto& cmd : commands) {
        os << to_json(cmd) << "\n";
    }
}

void Serializer::write_events(std::ostream& os, const std::vector<Event>& events) {
    for (const auto& ev : events) {
        os << to_json(ev) << "\n";
    }
}

} // namespace marketlab
