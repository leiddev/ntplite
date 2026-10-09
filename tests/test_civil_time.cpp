// ============================================================================
// ntplite - tests/test_civil_time.cpp
// ----------------------------------------------------------------------------
// Turning instants into text.
//
// The calendar arithmetic is done by hand rather than through gmtime(), because
// a 32 bit time_t cannot describe the far side of the era boundary NTP is
// heading for.  Hand written calendar code is where off-by-one errors like to
// live, so the cases here are chosen to be the ones that catch them: the day
// before the epoch, the last day of each month, a leap day, and the two
// centuries where the leap year rule turns the other way.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <ntplite/detail/civil_time.hpp>
#include <ntplite/time.hpp>
#include <ntplite_test.hpp>
#include <string>

namespace {

using ::ntplite::detail::civil_from_days;
using ::ntplite::detail::civil_from_timestamp;
using ::ntplite::detail::civil_time;
using ::ntplite::detail::format_signed_seconds;
using ::ntplite::detail::format_utc;
using ::ntplite::detail::signed_seconds_text_capacity;
using ::ntplite::detail::utc_text_capacity;
using ::ntplite::duration;
using ::ntplite::timestamp;

timestamp at(std::int64_t seconds, std::uint32_t nanoseconds) {
  return ::ntplite::make_timestamp(seconds, nanoseconds);
}

/// A duration built from nanoseconds.  make_duration() normalises, so this is
/// how a negative duration has to be spelled: -1 ns is {-1, 999999999}, not
/// {0, -1}, and printing the two fields as they stand would be wrong.
duration nanos(std::int64_t value) {
  return ::ntplite::make_duration(0, value);
}

/// Formats an instant and returns the text, for comparison in one expression.
std::string utc_text(const timestamp& value) {
  char text[utc_text_capacity];
  std::memset(text, 0x7F, sizeof(text));
  format_utc(value, text, sizeof(text));
  return std::string(text);
}

std::string seconds_text(const duration& value) {
  char text[signed_seconds_text_capacity];
  std::memset(text, 0x7F, sizeof(text));
  format_signed_seconds(value, text, sizeof(text));
  return std::string(text);
}

}  // namespace

// ---------------------------------------------------------------------------
// The calendar
// ---------------------------------------------------------------------------

NTP_TEST(civil_time, the_epoch_is_1970) {
  const civil_time parts = civil_from_timestamp(at(0, 0));

  NTP_TEST_CHECK_EQ(parts.year, 1970);
  NTP_TEST_CHECK_EQ(parts.month, 1);
  NTP_TEST_CHECK_EQ(parts.day, 1);
  NTP_TEST_CHECK_EQ(parts.hour, 0);
  NTP_TEST_CHECK_EQ(parts.minute, 0);
  NTP_TEST_CHECK_EQ(parts.second, 0);
  NTP_TEST_CHECK_EQ(parts.nanosecond, 0U);
}

NTP_TEST(civil_time, a_known_instant_keeps_its_nanoseconds) {
  // 2026-01-01T00:00:00Z
  const civil_time parts = civil_from_timestamp(at(1767225600LL, 123456789U));

  NTP_TEST_CHECK_EQ(parts.year, 2026);
  NTP_TEST_CHECK_EQ(parts.month, 1);
  NTP_TEST_CHECK_EQ(parts.day, 1);
  NTP_TEST_CHECK_EQ(parts.nanosecond, 123456789U);
}

NTP_TEST(civil_time, before_the_epoch_floors_towards_the_past) {
  // The interesting failure here is a truncated division, which would answer
  // 1970-01-01T00:00:00 for -1: the sign has to floor, not round towards zero.
  const civil_time parts = civil_from_timestamp(at(-1, 0));

  NTP_TEST_CHECK_EQ(parts.year, 1969);
  NTP_TEST_CHECK_EQ(parts.month, 12);
  NTP_TEST_CHECK_EQ(parts.day, 31);
  NTP_TEST_CHECK_EQ(parts.hour, 23);
  NTP_TEST_CHECK_EQ(parts.minute, 59);
  NTP_TEST_CHECK_EQ(parts.second, 59);

  // One whole day back, and a lot of them: 1900-01-01T00:00:00Z.
  const civil_time before = civil_from_timestamp(at(-2208988800LL, 0));
  NTP_TEST_CHECK_EQ(before.year, 1900);
  NTP_TEST_CHECK_EQ(before.month, 1);
  NTP_TEST_CHECK_EQ(before.day, 1);
}

NTP_TEST(civil_time, the_last_day_of_every_month_wraps_correctly) {
  const std::int64_t january_31 = 1769817600LL; /* 2026-01-31 */
  const civil_time january = civil_from_timestamp(at(january_31, 0));
  NTP_TEST_CHECK_EQ(january.month, 1);
  NTP_TEST_CHECK_EQ(january.day, 31);

  const civil_time february = civil_from_timestamp(at(1772236800LL, 0)); /* 2026-02-28 */
  NTP_TEST_CHECK_EQ(february.month, 2);
  NTP_TEST_CHECK_EQ(february.day, 28);

  // The day after the shortest month: the month and the day both have to move.
  const civil_time march = civil_from_timestamp(at(1772236800LL + 86400, 0));
  NTP_TEST_CHECK_EQ(march.month, 3);
  NTP_TEST_CHECK_EQ(march.day, 1);

  const civil_time april = civil_from_timestamp(at(1777507200LL, 0)); /* 2026-04-30 */
  NTP_TEST_CHECK_EQ(april.month, 4);
  NTP_TEST_CHECK_EQ(april.day, 30);

  const civil_time december = civil_from_timestamp(at(1798675200LL, 0)); /* 2026-12-31 */
  NTP_TEST_CHECK_EQ(december.month, 12);
  NTP_TEST_CHECK_EQ(december.day, 31);
  NTP_TEST_CHECK_EQ(december.year, 2026);
}

NTP_TEST(civil_time, a_leap_day_exists) {
  const civil_time hundred_year = civil_from_timestamp(at(951782400LL, 0)); /* 2000-02-29 */
  NTP_TEST_CHECK_EQ(hundred_year.year, 2000);
  NTP_TEST_CHECK_EQ(hundred_year.month, 2);
  NTP_TEST_CHECK_EQ(hundred_year.day, 29);

  const civil_time four_year = civil_from_timestamp(at(1582934400LL, 0)); /* 2020-02-29 */
  NTP_TEST_CHECK_EQ(four_year.year, 2020);
  NTP_TEST_CHECK_EQ(four_year.month, 2);
  NTP_TEST_CHECK_EQ(four_year.day, 29);
}

NTP_TEST(civil_time, handles_the_centuries_where_the_leap_rule_turns) {
  // 1900 is divisible by 100 but not by 400, so it has no 29th of February.
  const civil_time january_1900 = civil_from_timestamp(at(-2206396800LL, 0));
  NTP_TEST_CHECK_EQ(january_1900.year, 1900);
  NTP_TEST_CHECK_EQ(january_1900.month, 1);
  NTP_TEST_CHECK_EQ(january_1900.day, 31);

  const civil_time february_1900 = civil_from_timestamp(at(-2203977600LL, 0));
  NTP_TEST_CHECK_EQ(february_1900.year, 1900);
  NTP_TEST_CHECK_EQ(february_1900.month, 2);
  NTP_TEST_CHECK_EQ(february_1900.day, 28);

  const civil_time march_1900 = civil_from_timestamp(at(-2203891200LL, 0));
  NTP_TEST_CHECK_EQ(march_1900.year, 1900);
  NTP_TEST_CHECK_EQ(march_1900.month, 3);
  NTP_TEST_CHECK_EQ(march_1900.day, 1);

  // 2100 is the next one, and it is the era a 32 bit time_t cannot reach.
  const civil_time february_2100 = civil_from_timestamp(at(4107456000LL, 0));
  NTP_TEST_CHECK_EQ(february_2100.year, 2100);
  NTP_TEST_CHECK_EQ(february_2100.month, 2);
  NTP_TEST_CHECK_EQ(february_2100.day, 28);

  const civil_time march_2100 = civil_from_timestamp(at(4107542400LL, 0));
  NTP_TEST_CHECK_EQ(march_2100.year, 2100);
  NTP_TEST_CHECK_EQ(march_2100.month, 3);
  NTP_TEST_CHECK_EQ(march_2100.day, 1);
}

NTP_TEST(civil_time, counts_days_without_going_through_a_timestamp) {
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;

  civil_from_days(0, year, month, day);
  NTP_TEST_CHECK_EQ(year, 1970);
  NTP_TEST_CHECK_EQ(month, 1U);
  NTP_TEST_CHECK_EQ(day, 1U);

  civil_from_days(-1, year, month, day);
  NTP_TEST_CHECK_EQ(year, 1969);
  NTP_TEST_CHECK_EQ(month, 12U);
  NTP_TEST_CHECK_EQ(day, 31U);

  civil_from_days(1, year, month, day);
  NTP_TEST_CHECK_EQ(year, 1970);
  NTP_TEST_CHECK_EQ(day, 2U);

  // An exact multiple of the 400 year cycle, which is where the era arithmetic
  // divides evenly and an off-by-one would show up as a year out by 400.
  civil_from_days(146097, year, month, day);
  NTP_TEST_CHECK_EQ(year, 2370);
  NTP_TEST_CHECK_EQ(month, 1U);
  NTP_TEST_CHECK_EQ(day, 1U);
}

// ---------------------------------------------------------------------------
// The text
// ---------------------------------------------------------------------------

NTP_TEST(civil_time, format_utc_writes_iso_8601) {
  NTP_TEST_CHECK_EQ(utc_text(at(0, 0)), std::string("1970-01-01T00:00:00.000000000Z"));
  NTP_TEST_CHECK_EQ(utc_text(at(1767225600LL, 123456789U)),
                    std::string("2026-01-01T00:00:00.123456789Z"));
  NTP_TEST_CHECK_EQ(utc_text(at(-1, 0)), std::string("1969-12-31T23:59:59.000000000Z"));

  // The far side of NTP's next era boundary, and a date well past any 32 bit
  // time_t.
  NTP_TEST_CHECK_EQ(utc_text(at(2085978496LL, 0)), std::string("2036-02-07T06:28:16.000000000Z"));
  NTP_TEST_CHECK_EQ(utc_text(at(9999999999LL, 999999999U)),
                    std::string("2286-11-20T17:46:39.999999999Z"));
}

NTP_TEST(civil_time, format_utc_pads_every_field_to_two_digits) {
  // A date in the first nine days of a month, and a time in the first nine
  // microseconds of a day: every field is one digit wide or fewer, so the text
  // stops being parseable unless each one is padded.
  // 2025-12-09T00:00:00.000005000Z
  NTP_TEST_CHECK_EQ(utc_text(at(1767225600LL - 86400 * 23, 5000U)),
                    std::string("2025-12-09T00:00:00.000005000Z"));
}

NTP_TEST(civil_time, format_utc_truncates_rather_than_overflowing) {
  char text[utc_text_capacity];

  // Deliberately given less room than the text needs: the documented promise is
  // that a short buffer is filled as far as it goes rather than overflowed.
  std::memset(text, 0x7F, sizeof(text));
  format_utc(at(1767225600LL, 0), text, 8);

  // Eight bytes hold seven characters and the terminator: half a date is not
  // useful, but it is not a buffer overrun either.
  NTP_TEST_CHECK_STREQ(text, "2026-01");
  NTP_TEST_CHECK_EQ(text[7], '\0');

  // Nothing at all is written without room, or without a buffer.
  format_utc(at(1767225600LL, 0), text, 0);
  format_utc(at(1767225600LL, 0), NULL, sizeof(text));
  format_utc(at(1767225600LL, 0), NULL, 0);
}

NTP_TEST(civil_time, format_utc_fits_the_documented_size) {
  char text[utc_text_capacity];

  // The longest ordinary text: four digit year, nine digit fraction.
  format_utc(at(1767225600LL, 999999999U), text, sizeof(text));
  NTP_TEST_CHECK(std::strlen(text) < utc_text_capacity);

  // And the shortest, which is the same length.
  format_utc(at(0, 0), text, sizeof(text));
  NTP_TEST_CHECK(std::strlen(text) < utc_text_capacity);
}

NTP_TEST(civil_time, format_signed_seconds_always_shows_the_sign) {
  NTP_TEST_CHECK_EQ(seconds_text(nanos(0)), std::string("+0.000000000"));
  NTP_TEST_CHECK_EQ(seconds_text(nanos(1)), std::string("+0.000000001"));
  NTP_TEST_CHECK_EQ(seconds_text(nanos(1234567)), std::string("+0.001234567"));
  NTP_TEST_CHECK_EQ(seconds_text(nanos(1000000000LL)), std::string("+1.000000000"));
  NTP_TEST_CHECK_EQ(seconds_text(nanos(-1)), std::string("-0.000000001"));
  NTP_TEST_CHECK_EQ(seconds_text(nanos(-1000000000LL)), std::string("-1.000000000"));
  NTP_TEST_CHECK_EQ(seconds_text(nanos(31536000000000001LL)), std::string("+31536000.000000001"));
}

NTP_TEST(civil_time, format_signed_seconds_borrows_a_second_from_negative_values) {
  // -0.5 s is stored as {-1, 500000000}: the whole part is floored and the
  // fraction stays unsigned.  Printing the two fields as they stand would give
  // "-1.500000000", which is a different half second.
  NTP_TEST_CHECK_EQ(seconds_text(nanos(-500000000LL)), std::string("-0.500000000"));
  NTP_TEST_CHECK_EQ(seconds_text(nanos(-1500000000LL)), std::string("-1.500000000"));
  NTP_TEST_CHECK_EQ(seconds_text(nanos(-999999999LL)), std::string("-0.999999999"));

  const duration borrowed = nanos(-500000000LL);
  NTP_TEST_CHECK_EQ(borrowed.seconds, -1LL);
  NTP_TEST_CHECK_EQ(borrowed.nanoseconds, 500000000U);
}

NTP_TEST(civil_time, format_signed_seconds_handles_a_buffer_it_cannot_fill) {
  char text[signed_seconds_text_capacity];

  // Deliberately too small, like the date case above.
  std::memset(text, 0x7F, sizeof(text));
  format_signed_seconds(nanos(1234567), text, 4);

  NTP_TEST_CHECK_STREQ(text, "+0.");
  NTP_TEST_CHECK_EQ(text[3], '\0');

  // A duration is passed by reference, so there is no such thing as a null one
  // to guard against; a null buffer and a zero capacity both have to be safe.
  format_signed_seconds(nanos(1234567), text, 0);
  format_signed_seconds(nanos(1234567), NULL, sizeof(text));
}
