// ============================================================================
// ntplite - detail/ntp_time.hpp
// ----------------------------------------------------------------------------
// On-the-wire NTP time formats.
//
// NTP timestamps are 64-bit *unsigned fixed point* values: 32 bits of seconds
// since 1900-01-01T00:00:00Z followed by 32 bits of binary fraction of a
// second.  The fraction is therefore in units of 2^-32 s, about 233 ps.
//
// Two things make this awkward and worth isolating in one place:
//
//  1. the epoch is 1900, not 1970, so every conversion needs the 2208988800 s
//     offset;
//  2. the seconds field is only 32 bits wide, so it wraps every 2^32 s
//     (~136 years).  This is the "NTP era" problem: a raw 32-bit value is
//     ambiguous unless something tells us which era it belongs to.  Era 0
//     covers 1900-2036, era 1 covers 2036-2172, and so on.
//
// Everything here is internal but deliberately free of I/O and globals, so it
// is trivially unit-testable.
// ============================================================================

#ifndef NTP_LITE_DETAIL_NTP_TIME_HPP
#define NTP_LITE_DETAIL_NTP_TIME_HPP

#include <cstdint>
#include <ntplite/detail/config.hpp>

namespace ntplite {
namespace detail {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Seconds between the NTP epoch (1900-01-01T00:00:00Z) and the Unix epoch
/// (1970-01-01T00:00:00Z): 70 years, 17 of them leap (1900 is not one), so
/// 70 * 365 + 17 = 25567 days = 2208988800 s.
///
/// This is commonly written as the hex constant 0x83AA7E80 on the wire.
const std::int64_t ntp_to_unix_epoch_seconds = 2208988800LL;

/// Width of one NTP era: the number of seconds a 32-bit field can hold.
const std::int64_t ntp_era_seconds = 4294967296LL;  // 2^32

/// Nanoseconds in one second.
const std::uint32_t nanoseconds_per_second = 1000000000U;

/// Denominator of the NTP fractional part.
const std::uint64_t ntp_fraction_scale = 4294967296ULL;  // 2^32

/// Denominator of the 16.16 fractional part used by the root delay and root
/// dispersion fields.
const std::uint32_t ntp_short_fraction_scale = 65536U;  // 2^16

// ---------------------------------------------------------------------------
// Wire representations
// ---------------------------------------------------------------------------

/// A 64-bit NTP timestamp: 32.32 unsigned fixed point seconds since 1900.
struct ntp_timestamp {
  std::uint32_t seconds;   ///< seconds since 1900-01-01T00:00:00Z, modulo 2^32
  std::uint32_t fraction;  ///< binary fraction in units of 2^-32 s
};

/// A 32-bit NTP short format: 16.16 unsigned fixed point seconds.
/// Used by the root delay and root dispersion packet fields.
struct ntp_short {
  std::uint16_t seconds;
  std::uint16_t fraction;
};

/// The result of converting an NTP timestamp to civil time.
struct unix_parts {
  std::int64_t seconds;       ///< seconds since 1970-01-01T00:00:00Z
  std::uint32_t nanoseconds;  ///< [0, 999999999]
};

// ---------------------------------------------------------------------------
// Fraction conversions
// ---------------------------------------------------------------------------

/// Converts a 2^-32 binary fraction to nanoseconds.
///
/// The result is truncated, matching the classic NTP implementations.  The
/// discarded remainder is always below one nanosecond, and the result is
/// guaranteed to stay inside [0, 999999999] without any clamping:
/// (2^32 - 1) * 10^9 / 2^32 = 999999999.767...
inline std::uint32_t fraction_to_nanoseconds(std::uint32_t fraction) {
  const std::uint64_t scaled =
      static_cast<std::uint64_t>(fraction) * static_cast<std::uint64_t>(nanoseconds_per_second);
  return static_cast<std::uint32_t>(scaled >> 32);
}

/// Converts nanoseconds to a 2^-32 binary fraction, rounding to nearest.
///
/// Rounding (rather than truncating) keeps `ns -> fraction -> ns` round trips
/// within half a step instead of a full one.  The largest possible result is
/// 999999999 * 2^32 / 10^9 = 4294967292.2, so the value always fits in 32 bits.
inline std::uint32_t nanoseconds_to_fraction(std::uint32_t nanoseconds) {
  const std::uint64_t scaled =
      static_cast<std::uint64_t>(nanoseconds) * ntp_fraction_scale + (nanoseconds_per_second / 2);
  return static_cast<std::uint32_t>(scaled / static_cast<std::uint64_t>(nanoseconds_per_second));
}

// ---------------------------------------------------------------------------
// Era handling
// ---------------------------------------------------------------------------

/// Determines which NTP era a raw 32-bit second count belongs to.
///
/// `reference_unix_seconds` is our best guess at the current time - in
/// practice the local clock.  The era is chosen so that the decoded instant is
/// the one *closest* to that reference, which is the behaviour of production
/// NTP daemons and is valid as long as the real time is not more than ~68
/// years away from the reference.
///
/// Examples, with a reference of 2026-01-01:
///   * a raw value of 3976214400 is era 0 (2026);
///   * a raw value of 0 is era 1 (2036), not era 0 (1900), because 2036 is
///     closer to 2026 than 1900 is.
inline int resolve_era(std::uint32_t ntp_seconds, std::int64_t reference_unix_seconds) {
  const std::int64_t reference_ntp = reference_unix_seconds + ntp_to_unix_epoch_seconds;

  // floor(reference_ntp / 2^32), written so that it also works for instants
  // before the NTP epoch (where C++ integer division truncates toward zero).
  std::int64_t era = reference_ntp / ntp_era_seconds;
  if (reference_ntp < 0 && reference_ntp % ntp_era_seconds != 0) {
    era -= 1;
  }

  std::int64_t candidate = era * ntp_era_seconds + static_cast<std::int64_t>(ntp_seconds);
  const std::int64_t half_era = ntp_era_seconds / 2;

  if (candidate - reference_ntp > half_era) {
    era -= 1;
  } else if (reference_ntp - candidate > half_era) {
    era += 1;
  }

  return static_cast<int>(era);
}

/// Converts a raw 32-bit NTP second count to Unix seconds, picking the era
/// closest to `reference_unix_seconds`.
inline std::int64_t ntp_seconds_to_unix(std::uint32_t ntp_seconds,
                                        std::int64_t reference_unix_seconds) {
  const int era = resolve_era(ntp_seconds, reference_unix_seconds);
  const std::int64_t absolute_seconds =
      static_cast<std::int64_t>(era) * ntp_era_seconds + static_cast<std::int64_t>(ntp_seconds);
  return absolute_seconds - ntp_to_unix_epoch_seconds;
}

/// Converts Unix seconds to the raw 32-bit NTP second count, discarding the
/// era (i.e. reducing modulo 2^32).
inline std::uint32_t unix_seconds_to_ntp(std::int64_t unix_seconds) {
  const std::int64_t absolute_seconds = unix_seconds + ntp_to_unix_epoch_seconds;

  std::int64_t wrapped = absolute_seconds % ntp_era_seconds;
  if (wrapped < 0) {
    wrapped += ntp_era_seconds;
  }

  return static_cast<std::uint32_t>(wrapped);
}

// ---------------------------------------------------------------------------
// Whole-timestamp conversions
// ---------------------------------------------------------------------------

/// Converts an on-wire NTP timestamp to Unix seconds plus nanoseconds.
///
/// The era is inferred from the *whole seconds* field only; the sub-second
/// part never changes which era is correct.
inline unix_parts ntp_timestamp_to_unix(const ntp_timestamp& value,
                                        std::int64_t reference_unix_seconds) {
  unix_parts result;
  result.seconds = ntp_seconds_to_unix(value.seconds, reference_unix_seconds);
  result.nanoseconds = fraction_to_nanoseconds(value.fraction);
  return result;
}

/// Converts Unix seconds plus nanoseconds to an on-wire NTP timestamp.
///
/// `nanoseconds` must already be normalised into [0, 999999999]; values that
/// are out of range are not clamped, they simply wrap - callers hold the only
/// sensible invariant, so this function stays branch free.
inline ntp_timestamp unix_to_ntp_timestamp(std::int64_t unix_seconds, std::uint32_t nanoseconds) {
  ntp_timestamp result;
  result.seconds = unix_seconds_to_ntp(unix_seconds);
  result.fraction = nanoseconds_to_fraction(nanoseconds);
  return result;
}

// ---------------------------------------------------------------------------
// 16.16 short format (root delay / root dispersion)
// ---------------------------------------------------------------------------

/// Converts a 16.16 fixed point value to nanoseconds.
inline std::uint64_t ntp_short_to_nanoseconds(const ntp_short& value) {
  const std::uint64_t whole = static_cast<std::uint64_t>(value.seconds) *
                              static_cast<std::uint64_t>(nanoseconds_per_second);
  const std::uint64_t fraction =
      (static_cast<std::uint64_t>(value.fraction) * nanoseconds_per_second) >> 16;
  return whole + fraction;
}

/// Converts nanoseconds to a 16.16 fixed point value, rounding to nearest and
/// saturating at the largest representable value, 65535.99998 s.
inline ntp_short nanoseconds_to_ntp_short(std::uint64_t nanoseconds) {
  const std::uint64_t whole = nanoseconds / nanoseconds_per_second;
  const std::uint64_t remainder = nanoseconds % nanoseconds_per_second;

  ntp_short result;
  if (whole > 65535ULL) {
    // Saturate: everything at or beyond 65536 s is clamped to the largest
    // representable 16.16 value.  Exactly 65535 s is still representable and
    // falls through to the exact conversion below.
    result.seconds = 65535U;
    result.fraction = 65535U;
    return result;
  }

  const std::uint64_t fraction =
      (remainder * ntp_short_fraction_scale + (nanoseconds_per_second / 2)) /
      nanoseconds_per_second;

  result.seconds = static_cast<std::uint16_t>(whole);
  result.fraction = static_cast<std::uint16_t>(fraction > 65535ULL ? 65535ULL : fraction);
  return result;
}

}  // namespace detail
}  // namespace ntplite

#endif  // NTP_LITE_DETAIL_NTP_TIME_HPP
