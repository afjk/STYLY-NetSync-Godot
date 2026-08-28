// SPDX-License-Identifier: Apache-2.0
// Minimal JSON support for the NetSync core.
//
// Needed because RPC arguments travel as a JSON array of strings produced by
// Newtonsoft's `JsonConvert.SerializeObject(string[])` on the Unity side. The
// encoder here reproduces Newtonsoft's default escaping so a Godot-originated
// RPC is byte-identical to the Unity one for the same arguments.
//
// The general parser is small on purpose: it exists to read RPC argument arrays
// and golden-vector files, not to be a general-purpose JSON library.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace styly {
namespace netsync {

/// Encode a vector of strings as a JSON array, matching Newtonsoft's default
/// `StringEscapeHandling`: escape `"`, `\` and the C0 controls (using the short
/// forms where they exist, `\u00xx` otherwise); leave `/` and non-ASCII alone.
std::string encode_json_string_array(const std::vector<std::string> &values);

/// Encode a single JSON string literal, including the surrounding quotes.
std::string encode_json_string(const std::string &value);

/// Decode a JSON array into strings. Non-string elements are stringified:
/// numbers and booleans by their literal text, `null` as an empty string,
/// nested arrays/objects by their compact JSON form. Returns false when the
/// input is not a well-formed JSON array.
bool decode_json_string_array(const std::string &json, std::vector<std::string> &out);

/// A parsed JSON value. Used by the golden-vector tests; the runtime only needs
/// the array helpers above.
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool bool_value(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
    double number_value(double fallback = 0.0) const { return is_number() ? number_ : fallback; }
    std::int64_t int_value(std::int64_t fallback = 0) const {
        return is_number() ? static_cast<std::int64_t>(number_) : fallback;
    }
    const std::string &string_value() const { return string_; }

    const std::vector<JsonValue> &array_items() const { return array_; }
    std::size_t size() const { return is_array() ? array_.size() : object_.size(); }

    /// Object member lookup. Returns a shared null value when absent.
    const JsonValue &operator[](const std::string &key) const;
    /// Array element lookup. Returns a shared null value when out of range.
    const JsonValue &operator[](std::size_t index) const;

    bool has(const std::string &key) const { return object_.find(key) != object_.end(); }

    /// Compact re-serialisation, used when stringifying non-string RPC arguments.
    std::string dump() const;

    /// Parse `text`. On failure returns false and, when `error` is non-null,
    /// fills it with a short description.
    static bool parse(const std::string &text, JsonValue &out, std::string *error = nullptr);

    static const JsonValue &null_value();

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<JsonValue> array_;
    std::map<std::string, JsonValue> object_;

    friend class JsonParser;
};

}  // namespace netsync
}  // namespace styly
