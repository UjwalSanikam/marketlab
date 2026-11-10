#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <iostream>
#include <optional>
#include <charconv>
#include <limits>
#include <cctype>

#include "types.hpp"
#include "commands.hpp"
#include "events.hpp"
#include "order_book.hpp"

namespace marketlab {

struct ParseError {
    size_t line_number{0};
    std::string line_content;
    std::string message;
};

struct ReadCommandsResult {
    bool success{true};
    std::vector<Command> commands;
    std::optional<ParseError> error;
};

class Serializer {
public:
    // Strictly parse integer without allowing any trailing junk, floating point, or negative for unsigned
    template <typename T>
    [[nodiscard]] static std::optional<T> parse_strict_integer(std::string_view sv) noexcept {
        // Trim leading and trailing whitespace
        while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
            sv.remove_prefix(1);
        }
        while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
            sv.remove_suffix(1);
        }
        if (sv.empty()) return std::nullopt;

        // Disallow negative signs for unsigned types
        if constexpr (std::is_unsigned_v<T>) {
            if (sv.front() == '-') return std::nullopt;
            if (sv.front() == '+') sv.remove_prefix(1);
        }

        T result{};
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
        if (ec != std::errc{} || ptr != sv.data() + sv.size()) {
            return std::nullopt; // Trailing junk or overflow/underflow
        }
        return result;
    }

    // Format single command to JSON string
    [[nodiscard]] static std::string to_json(const Command& cmd);

    // Format single event to JSON string
    [[nodiscard]] static std::string to_json(const Event& ev);

    // Format book snapshot to JSON string
    [[nodiscard]] static std::string to_json(const BookSnapshot& snap);

    // Format book snapshot to formatted tabular ASCII string for human inspection
    [[nodiscard]] static std::string to_ascii_book(const BookSnapshot& snap);

    // Parse single line of command (JSON format or space-delimited format)
    [[nodiscard]] static std::optional<Command> parse_command_line(const std::string& line, std::string* out_error = nullptr);

    // Read full command stream from input stream with line number and error tracking
    [[nodiscard]] static ReadCommandsResult read_commands_checked(std::istream& is);

    // Convenience wrapper for legacy calls (throws or returns partial on success)
    [[nodiscard]] static std::vector<Command> read_commands(std::istream& is);

    // Write command stream to output stream
    static void write_commands(std::ostream& os, const std::vector<Command>& commands);

    // Write event stream to output stream
    static void write_events(std::ostream& os, const std::vector<Event>& events);
};

} // namespace marketlab
