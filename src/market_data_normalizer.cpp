#include "marketlab/market_data_normalizer.hpp"
#include <sstream>

namespace marketlab {

namespace {

void write_levels_json(std::ostream& os, const std::vector<LevelUpdate>& levels) {
    os << "[";
    for (size_t i = 0; i < levels.size(); ++i) {
        if (i > 0) os << ",";
        os << "[" << levels[i].price << "," << levels[i].quantity << "]";
    }
    os << "]";
}

bool parse_levels(const JsonValue* arr, int price_scale, int qty_scale, std::vector<LevelUpdate>& out) {
    if (!arr || !arr->is_array()) return false;
    for (const auto& item : arr->as_array()) {
        if (!item.is_array() || item.as_array().size() < 2) return false;
        std::string p_str = item.as_array()[0].is_string() ? item.as_array()[0].as_string() : item.as_array()[0].raw_number();
        std::string q_str = item.as_array()[1].is_string() ? item.as_array()[1].as_string() : item.as_array()[1].raw_number();

        auto p = DecimalParser::parse_decimal(p_str, price_scale);
        auto q = DecimalParser::parse_decimal(q_str, qty_scale);
        if (!p || !q) return false;

        out.push_back(LevelUpdate{*p, *q});
    }
    return true;
}

} // namespace

std::vector<NormalizedEvent> MarketDataNormalizer::normalize_stream(std::istream& is, std::vector<std::string>* out_errors) const {
    std::vector<NormalizedEvent> events;
    std::string line;
    size_t line_no = 0;

    while (std::getline(is, line)) {
        ++line_no;
        if (line.empty()) continue;
        if (line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        auto parse_res = JsonParser::parse(line);
        if (!parse_res.ok || !parse_res.value.is_object()) {
            if (out_errors) out_errors->push_back("Line " + std::to_string(line_no) + ": Invalid JSON record");
            continue;
        }

        const auto& root = parse_res.value;
        const auto* rec_type_val = root.find("record_type");
        if (!rec_type_val || !rec_type_val->is_string()) continue;
        std::string rec_type = rec_type_val->as_string();

        SimTime mono_ns = 0;
        if (const auto* m_val = root.find("recv_mono_ns")) {
            if (auto val = m_val->as_uint64()) mono_ns = *val;
        }

        if (rec_type == "SNAPSHOT") {
            const auto* payload = root.find("payload");
            if (!payload || !payload->is_object()) continue;

            uint64_t last_update_id = 0;
            if (const auto* l_val = payload->find("lastUpdateId")) {
                if (auto val = l_val->as_uint64()) last_update_id = *val;
            }

            std::vector<LevelUpdate> bids, asks;
            if (!parse_levels(payload->find("bids"), meta_.price_scale, meta_.qty_scale, bids) ||
                !parse_levels(payload->find("asks"), meta_.price_scale, meta_.qty_scale, asks)) {
                if (out_errors) out_errors->push_back("Line " + std::to_string(line_no) + ": Failed parsing snapshot price levels");
                continue;
            }

            events.push_back(NormalizedSnapshot{mono_ns, last_update_id, std::move(bids), std::move(asks)});

        } else if (rec_type == "STREAM_MESSAGE") {
            const auto* payload = root.find("payload");
            if (!payload || !payload->is_object()) continue;

            const auto* data = payload->find("data");
            if (!data || !data->is_object()) {
                data = payload; // Direct stream event
            }

            const auto* e_val = data->find("e");
            if (!e_val || !e_val->is_string()) continue;
            std::string event_type = e_val->as_string();

            uint64_t event_time_ms = 0;
            if (const auto* e_ts = data->find("E")) {
                if (auto val = e_ts->as_uint64()) event_time_ms = *val;
            }

            if (event_type == "depthUpdate") {
                uint64_t first_u = 0, final_u = 0;
                if (const auto* u1 = data->find("U")) if (auto val = u1->as_uint64()) first_u = *val;
                if (const auto* u2 = data->find("u")) if (auto val = u2->as_uint64()) final_u = *val;

                std::vector<LevelUpdate> bids, asks;
                parse_levels(data->find("b"), meta_.price_scale, meta_.qty_scale, bids);
                parse_levels(data->find("a"), meta_.price_scale, meta_.qty_scale, asks);

                events.push_back(NormalizedDepthUpdate{mono_ns, event_time_ms, first_u, final_u, std::move(bids), std::move(asks)});

            } else if (event_type == "trade") {
                uint64_t trade_id = 0;
                if (const auto* t_val = data->find("t")) if (auto val = t_val->as_uint64()) trade_id = *val;

                std::string p_str, q_str;
                if (const auto* p_val = data->find("p")) p_str = p_val->is_string() ? p_val->as_string() : p_val->raw_number();
                if (const auto* q_val = data->find("q")) q_str = q_val->is_string() ? q_val->as_string() : q_val->raw_number();

                bool is_buyer_maker = false;
                if (const auto* m_val = data->find("m")) is_buyer_maker = m_val->as_bool();

                auto p = DecimalParser::parse_decimal(p_str, meta_.price_scale);
                auto q = DecimalParser::parse_decimal(q_str, meta_.qty_scale);
                if (p && q) {
                    Side taker_side = is_buyer_maker ? Side::Sell : Side::Buy;
                    events.push_back(NormalizedTrade{mono_ns, event_time_ms, trade_id, *p, *q, taker_side});
                }
            }

        } else if (rec_type == "DATA_LOSS") {
            std::string reason = "DATA_LOSS";
            if (const auto* p = root.find("payload")) {
                if (const auto* r = p->find("reason")) reason = r->as_string();
            }
            events.push_back(NormalizedFeedStatus{mono_ns, FeedStatus::Invalid, reason});

        } else if (rec_type == "CONNECTION_EVENT") {
            if (const auto* p = root.find("payload")) {
                if (const auto* a = p->find("action")) {
                    if (a->as_string() == "DISCONNECTED") {
                        events.push_back(NormalizedFeedStatus{mono_ns, FeedStatus::Invalid, "DISCONNECTED"});
                    }
                }
            }
        }
    }

    return events;
}

std::string MarketDataNormalizer::to_json(const NormalizedEvent& ev) {
    std::ostringstream ss;
    std::visit([&ss](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, NormalizedSnapshot>) {
            ss << "{\"type\":\"SNAPSHOT\",\"time\":" << e.local_receipt_time_ns
               << ",\"last_update_id\":" << e.last_update_id
               << ",\"bids\":";
            write_levels_json(ss, e.bids);
            ss << ",\"asks\":";
            write_levels_json(ss, e.asks);
            ss << "}";
        } else if constexpr (std::is_same_v<T, NormalizedDepthUpdate>) {
            ss << "{\"type\":\"DEPTH_UPDATE\",\"time\":" << e.local_receipt_time_ns
               << ",\"exchange_time\":" << e.exchange_time_ms
               << ",\"U\":" << e.first_update_id
               << ",\"u\":" << e.final_update_id
               << ",\"bids\":";
            write_levels_json(ss, e.bids);
            ss << ",\"asks\":";
            write_levels_json(ss, e.asks);
            ss << "}";
        } else if constexpr (std::is_same_v<T, NormalizedTrade>) {
            ss << "{\"type\":\"PUBLIC_TRADE\",\"time\":" << e.local_receipt_time_ns
               << ",\"exchange_time\":" << e.exchange_time_ms
               << ",\"trade_id\":" << e.trade_id
               << ",\"price\":" << e.price
               << ",\"qty\":" << e.quantity
               << ",\"taker_side\":\"" << to_string(e.taker_side) << "\"}";
        } else if constexpr (std::is_same_v<T, NormalizedFeedStatus>) {
            ss << "{\"type\":\"FEED_STATUS\",\"time\":" << e.local_receipt_time_ns
               << ",\"status\":\"" << to_string(e.status) << "\""
               << ",\"reason\":\"" << e.reason << "\"}";
        }
    }, ev);
    return ss.str();
}

std::optional<NormalizedEvent> MarketDataNormalizer::parse_normalized_line(const std::string& line, std::string* out_err) {
    auto parse_res = JsonParser::parse(line);
    if (!parse_res.ok || !parse_res.value.is_object()) {
        if (out_err) *out_err = "Malformed JSON line";
        return std::nullopt;
    }

    const auto& root = parse_res.value;
    const auto* type_val = root.find("type");
    if (!type_val || !type_val->is_string()) {
        if (out_err) *out_err = "Missing 'type' field in normalized line";
        return std::nullopt;
    }
    std::string type = type_val->as_string();

    SimTime time = 0;
    if (const auto* t = root.find("time")) if (auto val = t->as_uint64()) time = *val;

    if (type == "SNAPSHOT") {
        uint64_t last_update_id = 0;
        if (const auto* l = root.find("last_update_id")) if (auto val = l->as_uint64()) last_update_id = *val;

        std::vector<LevelUpdate> bids, asks;
        auto parse_raw_levels = [](const JsonValue* arr, std::vector<LevelUpdate>& out) {
            if (!arr || !arr->is_array()) return;
            for (const auto& item : arr->as_array()) {
                if (item.is_array() && item.as_array().size() >= 2) {
                    auto p = item.as_array()[0].as_int64();
                    auto q = item.as_array()[1].as_int64();
                    if (p && q) out.push_back(LevelUpdate{*p, *q});
                }
            }
        };
        parse_raw_levels(root.find("bids"), bids);
        parse_raw_levels(root.find("asks"), asks);

        return NormalizedSnapshot{time, last_update_id, std::move(bids), std::move(asks)};

    } else if (type == "DEPTH_UPDATE") {
        uint64_t ex_time = 0, first_u = 0, final_u = 0;
        if (const auto* e = root.find("exchange_time")) if (auto val = e->as_uint64()) ex_time = *val;
        if (const auto* u1 = root.find("U")) if (auto val = u1->as_uint64()) first_u = *val;
        if (const auto* u2 = root.find("u")) if (auto val = u2->as_uint64()) final_u = *val;

        std::vector<LevelUpdate> bids, asks;
        auto parse_raw_levels = [](const JsonValue* arr, std::vector<LevelUpdate>& out) {
            if (!arr || !arr->is_array()) return;
            for (const auto& item : arr->as_array()) {
                if (item.is_array() && item.as_array().size() >= 2) {
                    auto p = item.as_array()[0].as_int64();
                    auto q = item.as_array()[1].as_int64();
                    if (p && q) out.push_back(LevelUpdate{*p, *q});
                }
            }
        };
        parse_raw_levels(root.find("bids"), bids);
        parse_raw_levels(root.find("asks"), asks);

        return NormalizedDepthUpdate{time, ex_time, first_u, final_u, std::move(bids), std::move(asks)};

    } else if (type == "PUBLIC_TRADE") {
        uint64_t ex_time = 0, trade_id = 0;
        Price p = 0;
        Quantity q = 0;
        Side side = Side::Buy;

        if (const auto* e = root.find("exchange_time")) if (auto val = e->as_uint64()) ex_time = *val;
        if (const auto* t = root.find("trade_id")) if (auto val = t->as_uint64()) trade_id = *val;
        if (const auto* pv = root.find("price")) if (auto val = pv->as_int64()) p = *val;
        if (const auto* qv = root.find("qty")) if (auto val = qv->as_int64()) q = *val;
        if (const auto* sv = root.find("taker_side")) {
            if (sv->as_string() == "SELL") side = Side::Sell;
        }

        return NormalizedTrade{time, ex_time, trade_id, p, q, side};

    } else if (type == "FEED_STATUS") {
        FeedStatus status = FeedStatus::Valid;
        std::string reason;
        if (const auto* s = root.find("status")) {
            std::string st = s->as_string();
            if (st == "INVALID") status = FeedStatus::Invalid;
            else if (st == "SYNCHRONIZING") status = FeedStatus::Synchronizing;
            else if (st == "UNINITIALIZED") status = FeedStatus::Uninitialized;
        }
        if (const auto* r = root.find("reason")) reason = r->as_string();

        return NormalizedFeedStatus{time, status, reason};
    }

    if (out_err) *out_err = "Unknown normalized event type: " + type;
    return std::nullopt;
}

std::vector<NormalizedEvent> MarketDataNormalizer::read_normalized_stream(std::istream& is) {
    std::vector<NormalizedEvent> events;
    std::string line;
    while (std::getline(is, line)) {
        if (line.empty()) continue;
        if (line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto ev = parse_normalized_line(line);
        if (ev) events.push_back(std::move(*ev));
    }
    return events;
}

void MarketDataNormalizer::write_normalized_stream(std::ostream& os, const std::vector<NormalizedEvent>& events) {
    for (const auto& ev : events) {
        std::string s = to_json(ev);
        os.write(s.data(), s.size());
        os.put('\n'); // Explicit LF byte 0x0A
    }
    os.flush();
}

} // namespace marketlab
