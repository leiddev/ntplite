// ============================================================================
// ntplite - tests/test_time.cpp
// ----------------------------------------------------------------------------
// Unit tests for the public time types: ntplite::timestamp (a point in time)
// and ntplite::duration (a signed difference).
//
// The emphasis is on the normalisation invariants, because everything
// downstream - offset estimation, the C ABI, the command line tool - depends
// on `nanoseconds` being in [0, 1e9) for both types.
// ============================================================================

#include <ntplite/time.hpp>

#include "ntplite_test.hpp"

namespace {

using ntplite::add_duration;
using ntplite::compare;
using ntplite::difference;
using ntplite::duration;
using ntplite::duration_from_seconds;
using ntplite::duration_seconds;
using ntplite::from_chrono;
using ntplite::make_duration;
using ntplite::make_timestamp;
using ntplite::timestamp;
using ntplite::timestamp_from_seconds;
using ntplite::timestamp_seconds;
using ntplite::to_chrono;

const std::int64_t kYear2026 = 1767225600;  // 2026-01-01T00:00:00Z

// ---------------------------------------------------------------------------
// make_timestamp / normalisation
// ---------------------------------------------------------------------------
NTP_TEST(timestamp, make_keeps_canonical_values_untouched) {
  const timestamp value = make_timestamp(5, 500000000);
  NTP_TEST_CHECK_EQ(5LL, value.seconds);
  NTP_TEST_CHECK_EQ(500000000U, value.nanoseconds);
}

NTP_TEST(timestamp, make_carries_overflowing_nanoseconds) {
  const timestamp exact = make_timestamp(5, 1000000000);
  NTP_TEST_CHECK_EQ(6LL, exact.seconds);
  NTP_TEST_CHECK_EQ(0U, exact.nanoseconds);

  const timestamp carried = make_timestamp(5, 1500000000);
  NTP_TEST_CHECK_EQ(6LL, carried.seconds);
  NTP_TEST_CHECK_EQ(500000000U, carried.nanoseconds);
}

NTP_TEST(timestamp, make_normalises_negative_nanoseconds) {
  // One nanosecond before the second boundary.
  const timestamp before = make_timestamp(5, -1);
  NTP_TEST_CHECK_EQ(4LL, before.seconds);
  NTP_TEST_CHECK_EQ(999999999U, before.nanoseconds);

  const timestamp earlier = make_timestamp(-3, -500000000);
  NTP_TEST_CHECK_EQ(-4LL, earlier.seconds);
  NTP_TEST_CHECK_EQ(500000000U, earlier.nanoseconds);
}

NTP_TEST(timestamp, make_handles_large_carries) {
  const timestamp value = make_timestamp(0, -2500000000LL);
  NTP_TEST_CHECK_EQ(-3LL, value.seconds);
  NTP_TEST_CHECK_EQ(500000000U, value.nanoseconds);
}

// ---------------------------------------------------------------------------
// Scalar views
// ---------------------------------------------------------------------------
NTP_TEST(timestamp, seconds_view) {
  const timestamp value = make_timestamp(kYear2026, 500000000U);
  NTP_TEST_CHECK_NEAR(1767225600.5, timestamp_seconds(value), 1e-9);
}

NTP_TEST(timestamp, from_seconds_round_trip) {
  const timestamp value = timestamp_from_seconds(1767225600.5);
  NTP_TEST_CHECK_EQ(kYear2026, value.seconds);
  NTP_TEST_CHECK_EQ(500000000U, value.nanoseconds);
}

NTP_TEST(timestamp, from_negative_fractional_seconds) {
  const timestamp value = timestamp_from_seconds(-0.25);
  NTP_TEST_CHECK_EQ(-1LL, value.seconds);
  NTP_TEST_CHECK_EQ(750000000U, value.nanoseconds);
  NTP_TEST_CHECK_NEAR(-0.25, timestamp_seconds(value), 1e-9);
}

// ---------------------------------------------------------------------------
// make_duration / normalisation
//
// The sub-second field is non-negative for durations too, so -1.5 s is
// {-2, 500000000} rather than {-1, -500000000}.
// ---------------------------------------------------------------------------
NTP_TEST(duration, make_keeps_small_values) {
  const duration value = make_duration(3, 250000000);
  NTP_TEST_CHECK_EQ(3LL, value.seconds);
  NTP_TEST_CHECK_EQ(250000000U, value.nanoseconds);
}

NTP_TEST(duration, make_carries_positive_overflow) {
  const duration value = make_duration(0, 1500000000);
  NTP_TEST_CHECK_EQ(1LL, value.seconds);
  NTP_TEST_CHECK_EQ(500000000U, value.nanoseconds);
}

NTP_TEST(duration, make_carries_negative_overflow) {
  const duration value = make_duration(0, -1500000000);
  NTP_TEST_CHECK_EQ(-2LL, value.seconds);
  NTP_TEST_CHECK_EQ(500000000U, value.nanoseconds);
  NTP_TEST_CHECK_NEAR(-1.5, duration_seconds(value), 1e-9);
}

NTP_TEST(duration, nanosecond_field_is_never_negative) {
  const duration value = make_duration(0, -1);
  NTP_TEST_CHECK_EQ(-1LL, value.seconds);
  NTP_TEST_CHECK_EQ(999999999U, value.nanoseconds);
  NTP_TEST_CHECK_NEAR(-0.000000001, duration_seconds(value), 1e-12);
}

NTP_TEST(duration, negative_seconds_and_positive_remainder) {
  const duration value = make_duration(-2, 500000000);
  NTP_TEST_CHECK_EQ(-2LL, value.seconds);
  NTP_TEST_CHECK_EQ(500000000U, value.nanoseconds);
  NTP_TEST_CHECK_NEAR(-1.5, duration_seconds(value), 1e-9);
}

NTP_TEST(duration, seconds_view_and_construction) {
  NTP_TEST_CHECK_NEAR(2.5, duration_seconds(duration_from_seconds(2.5)), 1e-9);

  const duration negative = duration_from_seconds(-1.25);
  NTP_TEST_CHECK_EQ(-2LL, negative.seconds);
  NTP_TEST_CHECK_EQ(750000000U, negative.nanoseconds);
  NTP_TEST_CHECK_NEAR(-1.25, duration_seconds(negative), 1e-9);
}

// ---------------------------------------------------------------------------
// std::chrono interoperability
// ---------------------------------------------------------------------------
NTP_TEST(chrono, unix_epoch_maps_to_zero) {
  const std::chrono::system_clock::time_point epoch;
  NTP_TEST_CHECK_EQ(0LL, from_chrono(epoch).seconds);
  NTP_TEST_CHECK_EQ(0U, from_chrono(epoch).nanoseconds);
}

NTP_TEST(chrono, round_trip_is_exact_at_100ns_granularity) {
  // 123456000 ns is a whole number of 100 ns ticks, so it survives a trip
  // through system_clock on both Windows (100 ns ticks) and Linux (1 ns).
  const timestamp original = make_timestamp(kYear2026, 123456000U);
  const timestamp back = from_chrono(to_chrono(original));

  NTP_TEST_CHECK_EQ(original.seconds, back.seconds);
  NTP_TEST_CHECK_EQ(original.nanoseconds, back.nanoseconds);
}

NTP_TEST(chrono, pre_epoch_instants_round_trip) {
  const timestamp original = make_timestamp(-631152000, 250000000U);
  const timestamp back = from_chrono(to_chrono(original));

  NTP_TEST_CHECK_EQ(original.seconds, back.seconds);
  NTP_TEST_CHECK_EQ(original.nanoseconds, back.nanoseconds);
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------
NTP_TEST(arithmetic, difference_within_a_second) {
  const duration delta =
      difference(make_timestamp(kYear2026, 500000000U), make_timestamp(kYear2026, 0U));
  NTP_TEST_CHECK_EQ(0LL, delta.seconds);
  NTP_TEST_CHECK_EQ(500000000U, delta.nanoseconds);
  NTP_TEST_CHECK_NEAR(0.5, duration_seconds(delta), 1e-9);
}

NTP_TEST(arithmetic, difference_across_a_second_boundary) {
  const duration delta = difference(make_timestamp(100, 0U), make_timestamp(0, 500000000U));
  NTP_TEST_CHECK_EQ(99LL, delta.seconds);
  NTP_TEST_CHECK_EQ(500000000U, delta.nanoseconds);
  NTP_TEST_CHECK_NEAR(99.5, duration_seconds(delta), 1e-9);
}

NTP_TEST(arithmetic, negative_difference) {
  const duration delta = difference(make_timestamp(0, 0U), make_timestamp(100, 500000000U));
  NTP_TEST_CHECK_EQ(-101LL, delta.seconds);
  NTP_TEST_CHECK_EQ(500000000U, delta.nanoseconds);
  NTP_TEST_CHECK_NEAR(-100.5, duration_seconds(delta), 1e-9);
}

NTP_TEST(arithmetic, add_duration_carries) {
  const timestamp sum = add_duration(make_timestamp(100, 750000000U), make_duration(1, 500000000));
  NTP_TEST_CHECK_EQ(102LL, sum.seconds);
  NTP_TEST_CHECK_EQ(250000000U, sum.nanoseconds);
}

NTP_TEST(arithmetic, add_negative_duration_borrows) {
  const timestamp sum = add_duration(make_timestamp(100, 0U), make_duration(0, -250000000));
  NTP_TEST_CHECK_EQ(99LL, sum.seconds);
  NTP_TEST_CHECK_EQ(750000000U, sum.nanoseconds);
}

NTP_TEST(arithmetic, add_then_difference_is_identity) {
  const timestamp start = make_timestamp(kYear2026, 123456789U);
  const duration delta = make_duration(-2, -250000000);
  const timestamp moved = add_duration(start, delta);

  const duration back = difference(moved, start);
  NTP_TEST_CHECK_EQ(delta.seconds, back.seconds);
  NTP_TEST_CHECK_EQ(delta.nanoseconds, back.nanoseconds);
}

NTP_TEST(arithmetic, difference_is_antisymmetric) {
  const timestamp a = make_timestamp(kYear2026, 123456789U);
  const timestamp b = make_timestamp(kYear2026 - 7, 900000000U);

  const duration forward = difference(a, b);
  const duration backward = difference(b, a);

  NTP_TEST_CHECK_NEAR(duration_seconds(forward), -duration_seconds(backward), 1e-9);
}

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------
NTP_TEST(ordering, compare) {
  NTP_TEST_CHECK_EQ(0, compare(make_timestamp(1, 0U), make_timestamp(1, 0U)));
  NTP_TEST_CHECK(compare(make_timestamp(1, 1U), make_timestamp(1, 0U)) > 0);
  NTP_TEST_CHECK(compare(make_timestamp(0, 999999999U), make_timestamp(1, 0U)) < 0);
  NTP_TEST_CHECK(compare(make_timestamp(-1, 0U), make_timestamp(0, 0U)) < 0);
}

}  // namespace
