// ============================================================================
// ntplite - tests/test_ntp_time.cpp
// ----------------------------------------------------------------------------
// Unit tests for the on-the-wire NTP time formats: the 32.32 fixed point
// timestamps, the 16.16 short format, and NTP era resolution.
//
// Every expected value here was derived by hand from the definitions rather
// than copied out of a run, so the tests genuinely pin the behaviour down.
// ============================================================================

#include <ntplite/detail/ntp_time.hpp>

#include "ntplite_test.hpp"

namespace {

using ntplite::detail::fraction_to_nanoseconds;
using ntplite::detail::nanoseconds_to_fraction;
using ntplite::detail::nanoseconds_to_ntp_short;
using ntplite::detail::ntp_era_seconds;
using ntplite::detail::ntp_seconds_to_unix;
using ntplite::detail::ntp_short;
using ntplite::detail::ntp_short_to_nanoseconds;
using ntplite::detail::ntp_timestamp;
using ntplite::detail::ntp_timestamp_to_unix;
using ntplite::detail::ntp_to_unix_epoch_seconds;
using ntplite::detail::resolve_era;
using ntplite::detail::unix_parts;
using ntplite::detail::unix_seconds_to_ntp;
using ntplite::detail::unix_to_ntp_timestamp;

// --- Reference instants, all UTC -------------------------------------------
const std::int64_t kUnixEpoch = 0;                // 1970-01-01T00:00:00Z
const std::int64_t kYear1950 = -631152000;        // 1950-01-01T00:00:00Z
const std::int64_t kYear2026 = 1767225600;        // 2026-01-01T00:00:00Z
const std::int64_t kRollover = 2085978496;        // 2036-02-07T06:28:16Z, era 1 begins
const std::int64_t kRolloverMinus1 = 2085978495;  // 2036-02-07T06:28:15Z, last era 0 second
const std::int64_t kYear2040 = 2208988800;        // 2040-01-01T00:00:00Z

const std::int64_t kMaxUint32 = 4294967295LL;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
NTP_TEST(ntp_time, epoch_offset_is_correct) {
  // 1900 -> 1970 is 70 years, 17 of them leap days: 25567 days.
  NTP_TEST_CHECK_EQ(25567LL * 86400LL, ntp_to_unix_epoch_seconds);
  NTP_TEST_CHECK_EQ(2208988800LL, ntp_to_unix_epoch_seconds);
}

NTP_TEST(ntp_time, era_is_two_to_the_thirty_two) {
  NTP_TEST_CHECK_EQ(4294967296LL, ntp_era_seconds);
  NTP_TEST_CHECK_EQ(ntp_era_seconds - 1, kMaxUint32);
}

// ---------------------------------------------------------------------------
// fraction <-> nanoseconds
// ---------------------------------------------------------------------------
NTP_TEST(fraction, known_values) {
  NTP_TEST_CHECK_EQ(0U, fraction_to_nanoseconds(0x00000000U));
  NTP_TEST_CHECK_EQ(500000000U, fraction_to_nanoseconds(0x80000000U));  // exactly 1/2 s
  NTP_TEST_CHECK_EQ(250000000U, fraction_to_nanoseconds(0x40000000U));  // exactly 1/4 s
  NTP_TEST_CHECK_EQ(750000000U, fraction_to_nanoseconds(0xC0000000U));  // exactly 3/4 s
}

NTP_TEST(fraction, truncates_and_stays_in_range) {
  // One LSB is 1/2^32 s ~= 0.2328 ns, so it truncates away.
  NTP_TEST_CHECK_EQ(0U, fraction_to_nanoseconds(1U));

  // The largest fraction maps to a value strictly below one second.
  const std::uint32_t largest = fraction_to_nanoseconds(0xFFFFFFFFU);
  NTP_TEST_CHECK_EQ(999999999U, largest);
  NTP_TEST_CHECK(largest < 1000000000U);
}

NTP_TEST(fraction, nanoseconds_known_values) {
  NTP_TEST_CHECK_EQ(0U, nanoseconds_to_fraction(0U));
  NTP_TEST_CHECK_EQ(0x80000000U, nanoseconds_to_fraction(500000000U));
  NTP_TEST_CHECK_EQ(0x40000000U, nanoseconds_to_fraction(250000000U));
  NTP_TEST_CHECK_EQ(0xC0000000U, nanoseconds_to_fraction(750000000U));
}

NTP_TEST(fraction, nanoseconds_never_overflows_the_field) {
  // 999999999 ns is the largest legal input; rounding it must not carry into
  // bit 32.
  const std::uint32_t mapped = nanoseconds_to_fraction(999999999U);
  NTP_TEST_CHECK(mapped < 0xFFFFFFFFU);
  NTP_TEST_CHECK_EQ(4294967292U, mapped);
}

NTP_TEST(fraction, round_trip_within_one_nanosecond) {
  const std::uint32_t samples[] = {0U,         1U,         100U,       1000U,
                                   1000000U,   100000000U, 123456789U, 250000000U,
                                   500000000U, 750000000U, 999999998U, 999999999U};

  const std::size_t count = sizeof(samples) / sizeof(samples[0]);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t original = samples[i];
    const std::uint32_t back = fraction_to_nanoseconds(nanoseconds_to_fraction(original));

    const long difference = static_cast<long>(back) - static_cast<long>(original);
    NTP_TEST_CHECK(difference <= 1 && difference >= -1);
  }
}

// ---------------------------------------------------------------------------
// Era resolution
// ---------------------------------------------------------------------------
NTP_TEST(era, unix_to_ntp_known_values) {
  NTP_TEST_CHECK_EQ(2208988800U, unix_seconds_to_ntp(kUnixEpoch));
  NTP_TEST_CHECK_EQ(3976214400U, unix_seconds_to_ntp(kYear2026));
  NTP_TEST_CHECK_EQ(0U, unix_seconds_to_ntp(kRollover));
  NTP_TEST_CHECK_EQ(4294967295U, unix_seconds_to_ntp(kRolloverMinus1));

  // 2040-01-01 is squarely in era 1: its absolute NTP second count is
  // 2208988800 + 2208988800 = 4417977600, which reduces to 123010304.
  NTP_TEST_CHECK_EQ(123010304U, unix_seconds_to_ntp(kYear2040));
}

NTP_TEST(era, unix_to_ntp_handles_pre_1970_instants) {
  // 1950 is 20 years before the Unix epoch, i.e. 50 years after the NTP epoch.
  NTP_TEST_CHECK_EQ(1577836800U, unix_seconds_to_ntp(kYear1950));
}

NTP_TEST(era, era_zero_for_a_time_near_the_reference) {
  // With a 2026 reference, the raw value 3976214400 unambiguously means 2026.
  NTP_TEST_CHECK_EQ(0, resolve_era(3976214400U, kYear2026));
  NTP_TEST_CHECK_EQ(kYear2026, ntp_seconds_to_unix(3976214400U, kYear2026));
}

NTP_TEST(era, era_is_chosen_by_proximity_not_by_default) {
  // Raw 0 is 1900 in era 0 and 2036 in era 1.  Against a 2026 reference the
  // closer of the two is 2036, so era 1 must win.
  NTP_TEST_CHECK_EQ(1, resolve_era(0U, kYear2026));
  NTP_TEST_CHECK_EQ(kRollover, ntp_seconds_to_unix(0U, kYear2026));

  // Against a 1950 reference the same raw value must resolve to 1900 instead:
  // 1900 is 50 years away, 2036 is 86 years away.
  NTP_TEST_CHECK_EQ(0, resolve_era(0U, kYear1950));
  NTP_TEST_CHECK_EQ(-2208988800LL, ntp_seconds_to_unix(0U, kYear1950));
}

NTP_TEST(era, rollover_boundary_is_continuous) {
  // One second before and at the era 1 boundary, decoded against a reference
  // that sits exactly on the boundary.
  NTP_TEST_CHECK_EQ(kRolloverMinus1, ntp_seconds_to_unix(4294967295U, kRollover));
  NTP_TEST_CHECK_EQ(kRollover, ntp_seconds_to_unix(0U, kRollover));
}

NTP_TEST(era, decodes_a_time_ten_years_past_the_rollover) {
  // 2040-01-01 is 123010304 s into era 1 (2208988800 + 2208988800 - 2^32).
  NTP_TEST_CHECK_EQ(1, resolve_era(123010304U, kYear2040));
  NTP_TEST_CHECK_EQ(kYear2040, ntp_seconds_to_unix(123010304U, kYear2040));
}

// ---------------------------------------------------------------------------
// Whole timestamps
// ---------------------------------------------------------------------------
NTP_TEST(ntp_timestamp, round_trip_at_nanosecond_granularity) {
  const std::int64_t seconds[] = {kYear2026, kYear1950, kRollover, kYear2040};
  const std::uint32_t nanoseconds[] = {0U, 250000000U, 500000000U, 750000000U};

  const std::size_t second_count = sizeof(seconds) / sizeof(seconds[0]);
  const std::size_t nano_count = sizeof(nanoseconds) / sizeof(nanoseconds[0]);

  for (std::size_t s = 0; s < second_count; ++s) {
    for (std::size_t n = 0; n < nano_count; ++n) {
      const ntp_timestamp wire = unix_to_ntp_timestamp(seconds[s], nanoseconds[n]);
      const unix_parts back = ntp_timestamp_to_unix(wire, seconds[s]);

      NTP_TEST_CHECK_EQ(seconds[s], back.seconds);
      NTP_TEST_CHECK_EQ(nanoseconds[n], back.nanoseconds);
    }
  }
}

NTP_TEST(ntp_timestamp, decodes_a_known_packet_value) {
  // 0xED003780 is 2026-01-01T00:00:00Z expressed in NTP seconds, with a
  // fraction of 0x80000000 meaning half a second.
  ntp_timestamp wire;
  wire.seconds = 3976214400U;
  wire.fraction = 0x80000000U;

  const unix_parts decoded = ntp_timestamp_to_unix(wire, kYear2026);
  NTP_TEST_CHECK_EQ(kYear2026, decoded.seconds);
  NTP_TEST_CHECK_EQ(500000000U, decoded.nanoseconds);
}

// ---------------------------------------------------------------------------
// 16.16 short format
// ---------------------------------------------------------------------------
NTP_TEST(ntp_short_format, to_nanoseconds) {
  ntp_short value;

  value.seconds = 0;
  value.fraction = 0;
  NTP_TEST_CHECK_EQ(0ULL, ntp_short_to_nanoseconds(value));

  value.seconds = 0;
  value.fraction = 0x8000;  // 0.5 s
  NTP_TEST_CHECK_EQ(500000000ULL, ntp_short_to_nanoseconds(value));

  value.seconds = 0;
  value.fraction = 0xFFFF;  // 0.99998... s
  NTP_TEST_CHECK_EQ(999984741ULL, ntp_short_to_nanoseconds(value));

  value.seconds = 1;
  value.fraction = 0;
  NTP_TEST_CHECK_EQ(1000000000ULL, ntp_short_to_nanoseconds(value));

  value.seconds = 1;
  value.fraction = 0xFFFF;
  NTP_TEST_CHECK_EQ(1999984741ULL, ntp_short_to_nanoseconds(value));
}

NTP_TEST(ntp_short_format, from_nanoseconds) {
  ntp_short value = nanoseconds_to_ntp_short(0ULL);
  NTP_TEST_CHECK_EQ(0, static_cast<int>(value.seconds));
  NTP_TEST_CHECK_EQ(0, static_cast<int>(value.fraction));

  value = nanoseconds_to_ntp_short(500000000ULL);
  NTP_TEST_CHECK_EQ(0, static_cast<int>(value.seconds));
  NTP_TEST_CHECK_EQ(0x8000, static_cast<int>(value.fraction));

  value = nanoseconds_to_ntp_short(1000000000ULL);
  NTP_TEST_CHECK_EQ(1, static_cast<int>(value.seconds));
  NTP_TEST_CHECK_EQ(0, static_cast<int>(value.fraction));

  // Exactly 65535 s is representable and must not be rounded up to the
  // saturation value.
  value = nanoseconds_to_ntp_short(65535ULL * 1000000000ULL);
  NTP_TEST_CHECK_EQ(65535, static_cast<int>(value.seconds));
  NTP_TEST_CHECK_EQ(0, static_cast<int>(value.fraction));
}

NTP_TEST(ntp_short_format, saturates_instead_of_wrapping) {
  const ntp_short value = nanoseconds_to_ntp_short(70000ULL * 1000000000ULL);
  NTP_TEST_CHECK_EQ(65535, static_cast<int>(value.seconds));
  NTP_TEST_CHECK_EQ(65535, static_cast<int>(value.fraction));

  const std::uint64_t back = ntp_short_to_nanoseconds(value);
  NTP_TEST_CHECK(back > 65535ULL * 1000000000ULL);
  NTP_TEST_CHECK(back < 65536ULL * 1000000000ULL);
}

NTP_TEST(ntp_short_format, round_trip_for_typical_delays) {
  // Root delay and dispersion are normally well under a second.
  const std::uint64_t samples[] = {0ULL,         1000ULL,       500000ULL,
                                   1000000ULL,   100000000ULL,  500000000ULL,
                                   999984741ULL, 4000000000ULL, 15000000000ULL};

  const std::size_t count = sizeof(samples) / sizeof(samples[0]);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint64_t back = ntp_short_to_nanoseconds(nanoseconds_to_ntp_short(samples[i]));
    const std::uint64_t original = samples[i];

    // One 16.16 LSB is 2^-16 s = 15258.789 ns; rounding to the nearest LSB
    // plus the truncation on the way back stays inside a single LSB.
    const std::uint64_t tolerance = 15259ULL;
    const std::uint64_t difference = back > original ? back - original : original - back;
    NTP_TEST_CHECK(difference <= tolerance);
  }
}

}  // namespace
