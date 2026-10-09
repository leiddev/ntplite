// ============================================================================
// ntplite - tests/test_error.cpp
// ----------------------------------------------------------------------------
// The error taxonomy, and its numerical contract with the frozen C ABI.
//
// The static assertions in detail/error.hpp already stop the build if the two
// enumerations drift apart, so these tests are about documenting the contract
// and about the behaviour the assertions cannot express: that every code has a
// distinct name, and that a status arriving from foreign code is never
// undefined behaviour.
// ============================================================================

#include <cstddef>
#include <ntplite/detail/error.hpp>
#include <set>
#include <string>

#include "ntplite_test.hpp"
#include "ntplite_test_support.hpp"

using ntplite::error_code;

namespace {

const error_code kAllCodes[] = {error_code::ok,
                                error_code::invalid_argument,
                                error_code::network_error,
                                error_code::resolve_error,
                                error_code::timeout,
                                error_code::protocol_error,
                                error_code::kiss_of_death,
                                error_code::unsupported,
                                error_code::internal_error};

// The same list, spelled the way the C API spells it.
const int kAllStatusValues[] = {NTP_LITE_OK,          NTP_LITE_ERR_INVALID,
                                NTP_LITE_ERR_NETWORK, NTP_LITE_ERR_RESOLVE,
                                NTP_LITE_ERR_TIMEOUT, NTP_LITE_ERR_PROTOCOL,
                                NTP_LITE_ERR_KOD,     NTP_LITE_ERR_UNSUPPORTED,
                                NTP_LITE_ERR_INTERNAL};

const std::size_t kCodeCount = sizeof(kAllCodes) / sizeof(kAllCodes[0]);

}  // namespace

NTP_TEST(error, every_code_has_a_distinct_name) {
  std::set<std::string> names;
  for (std::size_t i = 0; i < kCodeCount; ++i) {
    const std::string name = ntplite::error_code_name(kAllCodes[i]);
    NTP_TEST_CHECK(!name.empty());
    names.insert(name);
  }
  NTP_TEST_CHECK_EQ(names.size(), kCodeCount);
}

NTP_TEST(error, unknown_values_still_have_a_name) {
  // The switch in error_code_name() covers every enumerator, so this exercises
  // the fall-through that a caller with a corrupted value would hit.
  const std::string name = ntplite::error_code_name(static_cast<error_code>(-12345));
  NTP_TEST_CHECK(!name.empty());
}

NTP_TEST(error, values_match_the_frozen_c_enumeration) {
  for (std::size_t i = 0; i < kCodeCount; ++i) {
    NTP_TEST_CHECK_EQ(static_cast<int>(kAllCodes[i]), kAllStatusValues[i]);
  }
}

NTP_TEST(error, status_mapping_round_trips) {
  for (std::size_t i = 0; i < kCodeCount; ++i) {
    const ntplite_status_t status = ntplite::to_status(kAllCodes[i]);
    NTP_TEST_CHECK_ERROR(ntplite::from_status(status), kAllCodes[i]);
  }
}

NTP_TEST(error, out_of_range_statuses_survive_the_round_trip) {
  // ntplite::error_code is a *scoped* enumeration, so its underlying type is
  // fixed (int) and converting any int to it is well defined.  That is the
  // whole reason the C enumeration from 0.1.0 had to pin its own underlying
  // type as well: a status that arrives from a C or FFI caller is just a
  // number, and reading it must not be undefined behaviour.
  const int kOutOfRange = 9999;
  const ntplite_status_t status = static_cast<ntplite_status_t>(kOutOfRange);

  const error_code converted = ntplite::from_status(status);
  NTP_TEST_CHECK_EQ(static_cast<int>(converted), kOutOfRange);
  NTP_TEST_CHECK_EQ(static_cast<int>(ntplite::to_status(converted)), kOutOfRange);
}

NTP_TEST(error, negative_statuses_survive_the_round_trip) {
  const int kNegative = -1;
  const error_code converted = ntplite::from_status(static_cast<ntplite_status_t>(kNegative));
  NTP_TEST_CHECK_EQ(static_cast<int>(converted), kNegative);
}
