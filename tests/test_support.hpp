// SPDX-License-Identifier: Apache-2.0
// Minimal assertion harness. No external test framework: the test binaries must
// build anywhere the library builds, including on a bare CI runner.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace styly {
namespace netsync {
namespace test {

inline int &failure_count() {
    static int count = 0;
    return count;
}

inline int &check_count() {
    static int count = 0;
    return count;
}

inline void report_failure(const char *file, int line, const std::string &message) {
    ++failure_count();
    std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, message.c_str());
}

inline std::string to_hex(const std::vector<unsigned char> &bytes) {
    static const char *digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

/// Describe the first differing byte so a golden mismatch is actionable.
inline std::string describe_diff(const std::vector<unsigned char> &actual,
                                 const std::vector<unsigned char> &expected) {
    std::string out = "\n    actual   = " + to_hex(actual) + "\n    expected = " + to_hex(expected);
    const std::size_t limit = actual.size() < expected.size() ? actual.size() : expected.size();
    for (std::size_t i = 0; i < limit; ++i) {
        if (actual[i] != expected[i]) {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "\n    first difference at byte %zu: %02x != %02x",
                          i, actual[i], expected[i]);
            out += buffer;
            return out;
        }
    }
    if (actual.size() != expected.size()) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "\n    length differs: %zu != %zu", actual.size(),
                      expected.size());
        out += buffer;
    }
    return out;
}

inline int summary(const char *suite_name) {
    if (failure_count() == 0) {
        std::printf("PASS %s (%d checks)\n", suite_name, check_count());
        return 0;
    }
    std::printf("FAIL %s (%d failures out of %d checks)\n", suite_name, failure_count(),
                check_count());
    return 1;
}

}  // namespace test
}  // namespace netsync
}  // namespace styly

#define CHECK(condition)                                                                     \
    do {                                                                                     \
        ++::styly::netsync::test::check_count();                                              \
        if (!(condition)) {                                                                   \
            ::styly::netsync::test::report_failure(__FILE__, __LINE__, "CHECK(" #condition ")"); \
        }                                                                                     \
    } while (false)

#define CHECK_MSG(condition, message)                                                   \
    do {                                                                                \
        ++::styly::netsync::test::check_count();                                         \
        if (!(condition)) {                                                              \
            ::styly::netsync::test::report_failure(__FILE__, __LINE__,                   \
                                                   std::string("CHECK(" #condition "): ") + \
                                                       (message));                      \
        }                                                                               \
    } while (false)

#define CHECK_EQ(actual, expected)                                                            \
    do {                                                                                      \
        ++::styly::netsync::test::check_count();                                                \
        const auto &check_actual_ = (actual);                                                  \
        const auto &check_expected_ = (expected);                                              \
        if (!(check_actual_ == check_expected_)) {                                             \
            ::styly::netsync::test::report_failure(                                            \
                __FILE__, __LINE__,                                                            \
                std::string("CHECK_EQ(" #actual ", " #expected ") — values differ"));         \
        }                                                                                      \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                            \
    do {                                                                                   \
        ++::styly::netsync::test::check_count();                                            \
        const double check_actual_ = static_cast<double>(actual);                           \
        const double check_expected_ = static_cast<double>(expected);                       \
        const double check_delta_ = check_actual_ - check_expected_;                        \
        const double check_abs_ = check_delta_ < 0 ? -check_delta_ : check_delta_;          \
        if (!(check_abs_ <= (tolerance))) {                                                 \
            char check_buffer_[256];                                                        \
            std::snprintf(check_buffer_, sizeof(check_buffer_),                              \
                          "CHECK_NEAR(" #actual ", " #expected "): %.17g vs %.17g (delta %.3g)", \
                          check_actual_, check_expected_, check_abs_);                       \
            ::styly::netsync::test::report_failure(__FILE__, __LINE__, check_buffer_);       \
        }                                                                                    \
    } while (false)

#define CHECK_BYTES(actual, expected)                                                       \
    do {                                                                                    \
        ++::styly::netsync::test::check_count();                                             \
        /* Bind first: the arguments may be temporaries, and taking begin()/end() */        \
        /* from two separate evaluations would mix iterators from two objects. */           \
        const auto &check_actual_ref_ = (actual);                                            \
        const auto &check_expected_ref_ = (expected);                                        \
        const std::vector<unsigned char> check_actual_(check_actual_ref_.begin(),            \
                                                       check_actual_ref_.end());             \
        const std::vector<unsigned char> check_expected_(check_expected_ref_.begin(),        \
                                                         check_expected_ref_.end());         \
        if (check_actual_ != check_expected_) {                                              \
            ::styly::netsync::test::report_failure(                                          \
                __FILE__, __LINE__,                                                          \
                std::string("CHECK_BYTES(" #actual ", " #expected ")") +                     \
                    ::styly::netsync::test::describe_diff(check_actual_, check_expected_));  \
        }                                                                                    \
    } while (false)
