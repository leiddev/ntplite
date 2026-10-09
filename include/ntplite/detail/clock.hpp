// ============================================================================
// ntplite - detail/clock.hpp
// ----------------------------------------------------------------------------
// Reading the local clock, and the four-timestamp arithmetic of RFC 5905.
//
// Two separate concerns live here, and keeping them apart is the point:
//
//   * reading the *local* clocks.  The wall clock is what the offset has to be
//     expressed against, and a monotonic clock is what the round trip has to be
//     measured with.  Using the wall clock for the round trip would let a step
//     (which is exactly what happens on a host that something else is
//     synchronising) produce a negative or absurd delay.
//
//   * the estimator.  Given the four timestamps and the local round trip it
//     produces theta (offset) and delta (delay) exactly as the standard
//     defines them.  It is a pure function of its input with no clock access
//     at all, which is what makes it exhaustively testable with a table of
//     hand-computed numbers.
//
// Reference: RFC 5905, section 8 ("On-wire protocol").
// ============================================================================

#ifndef NTP_LITE_DETAIL_CLOCK_HPP
#define NTP_LITE_DETAIL_CLOCK_HPP

#include <chrono>
#include <cstdint>
#include <ntplite/detail/config.hpp>
#include <ntplite/detail/ntp_time.hpp>
#include <ntplite/time.hpp>

namespace ntplite {
namespace detail {

// ---------------------------------------------------------------------------
// Nanosecond arithmetic
// ---------------------------------------------------------------------------

/// The signed difference `lhs - rhs`, in nanoseconds.
///
/// Both instants have already had their era resolved against the local clock,
/// so the difference is bounded by half an era plus local clock error - about
/// 2.2e18 ns - and cannot overflow the intermediate.
inline std::int64_t difference_nanoseconds(const timestamp& lhs, const timestamp& rhs) {
  return (lhs.seconds - rhs.seconds) * static_cast<std::int64_t>(nanoseconds_per_second) +
         (static_cast<std::int64_t>(lhs.nanoseconds) - static_cast<std::int64_t>(rhs.nanoseconds));
}

/// A duration from a signed nanosecond count.
inline duration duration_from_nanoseconds(std::int64_t value) {
  return make_duration(0, value);
}

/// Decodes an on-wire timestamp against the local clock.
///
/// The local clock is the era reference, exactly as RFC 5905 requires: the era
/// of a 32-bit second count is then unambiguous as long as the local clock is
/// within ~68 years of the truth, which is a very safe assumption for a
/// machine that is asking someone else what time it is.
inline timestamp decode_timestamp(const ntp_timestamp& value, std::int64_t reference_unix_seconds) {
  const unix_parts parts = ntp_timestamp_to_unix(value, reference_unix_seconds);
  return make_timestamp(parts.seconds, static_cast<std::int64_t>(parts.nanoseconds));
}

// ---------------------------------------------------------------------------
// Reading the local clocks
// ---------------------------------------------------------------------------

/// Both local clocks, read as close together as the code can manage.
struct clock_reading {
  timestamp wall;                                   ///< Unix time, with a sub-second part
  std::chrono::steady_clock::time_point monotonic;  ///< for elapsed-time arithmetic
};

/// Samples the wall clock and the monotonic clock.
///
/// Call it immediately before sending a request and immediately after the
/// reply comes back; the two readings then bracket the exchange as tightly as
/// the socket layer allows.
///
/// The wall clock is converted through std::chrono::system_clock, whose epoch
/// is the Unix epoch on every platform ntplite supports (see time.hpp).
inline clock_reading read_clocks() {
  clock_reading reading;
  reading.monotonic = std::chrono::steady_clock::now();
  reading.wall = from_chrono(std::chrono::system_clock::now());
  return reading;
}

/// Nanoseconds between two monotonic readings, clamped at zero.
///
/// steady_clock is specified to be monotonic, so a negative result would mean
/// the assumption does not hold on this platform; clamping keeps the delay
/// arithmetic meaningful instead of propagating a bogus negative round trip.
inline std::int64_t elapsed_nanoseconds(const clock_reading& from, const clock_reading& to) {
  const std::int64_t count =
      std::chrono::duration_cast<std::chrono::nanoseconds>(to.monotonic - from.monotonic).count();
  return count < 0 ? 0 : count;
}

// ---------------------------------------------------------------------------
// The estimator
// ---------------------------------------------------------------------------

/// The inputs of one exchange.
struct clock_sample_input {
  timestamp local_send;                 ///< T1: our wall clock when we sent
  timestamp server_receive;             ///< T2: the server's clock when it received
  timestamp server_send;                ///< T3: the server's clock when it answered
  timestamp local_receive;              ///< T4: our wall clock when it arrived
  std::int64_t round_trip_nanoseconds;  ///< T4 - T1, measured on the monotonic clock
};

/// The result of the arithmetic.
struct clock_sample {
  /// theta: the amount to *add* to the local clock to reach the server's, so a
  /// positive offset means the local clock is behind.
  duration offset;

  /// delta: the round trip with the server's own processing time removed, i.e.
  /// the part of the round trip that is network and our own overhead.
  duration round_trip_delay;

  /// T3 - T2, as reported by the server.
  duration server_processing;

  /// T4 - T1, as measured locally.
  duration round_trip_time;

  /// False when the server's timestamps claim the request was processed for
  /// longer than the entire round trip, which no honest server does.  RFC 5905
  /// says to discard such a sample rather than to use it.
  bool delay_is_plausible;
};

/// Computes the offset and the round trip delay of one exchange.
///
///     theta = ((T2 - T1) + (T3 - T4)) / 2
///     delta = (T4 - T1) - (T3 - T2)
///
/// The `T4 - T1` term of delta is taken from the monotonic clock rather than
/// from two wall-clock readings, so a clock step during the exchange cannot
/// corrupt it.
inline clock_sample estimate_clock(const clock_sample_input& input) {
  const std::int64_t t2_minus_t1 = difference_nanoseconds(input.server_receive, input.local_send);
  const std::int64_t t3_minus_t4 = difference_nanoseconds(input.server_send, input.local_receive);
  const std::int64_t t3_minus_t2 = difference_nanoseconds(input.server_send, input.server_receive);
  const std::int64_t round_trip = input.round_trip_nanoseconds;
  const std::int64_t delay = round_trip - t3_minus_t2;

  // Halving a signed value: plain division truncates toward zero, which would
  // bias the offset in a different direction for negative offsets than for
  // positive ones.  Rounding half away from zero costs one comparison and
  // removes the asymmetry - the 0.5 ns it saves is far below any real clock's
  // resolution, but a consistent bias is not something to leave in a formula
  // whose whole job is to report a bias.
  const std::int64_t sum = t2_minus_t1 + t3_minus_t4;
  const std::int64_t offset = (sum >= 0) ? (sum + 1) / 2 : (sum - 1) / 2;

  clock_sample sample;
  sample.offset = duration_from_nanoseconds(offset);
  sample.round_trip_delay = duration_from_nanoseconds(delay);
  sample.server_processing = duration_from_nanoseconds(t3_minus_t2);
  sample.round_trip_time = duration_from_nanoseconds(round_trip);
  sample.delay_is_plausible = delay >= 0;
  return sample;
}

}  // namespace detail
}  // namespace ntplite

#endif  // NTP_LITE_DETAIL_CLOCK_HPP
