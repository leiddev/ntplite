// ============================================================================
// ntplite - include/ntplite/detail/civil_time.hpp
// ----------------------------------------------------------------------------
// Turning a timestamp into text, without going through the C library.
//
// NTP is about to change era: its 64 bit timestamp wraps in 2036, and the point
// of resolving the era against the local clock (see ntp_time.hpp) is that
// ntplite can name an instant on either side of that boundary.  A formatter
// built on time_t would be the one part of the library that could not: time_t
// is 32 bits on some platforms, where it refuses anything past 2038.
//
// std::gmtime() is also not thread safe, and is deprecated by MSVC.  So the
// calendar arithmetic is done here instead, in the open, where it can be
// checked against known dates.
// ============================================================================

#ifndef NTPLITE_DETAIL_CIVIL_TIME_HPP
#define NTPLITE_DETAIL_CIVIL_TIME_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ntplite/time.hpp>

namespace ntplite {
namespace detail {

/// An instant broken down into the fields of a calendar date, in UTC.
struct civil_time {
  civil_time();
  int year;                  ///< e.g. 2026
  int month;                 ///< 1 .. 12
  int day;                   ///< 1 .. 31
  int hour;                  ///< 0 .. 23
  int minute;                ///< 0 .. 59
  int second;                ///< 0 .. 59; posix time has no room for a leap second,
                             ///< so none ever appears here
  std::uint32_t nanosecond;  ///< 0 .. 999999999
};

inline civil_time::civil_time()
    : year(1970), month(1), day(1), hour(0), minute(0), second(0), nanosecond(0) {}

/// The number of seconds in a day.  NTP's day, like everyone else's, is 86400
/// seconds long: a leap second is smeared by the kernel or repeated by the
/// clock, it is not a 86401st second of an ordinary day.
const std::int64_t seconds_per_day = 86400;

/// Days since 1970-01-01 to year/month/day.
///
/// This is Howard Hinnant's `civil_from_days`, which is exact for the whole
/// range of the integer type and needs no lookup tables or branches per month.
/// The `shifted - 146096` before dividing is a floor division by 146097: C++
/// truncates towards zero, and an era has to be counted from -infinity for a
/// date before 1970 to come out right.
inline void civil_from_days(std::int64_t days, int& year, unsigned& month, unsigned& day) {
  const std::int64_t shifted = days + 719468;
  const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(shifted - era * 146097);  // [0, 146096]
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) /
      365;  // [0, 399]
  const std::int64_t base_year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);  // [0, 365]
  const unsigned march_month = (5 * day_of_year + 2) / 153;                    // [0, 11]
  day = day_of_year - (153 * march_month + 2) / 5 + 1;                         // [1, 31]
  month = march_month < 10 ? march_month + 3 : march_month - 9;                // [1, 12]
  year = static_cast<int>(base_year + (month <= 2 ? 1 : 0));
}

/// Breaks a timestamp down into UTC calendar fields.
inline civil_time civil_from_timestamp(const timestamp& value) {
  civil_time out;

  // The sub-second part is unsigned and non-negative, so only the whole seconds
  // need a floor division to keep the remainder non-negative too.
  std::int64_t days = value.seconds / seconds_per_day;
  std::int64_t remainder = value.seconds % seconds_per_day;
  if (remainder < 0) {
    remainder += seconds_per_day;
    --days;
  }

  unsigned month = 1;
  unsigned day = 1;
  civil_from_days(days, out.year, month, day);

  out.month = static_cast<int>(month);
  out.day = static_cast<int>(day);
  out.hour = static_cast<int>(remainder / 3600);
  out.minute = static_cast<int>((remainder % 3600) / 60);
  out.second = static_cast<int>(remainder % 60);
  out.nanosecond = value.nanoseconds;
  return out;
}

/// Room for "YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ" and its terminator, with room to
/// spare for a year outside four digits.
const std::size_t utc_text_capacity = 40;

/// Room for "-9223372036854775808.000000000" and its terminator.
const std::size_t signed_seconds_text_capacity = 40;

/// Copies `text` into a caller's buffer, keeping at most `capacity - 1`
/// characters and always terminating.
///
/// A buffer that is too short is filled as far as it goes.  Half a timestamp is
/// not much use, but it is the caller's buffer and the caller's choice, and it is
/// not a buffer overrun either.
inline void copy_text(char* out, std::size_t capacity, const char* text) {
  if (out == NULL || capacity == 0) {
    return;
  }

  std::size_t length = 0;
  while (length + 1 < capacity && text[length] != '\0') {
    ++length;
  }
  std::memcpy(out, text, length);
  out[length] = '\0';
}

/// Writes an instant as ISO 8601 in UTC, e.g. "2026-10-09T12:34:56.789012345Z".
///
/// `out` needs `utc_text_capacity` bytes.  A longer year widens the text rather
/// than being cut off - a date is only useful if it is the right date.
///
/// The text is built in a buffer that is always big enough and then copied into
/// the caller's.  Formatting straight into the caller's buffer would let a
/// compiler conclude that the text may not fit, and -Wformat-truncation would
/// then object to a truncation it can prove is possible - even though truncating
/// is the documented behaviour here, and a diagnostic that has to be silenced is
/// worse than code that never provokes it.
inline void format_utc(const timestamp& value, char* out, std::size_t capacity) {
  if (out == NULL || capacity == 0) {
    return;
  }

  // At most 11 characters of year, then the fixed rest: 37 characters and a
  // terminator, so utc_text_capacity is enough for any year an int can hold.
  const civil_time parts = civil_from_timestamp(value);
  char text[utc_text_capacity];
  std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%09luZ", parts.year, parts.month,
                parts.day, parts.hour, parts.minute, parts.second,
                static_cast<unsigned long>(parts.nanosecond));
  copy_text(out, capacity, text);
}

/// Writes a duration as seconds with a sign and nine decimals, e.g.
/// "+0.001234567" or "-0.500000000".
///
/// The sign is always printed, because for an offset the sign is the whole
/// point: a caller reading "+0.001" needs to know the local clock is fast, and
/// "0.001" alone would leave it to guess.
inline void format_signed_seconds(const duration& value, char* out, std::size_t capacity) {
  if (out == NULL || capacity == 0) {
    return;
  }

  // `nanoseconds` is unsigned even for a negative duration, so -0.5 s is
  // {-1, 500000000}: the magnitude cannot be read off the two fields directly,
  // a second has to be borrowed back from the whole part.
  std::int64_t whole = value.seconds;
  std::int64_t fraction = value.nanoseconds;
  const bool negative = whole < 0;
  if (negative) {
    whole = -whole;
    if (fraction != 0) {
      --whole;
      fraction = 1000000000LL - fraction;
    }
  }

  // At most 20 characters of whole part, then the sign, the point and nine
  // decimals: 31 characters and a terminator, well inside the capacity above.
  char text[signed_seconds_text_capacity];
  std::snprintf(text, sizeof(text), "%s%lld.%09lld", negative ? "-" : "+",
                static_cast<long long>(whole), static_cast<long long>(fraction));
  copy_text(out, capacity, text);
}

}  // namespace detail
}  // namespace ntplite

#endif  // NTPLITE_DETAIL_CIVIL_TIME_HPP
