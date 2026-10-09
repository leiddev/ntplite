// ============================================================================
// ntplite - tests/test_clock.cpp
// ----------------------------------------------------------------------------
// The estimator and the local clock readers.
//
// RFC 5905 defines the offset and the delay as
//
//     theta = ((T2 - T1) + (T3 - T4)) / 2      delta = (T4 - T1) - (T3 - T2)
//
// so the arithmetic can be checked exactly, with hand-computed numbers, without
// a network or a server anywhere in sight.  That is the whole reason the
// estimator is a pure function of four timestamps.
//
// The one thing these tests care about that a naive implementation gets wrong
// is where `T4 - T1` comes from.  ntplite measures it on the monotonic clock, so
// a wall clock that steps mid-exchange cannot turn into a huge or negative
// round trip.  The test that covers it deliberately makes the two disagree.
// ============================================================================

#include <cstdint>
#include <ntplite/detail/clock.hpp>
#include <ntplite/detail/ntp_time.hpp>
#include <ntplite/detail/socket.hpp>
#include <ntplite/time.hpp>

#include "ntplite_test.hpp"
#include "ntplite_test_support.hpp"

namespace {

using ::ntplite::duration;
using ::ntplite::timestamp;

/// A signed nanosecond count, which is the unit the estimator works in and the
/// only unit in which its rounding is visible.
inline std::int64_t nanos(const duration& value) {
  return value.seconds * 1000000000LL + static_cast<std::int64_t>(value.nanoseconds);
}

inline timestamp instant(std::int64_t seconds, std::int64_t nanoseconds) {
  return ::ntplite::make_timestamp(seconds, nanoseconds);
}

inline ::ntplite::detail::ntp_timestamp wire_timestamp(std::uint32_t seconds,
                                                       std::uint32_t fraction) {
  ::ntplite::detail::ntp_timestamp value;
  value.seconds = seconds;
  value.fraction = fraction;
  return value;
}

/// The inputs of an exchange, spelled out so each test reads as a scenario.
inline ::ntplite::detail::clock_sample_input make_input(const timestamp& t1, const timestamp& t2,
                                                        const timestamp& t3, const timestamp& t4,
                                                        std::int64_t round_trip_nanoseconds) {
  ::ntplite::detail::clock_sample_input input;
  input.local_send = t1;
  input.server_receive = t2;
  input.server_send = t3;
  input.local_receive = t4;
  input.round_trip_nanoseconds = round_trip_nanoseconds;
  return input;
}

}  // namespace

// ---------------------------------------------------------------------------
// The estimator
// ---------------------------------------------------------------------------

NTP_TEST(clock, a_symmetric_exchange_recovers_the_offset_exactly) {
  // The server's clock is exactly 1000 s ahead.  The path costs 10 ms each way
  // and the server spends 5 ms on the request:
  //
  //   T1 = 0.000            (local)
  //   T2 = 1000.010         (server)   = T1 + 1000 s + 10 ms
  //   T3 = 1000.015         (server)   = T2 + 5 ms
  //   T4 = 0.025            (local)    = T3 - 1000 s + 10 ms
  //
  //   theta = ((1000.010 - 0) + (1000.015 - 0.025)) / 2 = 1000 s
  //   delta = (0.025 - 0) - (1000.015 - 1000.010)       = 0.020 s
  const ::ntplite::detail::clock_sample sample = ::ntplite::detail::estimate_clock(
      make_input(instant(0, 0), instant(1000, 10000000), instant(1000, 15000000),
                 instant(0, 25000000), 25000000));

  NTP_TEST_CHECK_EQ(nanos(sample.offset), 1000000000000LL);
  NTP_TEST_CHECK_EQ(nanos(sample.round_trip_delay), 20000000LL);
  NTP_TEST_CHECK_EQ(nanos(sample.server_processing), 5000000LL);
  NTP_TEST_CHECK_EQ(nanos(sample.round_trip_time), 25000000LL);
  NTP_TEST_CHECK(sample.delay_is_plausible);
}

NTP_TEST(clock, a_fast_local_clock_gives_a_negative_offset) {
  // The mirror image: the local clock is 1000 s ahead of the server, so the
  // offset must come out negative.  Nothing in the formula may assume a sign.
  const ::ntplite::detail::clock_sample sample = ::ntplite::detail::estimate_clock(
      make_input(instant(1000, 0), instant(0, 10000000), instant(0, 15000000),
                 instant(1000, 25000000), 25000000));

  NTP_TEST_CHECK_EQ(nanos(sample.offset), -1000000000000LL);
  NTP_TEST_CHECK_EQ(nanos(sample.round_trip_delay), 20000000LL);
  NTP_TEST_CHECK(sample.delay_is_plausible);
}

NTP_TEST(clock, a_perfect_exchange_has_no_offset_and_no_delay) {
  const ::ntplite::detail::clock_sample sample = ::ntplite::detail::estimate_clock(
      make_input(instant(0, 0), instant(0, 0), instant(0, 0), instant(0, 0), 0));

  NTP_TEST_CHECK_EQ(nanos(sample.offset), 0);
  NTP_TEST_CHECK_EQ(nanos(sample.round_trip_delay), 0);
  NTP_TEST_CHECK(sample.delay_is_plausible);
}

NTP_TEST(clock, the_offset_rounds_half_away_from_zero) {
  // Sum = (T2 - T1) + (T3 - T4) = -1 ns.  Truncating division would report 0;
  // rounding half away from zero reports -1.  The difference is a single
  // nanosecond, far below any real clock's resolution, but a formula whose job
  // is to report a bias must not introduce one of its own.
  const ::ntplite::detail::clock_sample negative = ::ntplite::detail::estimate_clock(
      make_input(instant(0, 0), instant(0, 0), instant(0, 0), instant(0, 1), 1));
  NTP_TEST_CHECK_EQ(nanos(negative.offset), -1);

  // Sum = +1 ns, the other half of the symmetry.
  const ::ntplite::detail::clock_sample positive = ::ntplite::detail::estimate_clock(
      make_input(instant(0, 0), instant(0, 1), instant(0, 0), instant(0, 0), 0));
  NTP_TEST_CHECK_EQ(nanos(positive.offset), 1);
}

NTP_TEST(clock, an_implausible_delay_is_flagged_rather_than_used) {
  // The server claims to have spent 100 ms on a request that was in flight for
  // 10 ms in total.  delta = (T4 - T1) - (T3 - T2) = 10 ms - 100 ms = -90 ms.
  // RFC 5905 says to discard such a sample, so the flag has to be set.
  const ::ntplite::detail::clock_sample sample = ::ntplite::detail::estimate_clock(make_input(
      instant(0, 0), instant(0, 0), instant(0, 100000000), instant(0, 10000000), 10000000));

  NTP_TEST_CHECK(!sample.delay_is_plausible);
  NTP_TEST_CHECK_EQ(nanos(sample.round_trip_delay), -90000000LL);
  NTP_TEST_CHECK_EQ(nanos(sample.server_processing), 100000000LL);
}

NTP_TEST(clock, the_round_trip_delay_ignores_the_wall_clock) {
  // T1 and T4 are 100 s apart in wall clock terms - something stepped the clock
  // while the request was in flight - but the monotonic round trip says 10 ms.
  // delta must follow the monotonic measurement, because otherwise a clock step
  // would be reported as network delay.
  const ::ntplite::detail::clock_sample sample = ::ntplite::detail::estimate_clock(
      make_input(instant(0, 0), instant(0, 0), instant(0, 0), instant(100, 0), 10000000));

  NTP_TEST_CHECK_EQ(nanos(sample.round_trip_delay), 10000000LL);
  NTP_TEST_CHECK_EQ(nanos(sample.round_trip_time), 10000000LL);
  // The offset, on the other hand, *is* expressed against the wall clock and
  // must show the step.
  NTP_TEST_CHECK_EQ(nanos(sample.offset), -50000000000LL);
}

NTP_TEST(clock, the_server_processing_time_is_reported_separately) {
  // The delay is the round trip minus the server's share, so the two must not
  // be confused: a slow server looks like a long path unless they are split.
  const ::ntplite::detail::clock_sample sample = ::ntplite::detail::estimate_clock(make_input(
      instant(0, 0), instant(0, 1000000), instant(0, 40000000), instant(0, 50000000), 50000000));

  NTP_TEST_CHECK_EQ(nanos(sample.server_processing), 39000000LL);
  NTP_TEST_CHECK_EQ(nanos(sample.round_trip_time), 50000000LL);
  NTP_TEST_CHECK_EQ(nanos(sample.round_trip_delay), 11000000LL);
  NTP_TEST_CHECK(sample.delay_is_plausible);
}

// ---------------------------------------------------------------------------
// Nanosecond arithmetic
// ---------------------------------------------------------------------------

NTP_TEST(clock, nanosecond_differences_borrow_across_seconds) {
  const timestamp a = instant(5, 250000000);
  const timestamp b = instant(2, 750000000);

  NTP_TEST_CHECK_EQ(::ntplite::detail::difference_nanoseconds(a, b), 2500000000LL);
  NTP_TEST_CHECK_EQ(::ntplite::detail::difference_nanoseconds(b, a), -2500000000LL);
  NTP_TEST_CHECK_EQ(::ntplite::detail::difference_nanoseconds(a, a), 0);
}

NTP_TEST(clock, a_duration_built_from_negative_nanoseconds_stays_normalised) {
  const duration minus_one = ::ntplite::detail::duration_from_nanoseconds(-1);
  NTP_TEST_CHECK_EQ(minus_one.seconds, -1);
  NTP_TEST_CHECK_EQ(minus_one.nanoseconds, 999999999U);
  NTP_TEST_CHECK_EQ(nanos(minus_one), -1);

  const duration one_and_a_half = ::ntplite::detail::duration_from_nanoseconds(1500000000LL);
  NTP_TEST_CHECK_EQ(one_and_a_half.seconds, 1);
  NTP_TEST_CHECK_EQ(one_and_a_half.nanoseconds, 500000000U);
  NTP_TEST_CHECK_EQ(nanos(one_and_a_half), 1500000000LL);
}

// ---------------------------------------------------------------------------
// Decoding an on-wire timestamp
// ---------------------------------------------------------------------------

NTP_TEST(clock, a_timestamp_survives_a_round_trip_through_the_wire_format) {
  const std::int64_t unix_seconds = 1767225600;  // 2026-01-01T00:00:00Z
  const std::uint32_t nanoseconds = 250000000;

  const ::ntplite::detail::ntp_timestamp wire =
      ::ntplite::detail::unix_to_ntp_timestamp(unix_seconds, nanoseconds);
  const timestamp decoded = ::ntplite::detail::decode_timestamp(wire, unix_seconds);

  NTP_TEST_CHECK_EQ(decoded.seconds, unix_seconds);
  NTP_TEST_CHECK_EQ(decoded.nanoseconds, nanoseconds);
}

NTP_TEST(clock, the_era_of_a_decoded_timestamp_is_chosen_from_the_local_clock) {
  const std::int64_t reference = 1767225600;  // 2026-01-01T00:00:00Z

  // 2026 is era 0: 3976214400 s after 1900 is inside the first 2^32 seconds.
  const timestamp in_era_zero =
      ::ntplite::detail::decode_timestamp(wire_timestamp(3976214400U, 0U), reference);
  NTP_TEST_CHECK_EQ(in_era_zero.seconds, reference);

  // A raw seconds field of zero on its own is ambiguous - 1900 or 2036.  With a
  // local clock in 2026 the answer is 2036, because that is the closer era.
  // Getting this wrong would be an error of 136 years, so it is worth a test.
  const timestamp wrap = ::ntplite::detail::decode_timestamp(wire_timestamp(0U, 0U), reference);
  NTP_TEST_CHECK_EQ(wrap.seconds, 2085978496);  // 2036-02-07T06:28:16Z
}

NTP_TEST(clock, the_era_boundary_resolves_to_the_instant_next_to_the_reference) {
  // The instant one second before the 2036 rollover still has an all-ones raw
  // seconds field; the instant at the rollover has a zero one.  Both must
  // decode back to themselves rather than to their 1900 counterparts.
  const std::int64_t before = 2085978495;
  const std::int64_t at = 2085978496;

  NTP_TEST_CHECK_EQ(::ntplite::detail::decode_timestamp(
                        ::ntplite::detail::unix_to_ntp_timestamp(before, 0U), before)
                        .seconds,
                    before);
  NTP_TEST_CHECK_EQ(
      ::ntplite::detail::decode_timestamp(::ntplite::detail::unix_to_ntp_timestamp(at, 0U), at)
          .seconds,
      at);
}

// ---------------------------------------------------------------------------
// Reading the local clocks
// ---------------------------------------------------------------------------

NTP_TEST(clock, the_monotonic_clock_never_runs_backwards) {
  const ::ntplite::detail::clock_reading first = ::ntplite::detail::read_clocks();
  const ::ntplite::detail::clock_reading second = ::ntplite::detail::read_clocks();

  NTP_TEST_CHECK(first.monotonic <= second.monotonic);
  NTP_TEST_CHECK(::ntplite::detail::elapsed_nanoseconds(first, second) >= 0);
  // Reversed arguments are clamped rather than going negative.
  NTP_TEST_CHECK_EQ(::ntplite::detail::elapsed_nanoseconds(second, first), 0);
}

NTP_TEST(clock, the_wall_clock_is_a_plausible_unix_time) {
  const ::ntplite::detail::clock_reading reading = ::ntplite::detail::read_clocks();

  // Some time after 2020-01-01 and before 2100: a sanity check that the era and
  // the epoch of std::chrono::system_clock are what the library assumes.
  NTP_TEST_CHECK(reading.wall.seconds > 1577836800LL);
  NTP_TEST_CHECK(reading.wall.seconds < 4102444800LL);
  NTP_TEST_CHECK(reading.wall.nanoseconds < 1000000000U);
}

NTP_TEST(clock, sleeping_advances_the_monotonic_clock_by_about_the_requested_time) {
  const ::ntplite::detail::clock_reading before = ::ntplite::detail::read_clocks();
  ::ntplite::detail::sleep_ms(20);
  const ::ntplite::detail::clock_reading after = ::ntplite::detail::read_clocks();

  const std::int64_t elapsed = ::ntplite::detail::elapsed_nanoseconds(before, after);
  NTP_TEST_CHECK(elapsed >= 10000000LL);   // at least 10 ms of the 20 asked for
  NTP_TEST_CHECK(elapsed < 5000000000LL);  // and certainly not five seconds
}
