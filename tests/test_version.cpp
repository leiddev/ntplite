// ============================================================================
// ntplite - tests/test_version.cpp
// ----------------------------------------------------------------------------
// Sanity checks for the version surface and the C API.  These are the first
// tests to go red if the header layout or the ABI wiring breaks.
// ============================================================================

#include <ntplite/ntplite.h>

#include <ntplite/ntplite.hpp>

#include "ntplite_test.hpp"

namespace {

// ---------------------------------------------------------------------------
// C++ API
// ---------------------------------------------------------------------------
NTP_TEST(version, cpp_version_string_is_not_empty) {
  NTP_TEST_REQUIRE(ntplite::version_string() != NULL);
  NTP_TEST_CHECK(ntplite::version_string()[0] != '\0');
}

NTP_TEST(version, cpp_version_string_matches_the_macro) {
  NTP_TEST_CHECK_STREQ(ntplite::version_string(), NTP_LITE_VERSION_STRING);
}

NTP_TEST(version, cpp_components_are_non_negative) {
  NTP_TEST_CHECK(ntplite::version_major() >= 0);
  NTP_TEST_CHECK(ntplite::version_minor() >= 0);
  NTP_TEST_CHECK(ntplite::version_patch() >= 0);
}

NTP_TEST(version, cpp_version_number_is_consistent) {
  const int expected =
      ntplite::version_major() * 10000 + ntplite::version_minor() * 100 + ntplite::version_patch();
  NTP_TEST_CHECK_EQ(expected, NTP_LITE_VERSION_NUMBER);
}

// ---------------------------------------------------------------------------
// C API parity - the two surfaces must never drift apart.
// ---------------------------------------------------------------------------
NTP_TEST(version, c_api_version_matches_the_macro) {
  NTP_TEST_CHECK_EQ(NTP_LITE_C_API_VERSION, ntplite_c_api_version());
}

NTP_TEST(version, c_and_cpp_surfaces_agree) {
  NTP_TEST_CHECK_STREQ(ntplite_version_string(), ntplite::version_string());
  NTP_TEST_CHECK_EQ(ntplite_version_major(), ntplite::version_major());
  NTP_TEST_CHECK_EQ(ntplite_version_minor(), ntplite::version_minor());
  NTP_TEST_CHECK_EQ(ntplite_version_patch(), ntplite::version_patch());
}

// ---------------------------------------------------------------------------
// Status codes -> text
// ---------------------------------------------------------------------------
NTP_TEST(status, every_code_has_a_description) {
  const ntplite_status_t codes[] = {NTP_LITE_OK,          NTP_LITE_ERR_INVALID,
                                    NTP_LITE_ERR_NETWORK, NTP_LITE_ERR_RESOLVE,
                                    NTP_LITE_ERR_TIMEOUT, NTP_LITE_ERR_PROTOCOL,
                                    NTP_LITE_ERR_KOD,     NTP_LITE_ERR_UNSUPPORTED,
                                    NTP_LITE_ERR_INTERNAL};

  const std::size_t count = sizeof(codes) / sizeof(codes[0]);
  for (std::size_t i = 0; i < count; ++i) {
    const char* text = ntplite_status_string(codes[i]);
    NTP_TEST_REQUIRE(text != NULL);
    NTP_TEST_CHECK(text[0] != '\0');
    // The generic fallback must not be reachable for a known code.
    NTP_TEST_CHECK(!ntplite_test::string_equal(text, "unknown status"));
  }
}

NTP_TEST(status, ok_is_zero) {
  NTP_TEST_CHECK_EQ(0, static_cast<int>(NTP_LITE_OK));
}

// Regression test for an UndefinedBehaviorSanitizer finding: with a plain
// (unfixed) enumeration type, C++ narrows the value range to the smallest
// bit-field that fits the enumerators, and storing 9999 into it is undefined
// behaviour.  ntplite_status_t therefore pins its underlying type to `int`,
// because C callers - and FFI bindings - legitimately hand over arbitrary
// integers.
NTP_TEST(status, out_of_range_value_is_well_defined) {
  const ntplite_status_t bogus = static_cast<ntplite_status_t>(9999);
  NTP_TEST_CHECK_EQ(9999, static_cast<int>(bogus));

  const char* text = ntplite_status_string(bogus);
  NTP_TEST_REQUIRE(text != NULL);
  NTP_TEST_CHECK_STREQ(text, "unknown status");
}

NTP_TEST(status, negative_value_is_well_defined) {
  const ntplite_status_t bogus = static_cast<ntplite_status_t>(-1);
  NTP_TEST_CHECK_EQ(-1, static_cast<int>(bogus));
  NTP_TEST_CHECK_STREQ(ntplite_status_string(bogus), "unknown status");
}

// ---------------------------------------------------------------------------
// ABI locking.  These assertions exist so that the layout of the C ABI can
// only change on purpose.
// ---------------------------------------------------------------------------
NTP_TEST(abi, status_type_matches_int) {
  NTP_TEST_CHECK_EQ(sizeof(int), sizeof(ntplite_status_t));
}

NTP_TEST(abi, status_values_are_frozen) {
  NTP_TEST_CHECK_EQ(0, static_cast<int>(NTP_LITE_OK));
  NTP_TEST_CHECK_EQ(1, static_cast<int>(NTP_LITE_ERR_INVALID));
  NTP_TEST_CHECK_EQ(2, static_cast<int>(NTP_LITE_ERR_NETWORK));
  NTP_TEST_CHECK_EQ(3, static_cast<int>(NTP_LITE_ERR_RESOLVE));
  NTP_TEST_CHECK_EQ(4, static_cast<int>(NTP_LITE_ERR_TIMEOUT));
  NTP_TEST_CHECK_EQ(5, static_cast<int>(NTP_LITE_ERR_PROTOCOL));
  NTP_TEST_CHECK_EQ(6, static_cast<int>(NTP_LITE_ERR_KOD));
  NTP_TEST_CHECK_EQ(7, static_cast<int>(NTP_LITE_ERR_UNSUPPORTED));
  NTP_TEST_CHECK_EQ(8, static_cast<int>(NTP_LITE_ERR_INTERNAL));
}

}  // namespace
