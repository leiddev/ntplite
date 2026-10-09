/* ===========================================================================
 * ntplite - tests/c_consumer/main.c
 * ---------------------------------------------------------------------------
 * Proves that the public header is genuinely consumable from plain C and that
 * the prebuilt `ntplite::c` library exposes a usable ABI.
 *
 * This file must stay strictly C99: no C++ constructs, no dependence on the
 * C++ test framework, which a C compiler could not read.  The point of the file
 * is that it *is* a C program, so it carries its own few lines of checking.
 *
 * The one thing it deliberately does not do is reach a real NTP server.  A test
 * that depends on the network is a test that fails in a sandbox, so the only
 * query here is one that is guaranteed to fail, and its job is to check what a
 * failure leaves behind.
 * ===========================================================================
 */

#include <ntplite/ntplite.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr)                                                                        \
  do {                                                                                     \
    if (!(expr)) {                                                                         \
      ++failures;                                                                          \
      fprintf(stderr, "c_consumer: %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #expr); \
    }                                                                                      \
  } while (0)

#define CHECK_EQ(actual, expected)                                                          \
  do {                                                                                      \
    const long long check_actual = (long long)(actual);                                     \
    const long long check_expected = (long long)(expected);                                 \
    if (check_actual != check_expected) {                                                   \
      ++failures;                                                                           \
      fprintf(stderr, "c_consumer: %s:%d: %s is %lld, expected %lld\n", __FILE__, __LINE__, \
              #actual, check_actual, check_expected);                                       \
    }                                                                                       \
  } while (0)

#define CHECK_STREQ(actual, expected)                                                           \
  do {                                                                                          \
    if (strcmp((actual), (expected)) != 0) {                                                    \
      ++failures;                                                                               \
      fprintf(stderr, "c_consumer: %s:%d: %s is \"%s\", expected \"%s\"\n", __FILE__, __LINE__, \
              #actual, (actual), (expected));                                                   \
    }                                                                                           \
  } while (0)

/* An address reserved for documentation by RFC 5737: it is guaranteed never to
 * be a real host, so a query to it must fail, quickly and for a reason that does
 * not depend on the network it is run from. */
static const char* const kUnreachable = "203.0.113.1";

/* A replacement for strcpy() that MSVC will not warn about, and that cannot
 * overflow the field it is writing into. */
static void put(char* destination, size_t capacity, const char* text) {
  if (capacity == 0) {
    return;
  }
  snprintf(destination, capacity, "%s", text);
}

static void test_version(void) {
  const char* version = ntplite_version_string();

  CHECK(version != NULL);
  CHECK(version != NULL && version[0] != '\0');
  CHECK_EQ(ntplite_c_api_version(), NTP_LITE_C_API_VERSION);
  CHECK(ntplite_version_major() >= 0);
  CHECK(ntplite_version_minor() >= 0);
  CHECK(ntplite_version_patch() >= 0);
}

static void test_status_strings(void) {
  const ntplite_status_t all[] = {NTP_LITE_OK,          NTP_LITE_ERR_INVALID,
                                  NTP_LITE_ERR_NETWORK, NTP_LITE_ERR_RESOLVE,
                                  NTP_LITE_ERR_TIMEOUT, NTP_LITE_ERR_PROTOCOL,
                                  NTP_LITE_ERR_KOD,     NTP_LITE_ERR_UNSUPPORTED,
                                  NTP_LITE_ERR_INTERNAL};

  const size_t count = sizeof(all) / sizeof(all[0]);
  size_t i = 0;

  CHECK_STREQ(ntplite_status_string(NTP_LITE_OK), "OK");

  for (i = 0; i < count; ++i) {
    const char* text = ntplite_status_string(all[i]);
    size_t j = 0;

    CHECK(text != NULL);
    if (text == NULL || text[0] == '\0') {
      continue;
    }

    /* Every status has its own description: two codes that read the same are
     * two codes a caller cannot tell apart from the message. */
    for (j = 0; j < count; ++j) {
      if (j != i) {
        CHECK(strcmp(text, ntplite_status_string(all[j])) != 0);
      }
    }
  }

  /* A code that is not one of ours must still produce a string, because the
   * caller may hold one and print it. */
  CHECK(ntplite_status_string((ntplite_status_t)1234) != NULL);
}

static void test_options_init(void) {
  ntplite_options_t options;

  memset(&options, 0xAB, sizeof(options));
  ntplite_options_init(&options);

  CHECK_EQ(options.struct_size, sizeof(ntplite_options_t));
  CHECK_EQ(options.port, 123);
  CHECK_EQ(options.ip_version, 0);
  CHECK_EQ(options.ntp_version, 4);
  CHECK_EQ(options.server_timeout_ms, 1000);
  CHECK_EQ(options.total_timeout_ms, 5000);
  CHECK_EQ(options.retry_interval_ms, 400);

  /* A NULL options pointer is documented to be tolerated rather than to crash. */
  ntplite_options_init(NULL);
}

static void test_result_init(void) {
  ntplite_result_t result;

  memset(&result, 0xAB, sizeof(result));
  ntplite_result_init(&result);

  CHECK_EQ(result.struct_size, 0);
  CHECK_EQ(result.valid, 0);
  CHECK_EQ(result.kiss_of_death, 0);
  CHECK_EQ(result.attempts, 0);
  CHECK_EQ(result.stratum, 0);
  CHECK_EQ(result.offset.seconds, 0);
  CHECK_EQ(result.offset.nanoseconds, 0);
  CHECK_EQ(result.server_time.seconds, 0);
  CHECK_EQ(result.server[0], '\0');
  CHECK_EQ(result.reference[0], '\0');
  CHECK_EQ(result.kiss_code[0], '\0');

  ntplite_result_init(NULL);
}

static void test_arguments_are_validated(void) {
  ntplite_result_t result;
  ntplite_options_t options;

  ntplite_options_init(&options);

  /* A NULL result cannot be reported through, so it has to be refused up front. */
  CHECK_EQ(ntplite_query("127.0.0.1", &options, NULL), NTP_LITE_ERR_INVALID);

  ntplite_result_init(&result);
  CHECK_EQ(ntplite_query(NULL, &options, &result), NTP_LITE_ERR_INVALID);
  CHECK_EQ(ntplite_query("", &options, &result), NTP_LITE_ERR_INVALID);

  /* An IP version that is not any/IPv4/IPv6. */
  ntplite_options_init(&options);
  options.ip_version = 5;
  CHECK_EQ(ntplite_query("127.0.0.1", &options, &result), NTP_LITE_ERR_INVALID);

  /* An NTP version that is not 3 or 4 - including one whose low byte happens to
   * be 3, which a careless narrowing cast would have accepted. */
  ntplite_options_init(&options);
  options.ntp_version = 2;
  CHECK_EQ(ntplite_query("127.0.0.1", &options, &result), NTP_LITE_ERR_INVALID);
  options.ntp_version = 259;
  CHECK_EQ(ntplite_query("127.0.0.1", &options, &result), NTP_LITE_ERR_INVALID);

  /* A negative budget is a mistake; zero means "use the default", so only the
   * negative case belongs here. */
  ntplite_options_init(&options);
  options.total_timeout_ms = -1;
  CHECK_EQ(ntplite_query("127.0.0.1", &options, &result), NTP_LITE_ERR_INVALID);

  ntplite_options_init(&options);
  options.server_timeout_ms = -1;
  CHECK_EQ(ntplite_query("127.0.0.1", &options, &result), NTP_LITE_ERR_INVALID);

  ntplite_options_init(&options);
  options.retry_interval_ms = -1;
  CHECK_EQ(ntplite_query("127.0.0.1", &options, &result), NTP_LITE_ERR_INVALID);
}

/* A refused argument must not leave the caller's previous answer in place, and
 * neither must a failed query. */
static void test_failure_clears_the_result(void) {
  ntplite_options_t options;
  ntplite_result_t result;

  ntplite_options_init(&options);

  /* Pretend the caller is reusing a result it already filled in. */
  ntplite_result_init(&result);
  put(result.server, sizeof(result.server), "192.0.2.1:123");
  put(result.reference, sizeof(result.reference), "GPS");
  put(result.kiss_code, sizeof(result.kiss_code), "RATE");
  result.valid = 1;
  result.kiss_of_death = 1;
  result.attempts = 7;
  result.stratum = 2;
  result.offset.seconds = 1;
  result.offset.nanoseconds = 500000000;
  result.struct_size = sizeof(ntplite_result_t);

  /* An empty host is refused before anything is sent, so nothing about this
   * call depends on the state of the network it runs on. */
  CHECK_EQ(ntplite_query("", &options, &result), NTP_LITE_ERR_INVALID);

  CHECK_EQ(result.server[0], '\0');
  CHECK_EQ(result.reference[0], '\0');
  CHECK_EQ(result.kiss_code[0], '\0');
  CHECK_EQ(result.valid, 0);
  CHECK_EQ(result.kiss_of_death, 0);
  CHECK_EQ(result.attempts, 0);
  CHECK_EQ(result.stratum, 0);
  CHECK_EQ(result.offset.seconds, 0);
  CHECK_EQ(result.offset.nanoseconds, 0);

  /* The library writes into the caller's struct and must leave the caller's own
   * record of how big that struct is alone. */
  CHECK_EQ(result.struct_size, sizeof(ntplite_result_t));
}

/* struct_size is the forward compatibility seam: a caller that claims a larger
 * struct than this library knows must be filled in, not written past. */
static void test_struct_size(void) {
  ntplite_result_t result;

  ntplite_result_init(&result);
  result.struct_size = sizeof(ntplite_result_t) + 4096;
  CHECK(ntplite_query(NULL, NULL, &result) == NTP_LITE_ERR_INVALID);
  CHECK_EQ(result.struct_size, sizeof(ntplite_result_t) + 4096);

  /* Zero means "the layout this library was built with", not "no struct". */
  ntplite_result_init(&result);
  result.struct_size = 0;
  CHECK(ntplite_query(NULL, NULL, &result) == NTP_LITE_ERR_INVALID);
  CHECK_EQ(result.struct_size, 0);
}

/* The one query that is guaranteed to fail, and what it must leave behind. */
static void test_a_failed_query_reports_nothing_as_an_answer(void) {
  ntplite_options_t options;
  ntplite_options_t zeroes;
  ntplite_result_t result;
  ntplite_status_t status;

  ntplite_options_init(&options);
  options.total_timeout_ms = 500;
  options.server_timeout_ms = 200;
  options.retry_interval_ms = 50;

  ntplite_result_init(&result);
  status = ntplite_query(kUnreachable, &options, &result);

  CHECK(status != NTP_LITE_OK);
  CHECK_EQ(result.valid, 0);
  CHECK_EQ(result.kiss_of_death, 0);
  CHECK_EQ(result.kiss_code[0], '\0');
  CHECK_EQ(result.server_time.seconds, 0);
  CHECK_EQ(result.offset.seconds, 0);
  CHECK_EQ(result.offset.nanoseconds, 0);

  /* Zeroed options are meant to be as good as ntplite_options_init(): a zero
   * field means "use the default".  If that were wrong, this call would come
   * back as an argument error instead of a network failure.  It costs the
   * default budget of five seconds, unless the address fails sooner. */
  memset(&zeroes, 0, sizeof(zeroes));
  ntplite_result_init(&result);
  status = ntplite_query(kUnreachable, &zeroes, &result);
  CHECK(status != NTP_LITE_ERR_INVALID);

  /* A NULL options pointer means the same thing.  Checked against an argument
   * that is refused outright, so this part is instant. */
  ntplite_result_init(&result);
  CHECK_EQ(ntplite_query("", NULL, &result), NTP_LITE_ERR_INVALID);
}

static void test_format_utc(void) {
  ntplite_timestamp_t time;
  char text[NTPLITE_TIME_TEXT_SIZE];

  /* The epoch itself. */
  time.seconds = 0;
  time.nanoseconds = 0;
  ntplite_format_utc(&time, text, sizeof(text));
  CHECK_STREQ(text, "1970-01-01T00:00:00.000000000Z");

  /* A known instant, with a full nanosecond field: 2026-01-01T00:00:00Z. */
  time.seconds = 1767225600;
  time.nanoseconds = 123456789;
  ntplite_format_utc(&time, text, sizeof(text));
  CHECK_STREQ(text, "2026-01-01T00:00:00.123456789Z");

  /* One second before the epoch: the floor division has to borrow. */
  time.seconds = -1;
  time.nanoseconds = 0;
  ntplite_format_utc(&time, text, sizeof(text));
  CHECK_STREQ(text, "1969-12-31T23:59:59.000000000Z");

  /* Past 2038, which is where a formatter built on a 32 bit time_t would give
   * up.  This is NTP's next era boundary, 2036-02-07T06:28:16Z. */
  time.seconds = 2085978496;
  time.nanoseconds = 0;
  ntplite_format_utc(&time, text, sizeof(text));
  CHECK_STREQ(text, "2036-02-07T06:28:16.000000000Z");

  /* Further out still, to show there is no hidden 32 bit limit. */
  time.seconds = 9999999999LL;
  time.nanoseconds = 999999999;
  ntplite_format_utc(&time, text, sizeof(text));
  CHECK_STREQ(text, "2286-11-20T17:46:39.999999999Z");

  /* A leap day, which is where a hand written calendar usually goes wrong. */
  time.seconds = 1582934400; /* 2020-02-29T00:00:00Z */
  time.nanoseconds = 0;
  ntplite_format_utc(&time, text, sizeof(text));
  CHECK_STREQ(text, "2020-02-29T00:00:00.000000000Z");

  /* The documented size is enough, and the result is never cut short. */
  time.seconds = 1767225600;
  time.nanoseconds = 999999999;
  memset(text, 0x7F, sizeof(text));
  ntplite_format_utc(&time, text, sizeof(text));
  CHECK(strlen(text) < sizeof(text));

  /* Truncation is allowed, but never at the cost of the terminator. */
  memset(text, 0x7F, sizeof(text));
  ntplite_format_utc(&time, text, 8);
  CHECK_EQ(text[7], '\0');
  CHECK_STREQ(text, "2026-01");

  /* And nothing else is ever written. */
  ntplite_format_utc(NULL, text, sizeof(text));
  ntplite_format_utc(&time, NULL, sizeof(text));
  ntplite_format_utc(&time, text, 0);
}

static void test_format_seconds(void) {
  ntplite_duration_t value;
  char text[NTPLITE_SECONDS_TEXT_SIZE];

  value.seconds = 0;
  value.nanoseconds = 0;
  ntplite_format_seconds(&value, text, sizeof(text));
  CHECK_STREQ(text, "+0.000000000");

  value.seconds = 0;
  value.nanoseconds = 1234567;
  ntplite_format_seconds(&value, text, sizeof(text));
  CHECK_STREQ(text, "+0.001234567");

  /* A negative duration is stored as a floored whole part and an unsigned
   * fraction: -0.5 s is {-1, 500000000}.  The formatter has to borrow that
   * second back and print the sign itself. */
  value.seconds = -1;
  value.nanoseconds = 500000000;
  ntplite_format_seconds(&value, text, sizeof(text));
  CHECK_STREQ(text, "-0.500000000");

  value.seconds = -1;
  value.nanoseconds = 999999999;
  ntplite_format_seconds(&value, text, sizeof(text));
  CHECK_STREQ(text, "-0.000000001");

  value.seconds = -1;
  value.nanoseconds = 0;
  ntplite_format_seconds(&value, text, sizeof(text));
  CHECK_STREQ(text, "-1.000000000");

  /* Real offsets are small, but the fields are 64 bits wide: a wildly wrong
   * clock still has to print as a number rather than overflow. */
  value.seconds = 31536000;
  value.nanoseconds = 1;
  ntplite_format_seconds(&value, text, sizeof(text));
  CHECK_STREQ(text, "+31536000.000000001");

  memset(text, 0x7F, sizeof(text));
  ntplite_format_seconds(&value, text, sizeof(text));
  CHECK(strlen(text) < sizeof(text));

  ntplite_format_seconds(NULL, text, sizeof(text));
  ntplite_format_seconds(&value, NULL, sizeof(text));
  ntplite_format_seconds(&value, text, 0);
}

int main(void) {
  test_version();
  test_status_strings();
  test_options_init();
  test_result_init();
  test_arguments_are_validated();
  test_failure_clears_the_result();
  test_struct_size();
  test_a_failed_query_reports_nothing_as_an_answer();
  test_format_utc();
  test_format_seconds();

  if (failures != 0) {
    fprintf(stderr, "c_consumer: %d check(s) failed\n", failures);
    return 1;
  }

  printf("c_consumer: ok (ntplite %s, c abi %d)\n", ntplite_version_string(),
         ntplite_c_api_version());
  return 0;
}
