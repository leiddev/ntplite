/* ===========================================================================
 * ntplite - ntplite.h
 * ---------------------------------------------------------------------------
 * Stable, C-compatible public API for ntplite.
 *
 * ntplite is implemented in C++11 but exposes a flat `extern "C"` ABI, so this
 * header can be consumed from plain C, C++, or any language with C FFI support
 * (Rust, Python ctypes, Go cgo, ...).
 *
 * ---------------------------------------------------------------------------
 * How to use it
 * ---------------------------------------------------------------------------
 * Option A - prebuilt library (recommended; nothing to remember):
 *
 *     target_link_libraries(my_app PRIVATE ntplite::c)
 *     #include <ntplite/ntplite.h>
 *
 * Option B - header-only, stb style.  In EXACTLY ONE C++ translation unit:
 *
 *     #define NTP_LITE_IMPLEMENTATION
 *     #include <ntplite/ntplite.h>
 *
 *   Every other translation unit (C or C++) just includes the header normally.
 *
 *   Because the implementation is C++ you cannot compile the implementation
 *   unit with a C compiler; the `#error` below makes that explicit.
 *
 * Option C - C++ consumers should prefer <ntplite/ntplite.hpp> instead, which
 * is fully inline and needs no implementation macro at all.
 *
 * ---------------------------------------------------------------------------
 * Never mix option A with option B: doing so defines every symbol twice and
 * you will get a linker error ("duplicate symbol" / LNK2005).
 * ===========================================================================
 */

#ifndef NTP_LITE_NTP_LITE_H
#define NTP_LITE_NTP_LITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * API / ABI versioning
 * ---------------------------------------------------------------------------
 * NTP_LITE_C_API_VERSION is bumped whenever the C ABI changes in an
 * incompatible way.  Call ntplite_c_api_version() at runtime to detect a
 * mismatch between the header you compiled against and the library you linked.
 */
#define NTP_LITE_C_API_VERSION 1

/* ---------------------------------------------------------------------------
 * Status codes
 * ---------------------------------------------------------------------------
 * The numeric values are spelled out explicitly because they are part of the
 * ABI and must never move.
 *
 * The underlying type is pinned to `int` in C++ only.  Without that, C++ would
 * give the enumeration the narrowest bit-field capable of holding the
 * enumerators (0..8 -> 4 bits), and storing anything outside that range -
 * which happens routinely when the value arrives from C, from an FFI binding,
 * or from a corrupted buffer - would be undefined behaviour on load.  C
 * already treats enumeration objects as int-sized, so both declarations agree
 * on every supported compiler.
 */
#if defined(__cplusplus)
typedef enum ntplite_status : int {
#else
typedef enum ntplite_status {
#endif
  NTP_LITE_OK = 0,              /* success                                */
  NTP_LITE_ERR_INVALID = 1,     /* invalid argument                       */
  NTP_LITE_ERR_NETWORK = 2,     /* socket create / send / recv failure    */
  NTP_LITE_ERR_RESOLVE = 3,     /* hostname could not be resolved         */
  NTP_LITE_ERR_TIMEOUT = 4,     /* no reply within the requested timeout  */
  NTP_LITE_ERR_PROTOCOL = 5,    /* malformed / unexpected NTP reply       */
  NTP_LITE_ERR_KOD = 6,         /* server sent a Kiss-o'-Death packet     */
  NTP_LITE_ERR_UNSUPPORTED = 7, /* unsupported platform or configuration  */
  NTP_LITE_ERR_INTERNAL = 8     /* unexpected internal failure            */
} ntplite_status_t;

/* Returns the ABI revision this library was built with. */
int ntplite_c_api_version(void);

/* Human readable library version, e.g. "0.1.0".  Never NULL. */
const char* ntplite_version_string(void);

/* Individual version components. */
int ntplite_version_major(void);
int ntplite_version_minor(void);
int ntplite_version_patch(void);

/* Human readable description of a status code.  Never NULL. */
const char* ntplite_status_string(ntplite_status_t status);

/* ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 * These mirror ntplite::timestamp and ntplite::duration (see
 * <ntplite/time.hpp>) field for field, which is what lets the C++ client hand
 * one to C without allocating, reformatting or losing precision.
 *
 * Both represent a broken-down time rather than a float, because a `double`
 * carries only ~15 significant decimal digits: for a modern Unix timestamp
 * (about 1.7e9 s) that is sub-microsecond resolution at best, which is the same
 * order as the measurement itself.
 *
 * In both types `nanoseconds` is *unsigned* and always in [0, 999999999], even
 * for a negative duration: -1.5 s is `{-2, 500000000}`, not `{-1, -500000000}`.
 * Keeping the sub-second field non-negative removes a whole class of sign bugs
 * from the offset arithmetic.
 */
typedef struct ntplite_timestamp {
  int64_t seconds;      /* seconds since 1970-01-01T00:00:00Z */
  uint32_t nanoseconds; /* 0 .. 999999999                     */
} ntplite_timestamp_t;

typedef struct ntplite_duration {
  int64_t seconds;      /* floor of the value                 */
  uint32_t nanoseconds; /* 0 .. 999999999                     */
} ntplite_duration_t;

/* ---------------------------------------------------------------------------
 * Query options
 * ---------------------------------------------------------------------------
 * Zero means "use the default" for every field, so `{0}` - or a static object,
 * which is already zero - is a valid set of options and
 * ntplite_options_init() is only a convenience.
 *
 * There is no way to ask for a literal zero-valued timeout: a zero budget would
 * mean "give up before trying", which is never what a caller wants.
 */
typedef struct ntplite_options {
  /* Set to sizeof(ntplite_options_t).  Zero is accepted and means "this
   * layout"; a value larger than the library knows is treated as the largest
   * layout it knows.  Provided so a future release can add fields without
   * breaking callers compiled against this header. */
  size_t struct_size;

  uint16_t port;   /* 0 -> 123                       */
  int ip_version;  /* 0 -> any, 4 -> IPv4, 6 -> IPv6 */
  int ntp_version; /* 0 -> 4; only 3 and 4 are accepted */

  int64_t server_timeout_ms; /* 0 -> 1000; budget for one server      */
  int64_t total_timeout_ms;  /* 0 -> 5000; budget for the whole query */
  int64_t retry_interval_ms; /* 0 -> 400; wait before resending       */
} ntplite_options_t;

/* ---------------------------------------------------------------------------
 * Query result
 * ---------------------------------------------------------------------------
 * This is a snapshot, not a handle: it owns no resources, is safe to copy, and
 * is safe to read after the process has done anything else.
 *
 * Fields only a usable reply can supply are zero unless `valid` is set.
 */
typedef struct ntplite_result {
  size_t struct_size; /* see ntplite_options_t */

  /* --- the four timestamps of the exchange -------------------------------- */

  ntplite_timestamp_t local_send;     /* T1: when the request left here      */
  ntplite_timestamp_t server_receive; /* T2: when the server received it     */
  ntplite_timestamp_t server_send;    /* T3: when the server answered        */
  ntplite_timestamp_t local_receive;  /* T4: when the answer arrived here    */

  /* The server's clock as of "now": T4 plus `offset`.  This is the number a
   * caller wants when it asked "what time is it" - it is *not* the local
   * clock, which ntplite never modifies. */
  ntplite_timestamp_t server_time;

  /* --- the estimates ------------------------------------------------------ */

  /* Add to the local clock to get the server's time.  Positive means the local
   * clock is slow.  Nothing has been applied to it. */
  ntplite_duration_t offset;

  /* The round trip network delay, as RFC 5905 computes it.  The uncertainty of
   * `offset` is half of this. */
  ntplite_duration_t round_trip_delay;

  /* T4 minus T1 measured on the monotonic clock: the wall to wall time the
   * exchange took, including the server's own processing. */
  ntplite_duration_t round_trip_time;

  /* T3 minus T2: how long the server itself held the request. */
  ntplite_duration_t server_processing;

  /* What the server reports about its own path to its reference clock.  These
   * describe the server, not our link to it. */
  ntplite_duration_t root_delay;
  ntplite_duration_t root_dispersion;

  /* --- flags -------------------------------------------------------------- */

  int valid;              /* non-zero: a usable reply produced the fields above */
  int kiss_of_death;      /* non-zero: the server refused; see `kiss_code`      */
  int delay_is_plausible; /* non-zero: the delay fits the measured round trip   */
  int attempts;           /* datagrams put on the wire                          */

  /* --- what the server said about itself ---------------------------------- */

  int stratum;
  int leap;
  int version;
  int mode;
  int poll;
  int precision;

  /* --- text --------------------------------------------------------------- */

  char server[64];    /* the address that answered, e.g. "192.0.2.1:123"    */
  char reference[16]; /* its reference identifier, rendered as text         */
  char kiss_code[16]; /* the Kiss-o'-Death code, "" unless the server sent one */

  /* All three are NUL terminated, and are truncated rather than overflowing
   * when a value does not fit. */
} ntplite_result_t;

/* Fills `options` with the library defaults.  Call this before setting any
 * field if you want to be sure of what you are not setting. */
void ntplite_options_init(ntplite_options_t* options);

/* Fills `result` with zeroes.  This is also what a failed query leaves behind. */
void ntplite_result_init(ntplite_result_t* result);

/* Asks `host` what time it is.
 *
 * `host` may be a name ("pool.ntp.org") or a literal address ("192.0.2.1",
 * "2001:db8::1").  `options` may be NULL for the defaults.  `result` must not
 * be NULL.
 *
 * Returns NTP_LITE_OK and fills `result` when a server answered with something
 * usable.  Otherwise the status says why:
 *
 *   NTP_LITE_ERR_KOD       a server refused, and `result` still holds what it
 *                          said - which server it was, and its kiss_code;
 *   NTP_LITE_ERR_PROTOCOL  a server answered with something malformed, and
 *                          `result` again holds what it said;
 *   everything else        nothing was answered: every field describing an
 *                          answer is zeroed, and only `attempts` - which counts
 *                          requests put on the wire, not answers received - may
 *                          be non-zero.
 *
 * Retrying after NTP_LITE_ERR_KOD immediately is exactly what the server asked
 * you not to do.
 *
 * The local clock is never modified: the offset is reported, not applied.  This
 * call blocks for at most total_timeout_ms and is safe to make from several
 * threads at once, on different results. */
ntplite_status_t ntplite_query(const char* host, const ntplite_options_t* options,
                               ntplite_result_t* result);

/* ntplite_query() with the defaults: port 123, IPv4 or IPv6 as it resolves,
 * NTPv4, five seconds overall. */
ntplite_status_t ntplite_query_default(const char* host, ntplite_result_t* result);

/* ---------------------------------------------------------------------------
 * Printing a time
 * ---------------------------------------------------------------------------
 * These are here rather than left to the caller because getting them right is
 * not trivial: the calendar arithmetic behind them is exact past 2038, which a
 * formatter built on a 32 bit time_t is not, and that is precisely the range an
 * NTP client has to be able to name.
 */

/* Room for an ISO 8601 UTC timestamp and its terminator, e.g.
 * "2026-10-09T12:34:56.789012345Z". */
#define NTPLITE_TIME_TEXT_SIZE 40

/* Room for a signed second count with nine decimals and its terminator, e.g.
 * "-0.500000000". */
#define NTPLITE_SECONDS_TEXT_SIZE 40

/* Writes `time` as ISO 8601 in UTC: "2026-10-09T12:34:56.789012345Z".
 *
 * `buffer` should have NTPLITE_TIME_TEXT_SIZE bytes.  A shorter one is filled
 * as far as it goes and always NUL terminated; a NULL buffer, or a size of
 * zero, writes nothing.  Never fails in any other way. */
void ntplite_format_utc(const ntplite_timestamp_t* time, char* buffer, size_t size);

/* Writes `value` as a signed second count with nine decimals: "+0.001234567".
 *
 * The sign is always present, because for an offset the sign is the point: it
 * says which way the local clock is wrong.  Same buffer rules as
 * ntplite_format_utc(). */
void ntplite_format_seconds(const ntplite_duration_t* value, char* buffer, size_t size);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ===========================================================================
 * Optional implementation block (option B above)
 * ===========================================================================
 */
#if defined(NTP_LITE_IMPLEMENTATION)

#if !defined(__cplusplus)
#error \
    "NTP_LITE_IMPLEMENTATION requires a C++ compiler: ntplite is implemented in C++11. Use option A (link ntplite::c) from C instead."
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ntplite/detail/civil_time.hpp>
#include <ntplite/detail/config.hpp>
#include <ntplite/ntplite.hpp>
#include <string>

extern "C" {

int ntplite_c_api_version(void) {
  return NTP_LITE_C_API_VERSION;
}

const char* ntplite_version_string(void) {
  return ntplite::version_string();
}

int ntplite_version_major(void) {
  return ntplite::version_major();
}

int ntplite_version_minor(void) {
  return ntplite::version_minor();
}

int ntplite_version_patch(void) {
  return ntplite::version_patch();
}

const char* ntplite_status_string(ntplite_status_t status) {
  switch (status) {
    case NTP_LITE_OK:
      return "OK";
    case NTP_LITE_ERR_INVALID:
      return "invalid argument";
    case NTP_LITE_ERR_NETWORK:
      return "network error";
    case NTP_LITE_ERR_RESOLVE:
      return "hostname resolution failed";
    case NTP_LITE_ERR_TIMEOUT:
      return "operation timed out";
    case NTP_LITE_ERR_PROTOCOL:
      return "protocol error";
    case NTP_LITE_ERR_KOD:
      return "server sent a Kiss-o'-Death packet";
    case NTP_LITE_ERR_UNSUPPORTED:
      return "unsupported platform or configuration";
    case NTP_LITE_ERR_INTERNAL:
      return "internal error";
  }
  return "unknown status";
}

} /* extern "C" */

// ---------------------------------------------------------------------------
// The client
// ---------------------------------------------------------------------------
// The C face is a translation layer and nothing more.  It converts arguments,
// calls the C++ client, converts the answer back, and owns exactly one piece of
// policy of its own: zero means "use the default".  Everything that can be
// rejected is rejected by ntplite::query(), so the two faces cannot disagree
// about what is valid.
//
// The helpers live outside the extern "C" block on purpose: a language linkage
// specification applies to names with external linkage, so putting an unnamed
// namespace inside it would be asking for a warning at best.

namespace {

// The two C time types are declared field for field identical to their C++
// counterparts.  Asserting the sizes here turns a future divergence into a
// compile error rather than a silently misread offset.
static_assert(sizeof(ntplite_timestamp_t) == sizeof(ntplite::timestamp),
              "ntplite_timestamp_t must stay the same size as ntplite::timestamp");
static_assert(sizeof(ntplite_duration_t) == sizeof(ntplite::duration),
              "ntplite_duration_t must stay the same size as ntplite::duration");

ntplite_timestamp_t timestamp_to_c(const ntplite::timestamp& value) {
  ntplite_timestamp_t out;
  out.seconds = value.seconds;
  out.nanoseconds = value.nanoseconds;
  return out;
}

ntplite_duration_t duration_to_c(const ntplite::duration& value) {
  ntplite_duration_t out;
  out.seconds = value.seconds;
  out.nanoseconds = value.nanoseconds;
  return out;
}

// Copies text into a fixed buffer, truncating instead of overflowing.  A short
// description is worth more to a caller than a corrupted stack.
void copy_text(char* destination, std::size_t capacity, const std::string& source) {
  if (capacity == 0) {
    return;
  }
  const std::size_t room = capacity - 1;
  const std::size_t length = source.size() < room ? source.size() : room;
  std::memcpy(destination, source.data(), length);
  destination[length] = '\0';
}

// Built from the C++ defaults, so the two faces cannot drift apart.
ntplite_options_t default_options() {
  const ntplite::query_options source;
  ntplite_options_t options;
  std::memset(&options, 0, sizeof(options));
  options.struct_size = sizeof(ntplite_options_t);
  options.port = source.port;
  options.ip_version = static_cast<int>(source.ip);
  options.ntp_version = source.version;
  options.server_timeout_ms = source.server_timeout_ms;
  options.total_timeout_ms = source.total_timeout_ms;
  options.retry_interval_ms = source.retry_interval_ms;
  return options;
}

// Translates the C options into the C++ ones.  A zero field keeps the C++
// default; a non-zero field is passed through even when it is nonsense, because
// ntplite::query() is the one place that decides what is valid.
void options_from_c(const ntplite_options_t* options, ntplite::query_options& out) {
  out = ntplite::query_options();
  if (options == NULL) {
    return;
  }

  if (options->port != 0) {
    out.port = options->port;
  }
  if (options->ip_version != 0) {
    out.ip = static_cast<ntplite::ip_version>(options->ip_version);
  }

  // Only the two protocol versions that exist are converted: the packet's
  // version field is a byte, so a blind cast could turn 259 into a valid-looking
  // 3.  Anything else becomes 0, which the client rejects.
  if (options->ntp_version == 3) {
    out.version = ntplite::detail::version_3;
  } else if (options->ntp_version == 4) {
    out.version = ntplite::detail::version_4;
  } else if (options->ntp_version != 0) {
    out.version = 0;
  }

  if (options->server_timeout_ms != 0) {
    out.server_timeout_ms = options->server_timeout_ms;
  }
  if (options->total_timeout_ms != 0) {
    out.total_timeout_ms = options->total_timeout_ms;
  }
  if (options->retry_interval_ms != 0) {
    out.retry_interval_ms = options->retry_interval_ms;
  }
}

ntplite_result_t result_from_cxx(const ntplite::query_result& source) {
  ntplite_result_t out;
  std::memset(&out, 0, sizeof(out));
  out.struct_size = sizeof(ntplite_result_t);

  copy_text(out.server, sizeof(out.server), source.server);
  copy_text(out.reference, sizeof(out.reference), source.reference);
  copy_text(out.kiss_code, sizeof(out.kiss_code), source.kiss_code);

  out.local_send = timestamp_to_c(source.local_send);
  out.server_receive = timestamp_to_c(source.server_receive);
  out.server_send = timestamp_to_c(source.server_send);
  out.local_receive = timestamp_to_c(source.local_receive);
  out.server_time = timestamp_to_c(source.server_time);

  out.offset = duration_to_c(source.offset);
  out.round_trip_delay = duration_to_c(source.round_trip_delay);
  out.round_trip_time = duration_to_c(source.round_trip_time);
  out.server_processing = duration_to_c(source.server_processing);
  out.root_delay = duration_to_c(source.root_delay);
  out.root_dispersion = duration_to_c(source.root_dispersion);

  out.valid = source.valid ? 1 : 0;
  out.kiss_of_death = source.kiss_of_death ? 1 : 0;
  out.delay_is_plausible = source.delay_is_plausible ? 1 : 0;
  out.attempts = source.attempts;

  out.stratum = source.stratum;
  out.leap = source.leap;
  out.version = source.version;
  out.mode = source.mode;
  out.poll = source.poll;
  out.precision = source.precision;

  return out;
}

// Writes as much of `source` as the caller's struct can hold.
//
// A caller compiled against a shorter ntplite_result_t must not have its stack
// written past the end of it, and one compiled against a longer struct must not
// be handed our garbage in the tail.  struct_size is the caller's own sizeof,
// so the prefix it describes is exactly what the caller can read.
void copy_result_out(ntplite_result_t* destination, const ntplite_result_t& source) {
  const std::size_t reported = destination->struct_size;
  std::size_t length = reported;
  if (length == 0 || length > sizeof(ntplite_result_t)) {
    length = sizeof(ntplite_result_t);
  }

  std::memcpy(destination, &source, length);
  // struct_size is the first field, so it was inside the prefix that was just
  // overwritten.  Put the caller's own value back rather than replacing it with
  // the size of the struct that this library was compiled with.
  destination->struct_size = reported;
}

}  // namespace

extern "C" {

void ntplite_options_init(ntplite_options_t* options) {
  if (options == NULL) {
    return;
  }
  *options = default_options();
}

void ntplite_result_init(ntplite_result_t* result) {
  if (result == NULL) {
    return;
  }
  std::memset(result, 0, sizeof(ntplite_result_t));
}

ntplite_status_t ntplite_query(const char* host, const ntplite_options_t* options,
                               ntplite_result_t* result) {
  if (result == NULL) {
    return NTP_LITE_ERR_INVALID;
  }

  ntplite::query_options converted;
  options_from_c(options, converted);

  ntplite::query_result answer;
  const ntplite_status_t status = ntplite::to_status(ntplite::query(host, answer, converted));

  // Even a failure is copied out: when a server refused, or answered nonsense,
  // ntplite::query() leaves the packets it saw in `answer`, and that is the only
  // description of the problem the caller can get.  For every other failure
  // `answer` is zeroed, and the result should be too.
  const ntplite_result_t filled = result_from_cxx(answer);
  copy_result_out(result, filled);
  return status;
}

ntplite_status_t ntplite_query_default(const char* host, ntplite_result_t* result) {
  const ntplite_options_t options = default_options();
  return ntplite_query(host, &options, result);
}

void ntplite_format_utc(const ntplite_timestamp_t* time, char* buffer, size_t size) {
  if (time == NULL) {
    return;
  }
  ntplite::timestamp converted;
  converted.seconds = time->seconds;
  converted.nanoseconds = time->nanoseconds;
  ntplite::detail::format_utc(converted, buffer, size);
}

void ntplite_format_seconds(const ntplite_duration_t* value, char* buffer, size_t size) {
  if (value == NULL) {
    return;
  }
  ntplite::duration converted;
  converted.seconds = value->seconds;
  converted.nanoseconds = value->nanoseconds;
  ntplite::detail::format_signed_seconds(converted, buffer, size);
}

} /* extern "C" */

#endif /* NTP_LITE_IMPLEMENTATION */

#endif /* NTP_LITE_NTP_LITE_H */
