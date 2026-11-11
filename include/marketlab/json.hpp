#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <unordered_set>
#include <optional>
#include <sstream>
#include <charconv>
#include <cstdint>
#include <cctype>

namespace marketlab {

class JsonValue {
public:
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    JsonValue() : type_(Type::Null) {}
    explicit JsonValue(bool b) : type_(Type::Bool), bool_val_(b) {}
    explicit JsonValue(std::string s) : type_(Type::String), str_val_(std::move(s)) {}

    static JsonValue make_number(std::string raw, bool is_fp) {
        JsonValue v;
        v.type_ = Type::Number;
        v.str_val_ = std::move(raw);
        v.is_floating_point_ = is_fp;
        return v;
    }

    static JsonValue make_array(std::vector<JsonValue> arr) {
        JsonValue v;
        v.type_ = Type::Array;
        v.arr_val_ = std::move(arr);
        return v;
    }

    static JsonValue make_object(std::vector<std::pair<std::string, JsonValue>> obj) {
        JsonValue v;
        v.type_ = Type::Object;
        v.obj_val_ = std::move(obj);
        return v;
    }

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] bool is_floating_point() const noexcept { return is_floating_point_; }

    [[nodiscard]] bool as_bool() const { return bool_val_; }
    [[nodiscard]] const std::string& as_string() const { return str_val_; }
    [[nodiscard]] const std::string& raw_number() const { return str_val_; }
    [[nodiscard]] const std::vector<JsonValue>& as_array() const { return arr_val_; }
    [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& as_object() const { return obj_val_; }

    [[nodiscard]] std::string type_name() const {
        switch (type_) {
            case Type::Null: return "null";
            case Type::Bool: return "boolean";
            case Type::Number: return is_floating_point_ ? "floating-point number" : "integer";
            case Type::String: return "string";
            case Type::Array: return "array";
            case Type::Object: return "object";
        }
        return "unknown";
    }

    [[nodiscard]] const JsonValue* find(const std::string& key) const {
        if (type_ != Type::Object) return nullptr;
        for (const auto& [k, v] : obj_val_) {
            if (k == key) return &v;
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<int64_t> as_int64() const {
        if (type_ != Type::Number || is_floating_point_) return std::nullopt;
        int64_t res{};
        auto [ptr, ec] = std::from_chars(str_val_.data(), str_val_.data() + str_val_.size(), res);
        if (ec != std::errc{} || ptr != str_val_.data() + str_val_.size()) {
            return std::nullopt;
        }
        return res;
    }

    [[nodiscard]] std::optional<uint64_t> as_uint64() const {
        if (type_ != Type::Number || is_floating_point_ || str_val_.empty() || str_val_.front() == '-') {
            return std::nullopt;
        }
        uint64_t res{};
        auto [ptr, ec] = std::from_chars(str_val_.data(), str_val_.data() + str_val_.size(), res);
        if (ec != std::errc{} || ptr != str_val_.data() + str_val_.size()) {
            return std::nullopt;
        }
        return res;
    }

private:
    Type type_{Type::Null};
    bool bool_val_{false};
    bool is_floating_point_{false};
    std::string str_val_;
    std::vector<JsonValue> arr_val_;
    std::vector<std::pair<std::string, JsonValue>> obj_val_;
};

struct JsonParseResult {
    bool ok{false};
    JsonValue value;
    std::string error;
    size_t line{1};
    size_t column{1};
};

class JsonParser {
public:
    [[nodiscard]] static JsonParseResult parse(std::string_view src) {
        JsonParser p(src);
        return p.parse_document();
    }

private:
    explicit JsonParser(std::string_view src) : src_(src) {}

    JsonParseResult parse_document() {
        skip_whitespace();
        if (pos_ >= src_.size()) {
            return error("Empty JSON document (no content)");
        }
        auto val = parse_value();
        if (!val.has_value()) {
            return error(last_error_);
        }
        skip_whitespace();
        if (pos_ < src_.size()) {
            return error("Unexpected trailing characters after JSON document: '" +
                         std::string(src_.substr(pos_, std::min<size_t>(20, src_.size() - pos_))) + "'");
        }
        return JsonParseResult{true, std::move(*val), "", 1, 1};
    }

    std::optional<JsonValue> parse_value() {
        skip_whitespace();
        if (pos_ >= src_.size()) {
            fail("Unexpected end of input while expecting JSON value");
            return std::nullopt;
        }

        char c = src_[pos_];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return parse_string();
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();

        fail(std::string("Unexpected character '") + c + "' at start of JSON value");
        return std::nullopt;
    }

    std::optional<JsonValue> parse_object() {
        advance(); // consume '{'
        skip_whitespace();

        std::vector<std::pair<std::string, JsonValue>> fields;
        std::set<std::string> seen_keys;

        if (pos_ < src_.size() && src_[pos_] == '}') {
            advance(); // empty object
            return JsonValue::make_object(std::move(fields));
        }

        while (pos_ < src_.size()) {
            skip_whitespace();
            if (pos_ >= src_.size()) {
                fail("Unexpected end of input inside JSON object");
                return std::nullopt;
            }

            if (src_[pos_] != '"') {
                fail("Expected string key in JSON object, found '" + std::string(1, src_[pos_]) + "'");
                return std::nullopt;
            }

            auto key_val = parse_string();
            if (!key_val.has_value()) return std::nullopt;
            std::string key = key_val->as_string();

            if (seen_keys.count(key) > 0) {
                fail("Duplicate key '" + key + "' in JSON object");
                return std::nullopt;
            }
            seen_keys.insert(key);

            skip_whitespace();
            if (pos_ >= src_.size() || src_[pos_] != ':') {
                fail("Expected ':' after object key '" + key + "'");
                return std::nullopt;
            }
            advance(); // consume ':'

            skip_whitespace();
            auto val = parse_value();
            if (!val.has_value()) return std::nullopt;

            fields.emplace_back(std::move(key), std::move(*val));

            skip_whitespace();
            if (pos_ >= src_.size()) {
                fail("Unterminated JSON object, expected ',' or '}'");
                return std::nullopt;
            }

            if (src_[pos_] == '}') {
                advance(); // end of object
                return JsonValue::make_object(std::move(fields));
            }

            if (src_[pos_] == ',') {
                advance(); // consume ','
                skip_whitespace();
                if (pos_ < src_.size() && src_[pos_] == '}') {
                    fail("Trailing comma in JSON object is not permitted");
                    return std::nullopt;
                }
            } else {
                fail("Expected ',' or '}' in JSON object, found '" + std::string(1, src_[pos_]) + "'");
                return std::nullopt;
            }
        }

        fail("Unterminated JSON object");
        return std::nullopt;
    }

    std::optional<JsonValue> parse_array() {
        advance(); // consume '['
        skip_whitespace();

        std::vector<JsonValue> items;
        if (pos_ < src_.size() && src_[pos_] == ']') {
            advance(); // empty array
            return JsonValue::make_array(std::move(items));
        }

        while (pos_ < src_.size()) {
            skip_whitespace();
            auto item = parse_value();
            if (!item.has_value()) return std::nullopt;
            items.push_back(std::move(*item));

            skip_whitespace();
            if (pos_ >= src_.size()) {
                fail("Unterminated JSON array, expected ',' or ']'");
                return std::nullopt;
            }

            if (src_[pos_] == ']') {
                advance(); // end of array
                return JsonValue::make_array(std::move(items));
            }

            if (src_[pos_] == ',') {
                advance(); // consume ','
                skip_whitespace();
                if (pos_ < src_.size() && src_[pos_] == ']') {
                    fail("Trailing comma in JSON array is not permitted");
                    return std::nullopt;
                }
            } else {
                fail("Expected ',' or ']' in JSON array, found '" + std::string(1, src_[pos_]) + "'");
                return std::nullopt;
            }
        }

        fail("Unterminated JSON array");
        return std::nullopt;
    }

    std::optional<JsonValue> parse_string() {
        advance(); // consume opening '"'
        std::string s;

        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == '"') {
                advance(); // closing quote
                return JsonValue(std::move(s));
            }

            if (static_cast<unsigned char>(c) < 0x20) {
                fail("Unescaped control character in JSON string");
                return std::nullopt;
            }

            if (c == '\\') {
                advance();
                if (pos_ >= src_.size()) {
                    fail("Unfinished escape sequence in JSON string");
                    return std::nullopt;
                }
                char esc = src_[pos_];
                switch (esc) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 >= src_.size()) {
                            fail("Incomplete \\u unicode escape sequence");
                            return std::nullopt;
                        }
                        uint32_t codepoint = 0;
                        for (size_t i = 1; i <= 4; ++i) {
                            char h = src_[pos_ + i];
                            codepoint <<= 4;
                            if (h >= '0' && h <= '9') codepoint |= (h - '0');
                            else if (h >= 'a' && h <= 'f') codepoint |= (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') codepoint |= (h - 'A' + 10);
                            else {
                                fail("Invalid hex digit in \\u unicode escape sequence");
                                return std::nullopt;
                            }
                        }
                        pos_ += 4; // consumed 4 hex digits
                        // UTF-8 encode codepoint
                        if (codepoint <= 0x7F) {
                            s += static_cast<char>(codepoint);
                        } else if (codepoint <= 0x7FF) {
                            s += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
                            s += static_cast<char>(0x80 | (codepoint & 0x3F));
                        } else {
                            s += static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
                            s += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                            s += static_cast<char>(0x80 | (codepoint & 0x3F));
                        }
                        break;
                    }
                    default:
                        fail(std::string("Invalid escape character '\\") + esc + "' in JSON string");
                        return std::nullopt;
                }
                advance();
            } else {
                s += c;
                advance();
            }
        }

        fail("Unterminated JSON string");
        return std::nullopt;
    }

    std::optional<JsonValue> parse_number() {
        size_t start = pos_;
        bool is_fp = false;

        if (src_[pos_] == '-') {
            advance();
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                fail("Invalid negative number format");
                return std::nullopt;
            }
        }

        if (src_[pos_] == '0') {
            advance();
            if (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                fail("Leading zeroes are not permitted in JSON numbers");
                return std::nullopt;
            }
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                advance();
            }
        } else {
            fail("Invalid JSON number format");
            return std::nullopt;
        }

        // Fractional part
        if (pos_ < src_.size() && src_[pos_] == '.') {
            is_fp = true;
            advance();
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                fail("Number fraction must contain at least one digit after '.'");
                return std::nullopt;
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                advance();
            }
        }

        // Exponent part
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            is_fp = true;
            advance();
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                advance();
            }
            if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                fail("Exponent must contain at least one digit");
                return std::nullopt;
            }
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                advance();
            }
        }

        std::string raw = std::string(src_.substr(start, pos_ - start));
        return JsonValue::make_number(std::move(raw), is_fp);
    }

    std::optional<JsonValue> parse_bool() {
        if (src_.substr(pos_, 4) == "true") {
            advance_n(4);
            return JsonValue(true);
        }
        if (src_.substr(pos_, 5) == "false") {
            advance_n(5);
            return JsonValue(false);
        }
        fail("Invalid literal while expecting boolean ('true' or 'false')");
        return std::nullopt;
    }

    std::optional<JsonValue> parse_null() {
        if (src_.substr(pos_, 4) == "null") {
            advance_n(4);
            return JsonValue();
        }
        fail("Invalid literal while expecting 'null'");
        return std::nullopt;
    }

    void advance() {
        if (pos_ < src_.size()) {
            if (src_[pos_] == '\n') {
                ++line_;
                col_ = 1;
            } else {
                ++col_;
            }
            ++pos_;
        }
    }

    void advance_n(size_t n) {
        for (size_t i = 0; i < n; ++i) advance();
    }

    void skip_whitespace() {
        while (pos_ < src_.size()) {
            char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else {
                break;
            }
        }
    }

    void fail(std::string msg) {
        last_error_ = "Parse error at line " + std::to_string(line_) +
                      ", column " + std::to_string(col_) + ": " + std::move(msg);
    }

    JsonParseResult error(std::string msg) {
        return JsonParseResult{false, {}, std::move(msg), line_, col_};
    }

    std::string_view src_;
    size_t pos_{0};
    size_t line_{1};
    size_t col_{1};
    std::string last_error_;
};

} // namespace marketlab
