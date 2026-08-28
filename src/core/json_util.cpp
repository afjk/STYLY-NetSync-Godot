// SPDX-License-Identifier: Apache-2.0
#include "json_util.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace styly {
namespace netsync {

namespace {

void append_utf8(std::string &out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}

std::string format_number(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    if (value == static_cast<double>(static_cast<std::int64_t>(value)) &&
        std::fabs(value) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld",
                      static_cast<long long>(static_cast<std::int64_t>(value)));
        return buffer;
    }
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

}  // namespace

std::string encode_json_string(const std::string &value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (unsigned char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else {
                    // Non-ASCII bytes pass through unescaped, matching
                    // Newtonsoft's StringEscapeHandling.Default.
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string encode_json_string_array(const std::vector<std::string> &values) {
    std::string out;
    out.push_back('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out += encode_json_string(values[i]);
    }
    out.push_back(']');
    return out;
}

// --- Parser -----------------------------------------------------------------

class JsonParser {
public:
    JsonParser(const std::string &text) : text_(text) {}

    bool parse(JsonValue &out, std::string *error) {
        skip_whitespace();
        if (!parse_value(out, 0)) {
            if (error != nullptr) {
                *error = error_;
            }
            return false;
        }
        skip_whitespace();
        if (index_ != text_.size()) {
            if (error != nullptr) {
                *error = "trailing characters after JSON value";
            }
            return false;
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 64;

    bool fail(const char *message) {
        if (error_.empty()) {
            error_ = message;
        }
        return false;
    }

    void skip_whitespace() {
        while (index_ < text_.size()) {
            const char c = text_[index_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++index_;
            } else {
                break;
            }
        }
    }

    bool literal(const char *word) {
        const std::size_t length = std::char_traits<char>::length(word);
        if (text_.compare(index_, length, word) != 0) {
            return false;
        }
        index_ += length;
        return true;
    }

    bool parse_value(JsonValue &out, int depth) {
        if (depth > kMaxDepth) {
            return fail("maximum nesting depth exceeded");
        }
        skip_whitespace();
        if (index_ >= text_.size()) {
            return fail("unexpected end of input");
        }
        const char c = text_[index_];
        switch (c) {
            case '{':
                return parse_object(out, depth);
            case '[':
                return parse_array(out, depth);
            case '"': {
                std::string value;
                if (!parse_string(value)) {
                    return false;
                }
                out.type_ = JsonValue::Type::String;
                out.string_ = std::move(value);
                return true;
            }
            case 't':
                if (!literal("true")) {
                    return fail("invalid literal");
                }
                out.type_ = JsonValue::Type::Bool;
                out.bool_ = true;
                return true;
            case 'f':
                if (!literal("false")) {
                    return fail("invalid literal");
                }
                out.type_ = JsonValue::Type::Bool;
                out.bool_ = false;
                return true;
            case 'n':
                if (!literal("null")) {
                    return fail("invalid literal");
                }
                out.type_ = JsonValue::Type::Null;
                return true;
            default:
                return parse_number(out);
        }
    }

    bool parse_number(JsonValue &out) {
        const std::size_t start = index_;
        if (index_ < text_.size() && (text_[index_] == '-' || text_[index_] == '+')) {
            ++index_;
        }
        bool has_digits = false;
        while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
            ++index_;
            has_digits = true;
        }
        if (index_ < text_.size() && text_[index_] == '.') {
            ++index_;
            while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
                ++index_;
                has_digits = true;
            }
        }
        if (has_digits && index_ < text_.size() && (text_[index_] == 'e' || text_[index_] == 'E')) {
            ++index_;
            if (index_ < text_.size() && (text_[index_] == '-' || text_[index_] == '+')) {
                ++index_;
            }
            while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
                ++index_;
            }
        }
        if (!has_digits) {
            return fail("invalid number");
        }
        out.type_ = JsonValue::Type::Number;
        out.number_ = std::strtod(text_.substr(start, index_ - start).c_str(), nullptr);
        return true;
    }

    bool parse_string(std::string &out) {
        if (index_ >= text_.size() || text_[index_] != '"') {
            return fail("expected string");
        }
        ++index_;
        out.clear();
        while (index_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[index_]);
            if (c == '"') {
                ++index_;
                return true;
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++index_;
                continue;
            }
            ++index_;
            if (index_ >= text_.size()) {
                return fail("unterminated escape");
            }
            const char escape = text_[index_++];
            switch (escape) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t code_point = 0;
                    if (!parse_hex4(code_point)) {
                        return false;
                    }
                    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                        // High surrogate: consume the matching low surrogate.
                        if (index_ + 1 < text_.size() && text_[index_] == '\\' &&
                            text_[index_ + 1] == 'u') {
                            const std::size_t saved = index_;
                            index_ += 2;
                            std::uint32_t low = 0;
                            if (parse_hex4(low) && low >= 0xDC00 && low <= 0xDFFF) {
                                code_point = 0x10000 + ((code_point - 0xD800) << 10) +
                                             (low - 0xDC00);
                            } else {
                                index_ = saved;
                            }
                        }
                    }
                    append_utf8(out, code_point);
                    break;
                }
                default:
                    return fail("invalid escape sequence");
            }
        }
        return fail("unterminated string");
    }

    bool parse_hex4(std::uint32_t &out) {
        if (index_ + 4 > text_.size()) {
            return fail("truncated \\u escape");
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[index_ + static_cast<std::size_t>(i)];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("invalid hex digit in \\u escape");
            }
        }
        index_ += 4;
        out = value;
        return true;
    }

    bool parse_array(JsonValue &out, int depth) {
        ++index_;  // '['
        out.type_ = JsonValue::Type::Array;
        out.array_.clear();
        skip_whitespace();
        if (index_ < text_.size() && text_[index_] == ']') {
            ++index_;
            return true;
        }
        while (true) {
            JsonValue element;
            if (!parse_value(element, depth + 1)) {
                return false;
            }
            out.array_.push_back(std::move(element));
            skip_whitespace();
            if (index_ >= text_.size()) {
                return fail("unterminated array");
            }
            if (text_[index_] == ',') {
                ++index_;
                continue;
            }
            if (text_[index_] == ']') {
                ++index_;
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }

    bool parse_object(JsonValue &out, int depth) {
        ++index_;  // '{'
        out.type_ = JsonValue::Type::Object;
        out.object_.clear();
        skip_whitespace();
        if (index_ < text_.size() && text_[index_] == '}') {
            ++index_;
            return true;
        }
        while (true) {
            skip_whitespace();
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_whitespace();
            if (index_ >= text_.size() || text_[index_] != ':') {
                return fail("expected ':' in object");
            }
            ++index_;
            JsonValue value;
            if (!parse_value(value, depth + 1)) {
                return false;
            }
            out.object_[key] = std::move(value);
            skip_whitespace();
            if (index_ >= text_.size()) {
                return fail("unterminated object");
            }
            if (text_[index_] == ',') {
                ++index_;
                continue;
            }
            if (text_[index_] == '}') {
                ++index_;
                return true;
            }
            return fail("expected ',' or '}' in object");
        }
    }

    const std::string &text_;
    std::size_t index_ = 0;
    std::string error_;
};

const JsonValue &JsonValue::null_value() {
    static const JsonValue value;
    return value;
}

const JsonValue &JsonValue::operator[](const std::string &key) const {
    const auto it = object_.find(key);
    return it == object_.end() ? null_value() : it->second;
}

const JsonValue &JsonValue::operator[](std::size_t index) const {
    return index < array_.size() ? array_[index] : null_value();
}

std::string JsonValue::dump() const {
    switch (type_) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return bool_ ? "true" : "false";
        case Type::Number:
            return format_number(number_);
        case Type::String:
            return encode_json_string(string_);
        case Type::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < array_.size(); ++i) {
                if (i != 0) {
                    out.push_back(',');
                }
                out += array_[i].dump();
            }
            out.push_back(']');
            return out;
        }
        case Type::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto &entry : object_) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                out += encode_json_string(entry.first);
                out.push_back(':');
                out += entry.second.dump();
            }
            out.push_back('}');
            return out;
        }
    }
    return "null";
}

bool JsonValue::parse(const std::string &text, JsonValue &out, std::string *error) {
    JsonParser parser(text);
    return parser.parse(out, error);
}

bool decode_json_string_array(const std::string &json, std::vector<std::string> &out) {
    out.clear();
    JsonValue value;
    if (!JsonValue::parse(json, value) || !value.is_array()) {
        return false;
    }
    out.reserve(value.array_items().size());
    for (const JsonValue &element : value.array_items()) {
        switch (element.type()) {
            case JsonValue::Type::String:
                out.push_back(element.string_value());
                break;
            case JsonValue::Type::Null:
                out.push_back(std::string());
                break;
            default:
                out.push_back(element.dump());
                break;
        }
    }
    return true;
}

}  // namespace netsync
}  // namespace styly
