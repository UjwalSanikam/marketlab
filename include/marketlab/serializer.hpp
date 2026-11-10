#pragma once

#include <string>
#include <vector>
#include <iostream>
#include <optional>

#include "types.hpp"
#include "commands.hpp"
#include "events.hpp"
#include "order_book.hpp"

namespace marketlab {

class Serializer {
public:
    // Format single command to JSON string
    [[nodiscard]] static std::string to_json(const Command& cmd);

    // Format single event to JSON string
    [[nodiscard]] static std::string to_json(const Event& ev);

    // Format book snapshot to JSON string
    [[nodiscard]] static std::string to_json(const BookSnapshot& snap);

    // Format book snapshot to formatted tabular ASCII string for human inspection
    [[nodiscard]] static std::string to_ascii_book(const BookSnapshot& snap);

    // Parse single line of command (JSON format or space/comma-separated format)
    [[nodiscard]] static std::optional<Command> parse_command_line(const std::string& line);

    // Read full command stream from input stream
    [[nodiscard]] static std::vector<Command> read_commands(std::istream& is);

    // Write command stream to output stream
    static void write_commands(std::ostream& os, const std::vector<Command>& commands);

    // Write event stream to output stream
    static void write_events(std::ostream& os, const std::vector<Event>& events);
};

} // namespace marketlab
