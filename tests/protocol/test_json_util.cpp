// SPDX-License-Identifier: Apache-2.0
// RPC argument encoding must match Newtonsoft's
// JsonConvert.SerializeObject(string[]) so a Godot-originated RPC is
// indistinguishable from a Unity one, and decoding must tolerate everything a
// Unity or Python peer can put on the wire.

#include <string>
#include <vector>

#include "../test_support.hpp"
#include "core/json_util.hpp"

using namespace styly::netsync;
using namespace styly::netsync::test;

namespace {

void test_encode_basic_arrays() {
    CHECK_EQ(encode_json_string_array({}), std::string("[]"));
    CHECK_EQ(encode_json_string_array({"a"}), std::string("[\"a\"]"));
    CHECK_EQ(encode_json_string_array({"a", "b", "c"}), std::string("[\"a\",\"b\",\"c\"]"));
    CHECK_EQ(encode_json_string_array({""}), std::string("[\"\"]"));
}

void test_encode_escaping_matches_newtonsoft() {
    // Short escapes.
    CHECK_EQ(encode_json_string("\""), std::string("\"\\\"\""));
    CHECK_EQ(encode_json_string("\\"), std::string("\"\\\\\""));
    CHECK_EQ(encode_json_string("\n"), std::string("\"\\n\""));
    CHECK_EQ(encode_json_string("\r"), std::string("\"\\r\""));
    CHECK_EQ(encode_json_string("\t"), std::string("\"\\t\""));
    CHECK_EQ(encode_json_string("\b"), std::string("\"\\b\""));
    CHECK_EQ(encode_json_string("\f"), std::string("\"\\f\""));

    // Other C0 controls use the \u00xx long form.
    CHECK_EQ(encode_json_string(std::string(1, '\x01')), std::string("\"\\u0001\""));
    CHECK_EQ(encode_json_string(std::string(1, '\x1f')), std::string("\"\\u001f\""));

    // Newtonsoft does NOT escape the forward slash.
    CHECK_EQ(encode_json_string("a/b"), std::string("\"a/b\""));

    // Nor non-ASCII: it emits UTF-8 directly, unlike Python's json.dumps default.
    CHECK_EQ(encode_json_string("日本語"), std::string("\"日本語\""));
    CHECK_EQ(encode_json_string("café"), std::string("\"café\""));
}

void test_decode_string_arrays() {
    std::vector<std::string> out;

    CHECK(decode_json_string_array("[]", out));
    CHECK_EQ(out.size(), static_cast<std::size_t>(0));

    CHECK(decode_json_string_array("[\"a\",\"b\"]", out));
    CHECK_EQ(out.size(), static_cast<std::size_t>(2));
    CHECK_EQ(out[0], std::string("a"));
    CHECK_EQ(out[1], std::string("b"));

    // Whitespace between tokens.
    CHECK(decode_json_string_array("[ \"a\" , \"b\" ]", out));
    CHECK_EQ(out.size(), static_cast<std::size_t>(2));

    // Escapes, including \u and the unescaped forward slash.
    CHECK(decode_json_string_array("[\"line\\nbreak\",\"quote\\\"\",\"\\u00e9\",\"a\\/b\"]", out));
    CHECK_EQ(out[0], std::string("line\nbreak"));
    CHECK_EQ(out[1], std::string("quote\""));
    CHECK_EQ(out[2], std::string("é"));
    CHECK_EQ(out[3], std::string("a/b"));

    // Surrogate pairs (a Python peer with ensure_ascii=True emits these).
    CHECK(decode_json_string_array("[\"\\ud83d\\ude00\"]", out));
    CHECK_EQ(out[0], std::string("\xF0\x9F\x98\x80"));  // U+1F600
}

void test_decode_non_string_elements() {
    // A peer may send a JSON array whose elements are not strings. Upstream
    // Unity deserialises into string[], which turns numbers into their text and
    // null into null; this port stringifies rather than dropping the argument,
    // so positional arguments keep their indices.
    std::vector<std::string> out;
    CHECK(decode_json_string_array("[1,2.5,true,false,null]", out));
    CHECK_EQ(out.size(), static_cast<std::size_t>(5));
    CHECK_EQ(out[0], std::string("1"));
    CHECK_EQ(out[2], std::string("true"));
    CHECK_EQ(out[3], std::string("false"));
    CHECK_EQ(out[4], std::string(""));
}

void test_decode_rejects_malformed_input() {
    std::vector<std::string> out;
    CHECK(!decode_json_string_array("", out));
    CHECK(!decode_json_string_array("[", out));
    CHECK(!decode_json_string_array("[\"a\"", out));
    CHECK(!decode_json_string_array("{\"a\":1}", out));  // object, not array
    CHECK(!decode_json_string_array("\"a\"", out));      // bare string
    CHECK(!decode_json_string_array("[\"unterminated]", out));
    CHECK(!decode_json_string_array("[1,]", out));
    CHECK(!decode_json_string_array("[] extra", out));
}

void test_encode_decode_round_trip() {
    const std::vector<std::vector<std::string>> cases = {
        {},
        {""},
        {"simple"},
        {"a", "b", "c"},
        {"with \"quotes\"", "with \\backslash", "with\nnewline\ttab"},
        {"日本語", "émoji 😀", std::string(1, '\x01')},
        {std::string(4096, 'x')},
    };
    for (const std::vector<std::string> &original : cases) {
        std::vector<std::string> decoded;
        CHECK(decode_json_string_array(encode_json_string_array(original), decoded));
        CHECK_EQ(decoded.size(), original.size());
        for (std::size_t i = 0; i < original.size() && i < decoded.size(); ++i) {
            CHECK_EQ(decoded[i], original[i]);
        }
    }
}

void test_json_value_parser() {
    JsonValue value;
    CHECK(JsonValue::parse("{\"a\":1,\"b\":[true,null],\"c\":{\"d\":\"x\"}}", value));
    CHECK(value.is_object());
    CHECK_EQ(value["a"].int_value(), static_cast<std::int64_t>(1));
    CHECK(value["b"].is_array());
    CHECK_EQ(value["b"].size(), static_cast<std::size_t>(2));
    CHECK(value["b"][static_cast<std::size_t>(0)].bool_value());
    CHECK(value["b"][static_cast<std::size_t>(1)].is_null());
    CHECK_EQ(value["c"]["d"].string_value(), std::string("x"));

    // Missing keys and out-of-range indices yield a null value, not a crash.
    CHECK(value["missing"].is_null());
    CHECK(value["b"][static_cast<std::size_t>(99)].is_null());

    // Numbers.
    CHECK(JsonValue::parse("[-1, 2.5, 1e3, -1.5e-2]", value));
    CHECK_EQ(value[static_cast<std::size_t>(0)].int_value(), static_cast<std::int64_t>(-1));
    CHECK_NEAR(value[static_cast<std::size_t>(1)].number_value(), 2.5, 1e-12);
    CHECK_NEAR(value[static_cast<std::size_t>(2)].number_value(), 1000.0, 1e-12);
    CHECK_NEAR(value[static_cast<std::size_t>(3)].number_value(), -0.015, 1e-12);

    // Deep nesting is bounded rather than blowing the stack.
    const std::string deep(200, '[');
    CHECK(!JsonValue::parse(deep, value));
}

}  // namespace

int main() {
    test_encode_basic_arrays();
    test_encode_escaping_matches_newtonsoft();
    test_decode_string_arrays();
    test_decode_non_string_elements();
    test_decode_rejects_malformed_input();
    test_encode_decode_round_trip();
    test_json_value_parser();
    return summary("json utilities");
}
