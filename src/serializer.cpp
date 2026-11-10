#include "marketlab/serializer.hpp"
#include <sstream>
#include <iomanip>
#include <regex>

namespace marketlab {

namespace {

std::string trim(std::string_view sv) {
    size_t first = sv.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return "";
    size_t last = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(first, (last - first + 1)));
}

// Extract JSON field string or raw token
std::optional<std::string> extract_json_value(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"\\s*:\\s*(\"[^\"]*\"|[-+]?[0-9a-zA-Z_.]+)?";
    std::regex re(pattern);
    std::smatch match;
    if (std::regex_search(json, match, re) && match.size() > 1 && match[1].matched) {
        std::string val = match[1].str();
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            return val.substr(1, val.size() - 2);
        }
        return val;
    }
    return std::nullopt;
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

std::optional<Command> Serializer::parse_command_line(const std::string& line, std::string* out_error) {
    std::string s = trim(line);
    if (s.empty()) {
        if (out_error) *out_error = "Empty line";
        return std::nullopt;
    }
    if (s.front() == '#') {
        if (out_error) *out_error = "Comment line";
        return std::nullopt;
    }

    if (s.front() == '{') {
        auto type_str = extract_json_value(s, "type");
        if (!type_str) {
            if (out_error) *out_error = "Missing or malformed 'type' field in JSON command";
            return std::nullopt;
        }

        auto seq_str = extract_json_value(s, "seq");
        auto id_str = extract_json_value(s, "id");
        if (!seq_str || !id_str) {
            if (out_error) *out_error = "Missing 'seq' or 'id' field in JSON command";
            return std::nullopt;
        }

        auto seq_opt = parse_strict_integer<SeqNum>(*seq_str);
        auto id_opt = parse_strict_integer<OrderId>(*id_str);
        if (!seq_opt || !id_opt) {
            if (out_error) *out_error = "Invalid numeric format or range for 'seq' or 'id'";
            return std::nullopt;
        }

        if (*type_str == "NEW") {
            auto side_str = extract_json_value(s, "side");
            auto otype_str = extract_json_value(s, "order_type");
            auto price_str = extract_json_value(s, "price");
            auto qty_str = extract_json_value(s, "qty");
            auto tif_str = extract_json_value(s, "tif");

            if (!side_str || !otype_str || !price_str || !qty_str) {
                if (out_error) *out_error = "Missing required fields for NEW order (side, order_type, price, qty)";
                return std::nullopt;
            }

            auto side_opt = parse_side(*side_str);
            if (!side_opt) {
                if (out_error) *out_error = "Invalid 'side' value: '" + *side_str + "'";
                return std::nullopt;
            }

            auto otype_opt = parse_order_type(*otype_str);
            if (!otype_opt) {
                if (out_error) *out_error = "Invalid 'order_type' value: '" + *otype_str + "'";
                return std::nullopt;
            }

            TimeInForce tif = TimeInForce::GTC;
            if (tif_str) {
                auto tif_opt = parse_time_in_force(*tif_str);
                if (!tif_opt) {
                    if (out_error) *out_error = "Invalid 'tif' (TimeInForce) value: '" + *tif_str + "'";
                    return std::nullopt;
                }
                tif = *tif_opt;
            }

            auto price_opt = parse_strict_integer<Price>(*price_str);
            if (!price_opt) {
                if (out_error) *out_error = "Invalid numeric format or range for 'price': '" + *price_str + "'";
                return std::nullopt;
            }

            auto qty_opt = parse_strict_integer<Quantity>(*qty_str);
            if (!qty_opt) {
                if (out_error) *out_error = "Invalid numeric format or range for 'qty': '" + *qty_str + "'";
                return std::nullopt;
            }

            return NewOrderCommand{*seq_opt, *id_opt, *side_opt, *otype_opt, *price_opt, *qty_opt, tif};
        } else if (*type_str == "CANCEL") {
            return CancelOrderCommand{*seq_opt, *id_opt};
        } else {
            if (out_error) *out_error = "Unknown command type: '" + *type_str + "'";
            return std::nullopt;
        }
    }

    // Space-delimited format
    std::istringstream iss(s);
    std::string type_token;
    if (!(iss >> type_token)) {
        if (out_error) *out_error = "Malformed command line";
        return std::nullopt;
    }

    if (type_token == "NEW") {
        std::string seq_tok, id_tok, side_str, otype_str, price_tok, qty_tok;
        std::string tif_str = "GTC";

        if (!(iss >> seq_tok >> id_tok >> side_str >> otype_str >> price_tok >> qty_tok)) {
            if (out_error) *out_error = "Incomplete NEW command tokens";
            return std::nullopt;
        }

        iss >> tif_str;

        auto seq_opt = parse_strict_integer<SeqNum>(seq_tok);
        auto id_opt = parse_strict_integer<OrderId>(id_tok);
        auto price_opt = parse_strict_integer<Price>(price_tok);
        auto qty_opt = parse_strict_integer<Quantity>(qty_tok);

        if (!seq_opt || !id_opt || !price_opt || !qty_opt) {
            if (out_error) *out_error = "Invalid numeric format in tokens";
            return std::nullopt;
        }

        auto side_opt = parse_side(side_str);
        auto otype_opt = parse_order_type(otype_str);
        auto tif_opt = parse_time_in_force(tif_str);

        if (!side_opt) {
            if (out_error) *out_error = "Invalid side: '" + side_str + "'";
            return std::nullopt;
        }
        if (!otype_opt) {
            if (out_error) *out_error = "Invalid order type: '" + otype_str + "'";
            return std::nullopt;
        }
        if (!tif_opt) {
            if (out_error) *out_error = "Invalid TimeInForce: '" + tif_str + "'";
            return std::nullopt;
        }

        return NewOrderCommand{*seq_opt, *id_opt, *side_opt, *otype_opt, *price_opt, *qty_opt, *tif_opt};
    } else if (type_token == "CANCEL") {
        std::string seq_tok, id_tok;
        if (!(iss >> seq_tok >> id_tok)) {
            if (out_error) *out_error = "Incomplete CANCEL command tokens";
            return std::nullopt;
        }

        auto seq_opt = parse_strict_integer<SeqNum>(seq_tok);
        auto id_opt = parse_strict_integer<OrderId>(id_tok);
        if (!seq_opt || !id_opt) {
            if (out_error) *out_error = "Invalid numeric format for CANCEL seq or id";
            return std::nullopt;
        }

        return CancelOrderCommand{*seq_opt, *id_opt};
    }

    if (out_error) *out_error = "Unrecognized command token: '" + type_token + "'";
    return std::nullopt;
}

ReadCommandsResult Serializer::read_commands_checked(std::istream& is) {
    ReadCommandsResult res;
    std::string line;
    size_t line_number = 0;

    while (std::getline(is, line)) {
        ++line_number;
        std::string trimmed = trim(line);
        // Intentional handling: ignore blank lines and '#' comment lines
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        std::string error_msg;
        auto cmd = parse_command_line(trimmed, &error_msg);
        if (!cmd) {
            res.success = false;
            res.error = ParseError{line_number, line, error_msg};
            return res;
        }
        res.commands.push_back(*cmd);
    }

    res.success = true;
    return res;
}

std::vector<Command> Serializer::read_commands(std::istream& is) {
    auto res = read_commands_checked(is);
    return res.commands;
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
