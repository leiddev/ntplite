// ============================================================================
// ntplite - tests/ntplite_test_support.hpp
// ----------------------------------------------------------------------------
// Helpers shared by the test translation units.
//
// The micro-framework in ntplite_test.hpp deliberately knows nothing about
// ntplite itself, so anything that has to render a library type lives here.
// ============================================================================

#ifndef NTP_LITE_TESTS_NTP_LITE_TEST_SUPPORT_HPP
#define NTP_LITE_TESTS_NTP_LITE_TEST_SUPPORT_HPP

#include <ntplite/detail/error.hpp>
#include <string>

#include "ntplite_test.hpp"

namespace ntplite_test {

/// Compares two error codes, reporting both names.
///
/// ntplite::error_code is a scoped enumeration and has no operator<<, so a raw
/// CHECK_EQ would not compile and a raw CHECK would not say what went wrong.
inline void check_error_code_impl(::ntplite::error_code actual, ::ntplite::error_code expected,
                                  const char* file, int line) {
  ++total_checks();
  if (actual != expected) {
    report_failure(file, line,
                   std::string("expected error '") + ::ntplite::error_code_name(expected) +
                       "', got '" + ::ntplite::error_code_name(actual) + "'");
  }
}

}  // namespace ntplite_test

/// Asserts that `actual` equals `expected`, printing both by name on failure.
#define NTP_TEST_CHECK_ERROR(actual, expected) \
  ::ntplite_test::check_error_code_impl((actual), (expected), __FILE__, __LINE__)

#endif  // NTP_LITE_TESTS_NTP_LITE_TEST_SUPPORT_HPP
