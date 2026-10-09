// ============================================================================
// ntplite - time.hpp
// ----------------------------------------------------------------------------
// The public time types.
//
// ntplite never touches the system clock; it only *reports* instants and
// differences.  These two small PODs are what the rest of the library, the C
// ABI and the command line tool all speak.
//
// Both are aggregates with no user declared constructors, so they stay
// trivially copyable and map one-to-one onto the C structures.
// ============================================================================

#ifndef NTP_LITE_TIME_HPP
#define NTP_LITE_TIME_HPP

#include <chrono>
#include <cmath>
#include <cstdint>
#include <ntplite/detail/ntp_time.hpp>

namespace ntplite {

/// A point in time, as whole seconds plus a nanosecond remainder since the
/// Unix epoch (1970-01-01T00:00:00Z).
///
/// The representation is canonical: `nanoseconds` is always in
/// [0, 999999999].  Use make_timestamp() to build one from a value that may
/// not satisfy that invariant.
///
/// This is UTC as kept by Unix time, so it deliberately ignores leap seconds:
/// a UTC day containing a leap second is 86401 s long in reality but 86400 s
/// here.  For clock offset measurement that difference is irrelevant.
struct timestamp {
  std::int64_t seconds;       ///< seconds since 1970-01-01T00:00:00Z
  std::uint32_t nanoseconds;  ///< [0, 999999999]
};

/// A signed time difference with nanosecond resolution.
///
/// Normalised exactly like ntplite::timestamp, only the seconds field is
/// signed: `seconds` is the floor of the value and `nanoseconds` is always in
/// [0, 999999999].  So -1.5 s is `{-2, 500000000}`, never `{-1, -500000000}`.
///
/// Keeping the sub-second field non-negative in both types removes a whole
/// class of sign bugs from the offset arithmetic downstream, and makes the C
/// ABI mapping trivial.
struct duration {
  std::int64_t seconds;
  std::uint32_t nanoseconds;  ///< [0, 999999999]
};

// ---------------------------------------------------------------------------
// Construction and normalisation
// ---------------------------------------------------------------------------

/// Builds a timestamp, normalising `nanoseconds` into [0, 999999999] and
/// carrying the excess into the seconds field.
inline timestamp make_timestamp(std::int64_t seconds, std::int64_t nanoseconds) {
  const std::int64_t scale = static_cast<std::int64_t>(detail::nanoseconds_per_second);

  std::int64_t carry = nanoseconds / scale;
  std::int64_t remainder = nanoseconds % scale;
  if (remainder < 0) {
    remainder += scale;
    carry -= 1;
  }

  timestamp result;
  result.seconds = seconds + carry;
  result.nanoseconds = static_cast<std::uint32_t>(remainder);
  return result;
}

/// Builds a duration, normalising `nanoseconds` into [0, 999999999] and
/// carrying the excess into the (signed) seconds field.
inline duration make_duration(std::int64_t seconds, std::int64_t nanoseconds) {
  const std::int64_t scale = static_cast<std::int64_t>(detail::nanoseconds_per_second);

  std::int64_t carry = nanoseconds / scale;
  std::int64_t remainder = nanoseconds % scale;
  if (remainder < 0) {
    remainder += scale;
    carry -= 1;
  }

  duration result;
  result.seconds = seconds + carry;
  result.nanoseconds = static_cast<std::uint32_t>(remainder);
  return result;
}

// ---------------------------------------------------------------------------
// Scalar views
// ---------------------------------------------------------------------------

/// The instant as a fractional number of Unix seconds.
///
/// Convenient for logging and for tests, but a `double` only carries ~15-16
/// significant decimal digits: for a modern Unix timestamp (1.7e9 s) that is
/// roughly sub-microsecond resolution.  Do not use it for precise maths.
inline double timestamp_seconds(const timestamp& value) {
  return static_cast<double>(value.seconds) + static_cast<double>(value.nanoseconds) * 1e-9;
}

/// The difference as a fractional number of seconds.
inline double duration_seconds(const duration& value) {
  return static_cast<double>(value.seconds) + static_cast<double>(value.nanoseconds) * 1e-9;
}

/// Builds a timestamp from a fractional number of Unix seconds.
///
/// Precondition: `value` is finite and well inside the int64 second range.
inline timestamp timestamp_from_seconds(double value) {
  const double whole = std::floor(value);
  const double fraction = value - whole;  // in [0, 1)
  return make_timestamp(static_cast<std::int64_t>(whole),
                        static_cast<std::int64_t>(fraction * 1e9 + 0.5));
}

/// Builds a duration from a fractional number of seconds.
///
/// Precondition: `value` is finite and well inside the int64 second range.
inline duration duration_from_seconds(double value) {
  const double whole = std::floor(value);
  const double fraction = value - whole;  // in [0, 1)
  return make_duration(static_cast<std::int64_t>(whole),
                       static_cast<std::int64_t>(fraction * 1e9 + 0.5));
}

// ---------------------------------------------------------------------------
// std::chrono interoperability
//
// std::chrono::system_clock's epoch is unspecified by the standard but is the
// Unix epoch on every platform ntplite supports, so the conversion below is a
// plain duration cast.
// ---------------------------------------------------------------------------

/// Converts to a `std::chrono::system_clock` time point, as accurately as that
/// clock's resolution allows.
inline std::chrono::system_clock::time_point to_chrono(const timestamp& value) {
  const std::chrono::nanoseconds total =
      std::chrono::seconds(value.seconds) + std::chrono::nanoseconds(value.nanoseconds);
  return std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(total));
}

/// Converts from a `std::chrono::system_clock` time point.
inline timestamp from_chrono(const std::chrono::system_clock::time_point& value) {
  const std::chrono::nanoseconds total =
      std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch());
  const std::chrono::seconds whole = std::chrono::duration_cast<std::chrono::seconds>(total);
  const std::chrono::nanoseconds remainder = total - whole;
  return make_timestamp(whole.count(), remainder.count());
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

/// The difference `lhs - rhs`.
inline duration difference(const timestamp& lhs, const timestamp& rhs) {
  return make_duration(lhs.seconds - rhs.seconds, static_cast<std::int64_t>(lhs.nanoseconds) -
                                                      static_cast<std::int64_t>(rhs.nanoseconds));
}

/// `value` shifted by `delta`.
inline timestamp add_duration(const timestamp& value, const duration& delta) {
  return make_timestamp(value.seconds + delta.seconds,
                        static_cast<std::int64_t>(value.nanoseconds) +
                            static_cast<std::int64_t>(delta.nanoseconds));
}

/// Ordering for timestamps.  Returns a negative value, zero, or a positive
/// value, mirroring `strcmp` / `<=>`.
inline int compare(const timestamp& lhs, const timestamp& rhs) {
  if (lhs.seconds != rhs.seconds) {
    return lhs.seconds < rhs.seconds ? -1 : 1;
  }
  if (lhs.nanoseconds != rhs.nanoseconds) {
    return lhs.nanoseconds < rhs.nanoseconds ? -1 : 1;
  }
  return 0;
}

}  // namespace ntplite

#endif  // NTP_LITE_TIME_HPP
