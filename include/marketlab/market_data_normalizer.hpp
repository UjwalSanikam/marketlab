#pragma once

#include <vector>
#include <string>
#include <istream>
#include <ostream>
#include <variant>
#include <optional>

#include "market_data_types.hpp"
#include "json.hpp"

namespace marketlab {

using NormalizedEvent = std::variant<NormalizedSnapshot, NormalizedDepthUpdate, NormalizedTrade, NormalizedFeedStatus>;

class MarketDataNormalizer {
public:
    explicit MarketDataNormalizer(InstrumentMetadata meta = {}) : meta_(std::move(meta)) {}

    // Parse raw capture JSONL stream and produce chronological normalized events
    [[nodiscard]] std::vector<NormalizedEvent> normalize_stream(std::istream& is, std::vector<std::string>* out_errors = nullptr) const;

    // Serialize normalized event to JSON
    [[nodiscard]] static std::string to_json(const NormalizedEvent& ev);

    // Parse single line of normalized event
    [[nodiscard]] static std::optional<NormalizedEvent> parse_normalized_line(const std::string& line, std::string* out_err = nullptr);

    // Read full stream of pre-normalized events
    [[nodiscard]] static std::vector<NormalizedEvent> read_normalized_stream(std::istream& is);

    // Write normalized events to stream (in binary LF mode)
    static void write_normalized_stream(std::ostream& os, const std::vector<NormalizedEvent>& events);

private:
    InstrumentMetadata meta_;
};

} // namespace marketlab
