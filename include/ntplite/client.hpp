// ============================================================================
// ntplite - client.hpp
// ----------------------------------------------------------------------------
// The C++ client: one call, one answer.
//
//   ntplite::query_result result;
//   const ntplite::error_code error = ntplite::query("pool.ntp.org", result);
//   if (error == ntplite::error_code::ok) {
//     std::cout << "server time is " << ntplite::timestamp_seconds(result.server_time)
//               << " +/- " << ntplite::duration_seconds(result.round_trip_delay) / 2
//               << " s\n";
//   }
//
// A query never touches the local clock.  It measures the offset between the
// local clock and a server's clock and hands the numbers back; what to do with
// them - log them, adjust a monotonic correction, hand them to an application -
// is the caller's decision.  That is deliberate: a library that silently steps
// the system clock is a library nobody can safely link against.
//
// Everything is header-only and blocking.  Call it from a worker thread, or use
// detail::send_request() and detail::await_reply() to drive the exchange from
// an event loop of your own.
// ============================================================================

#ifndef NTP_LITE_CLIENT_HPP
#define NTP_LITE_CLIENT_HPP

#include <cstdint>
#include <ntplite/detail/exchange.hpp>
#include <ntplite/time.hpp>
#include <string>
#include <vector>

namespace ntplite {

/// The port every public NTP server listens on.
const std::uint16_t default_ntp_port = 123;

/// Which address family a query may use.
///
/// The default is to take whichever the resolver offers first, which on a dual
/// stack host means IPv6.  Forcing a family is useful on a network where one of
/// the two is filtered but the resolver does not know it.
enum class ip_version {
  any = 0,   ///< whatever the resolver prefers
  ipv4 = 4,  ///< A records only
  ipv6 = 6   ///< AAAA records only
};

/// How a query should behave, and how patient it should be.
struct query_options {
  query_options()
      : port(default_ntp_port),
        ip(ip_version::any),
        version(detail::version_4),
        server_timeout_ms(1000),
        total_timeout_ms(5000),
        retry_interval_ms(400) {}

  std::uint16_t port;              ///< UDP port to send to
  ip_version ip;                   ///< address family to allow
  std::uint8_t version;            ///< NTP version to speak, 3 or 4
  std::int64_t server_timeout_ms;  ///< budget for one server, in milliseconds
  std::int64_t total_timeout_ms;   ///< budget for the whole query, in milliseconds
  std::int64_t retry_interval_ms;  ///< wait for one answer before resending, in milliseconds
};

/// The outcome of a query.
///
/// Every field is filled in even when the query failed, so a caller that got a
/// Kiss-o'-Death can print the code, and a caller that timed out can still see
/// how many datagrams went out.  Fields that only a usable reply can supply are
/// left zero, and `valid` says which case you are in.
struct query_result {
  query_result()
      : server(),
        reference(),
        kiss_code(),
        local_send(),
        server_receive(),
        server_send(),
        local_receive(),
        server_time(),
        offset(),
        round_trip_delay(),
        round_trip_time(),
        server_processing(),
        root_delay(),
        root_dispersion(),
        stratum(0),
        leap(0),
        version(0),
        mode(0),
        poll(0),
        precision(0),
        kiss_of_death(false),
        delay_is_plausible(false),
        valid(false),
        attempts(0) {}

  // --- who answered, and with what -----------------------------------------

  std::string server;     ///< "192.0.2.1:123" - the server that answered
  std::string reference;  ///< what that server is synchronised to
  std::string kiss_code;  ///< "RATE", "DENY", ... - empty unless a Kiss-o'-Death arrived

  // --- the four timestamps -------------------------------------------------

  timestamp local_send;      ///< T1, when the request left
  timestamp server_receive;  ///< T2, when the server received it
  timestamp server_send;     ///< T3, when the server sent its answer
  timestamp local_receive;   ///< T4, when the answer arrived
  timestamp server_time;     ///< the estimate of "now": T4 plus the offset

  // --- the estimates -------------------------------------------------------

  /// How much the local clock is behind the server.  Add it to the local clock
  /// to get the server's time; a positive value means the local clock is slow.
  duration offset;

  /// The round trip network delay between local and server, as RFC 5905
  /// computes it.  The uncertainty of `offset` is half of this.
  duration round_trip_delay;

  /// T4 minus T1, measured on the monotonic clock - the wall to wall time the
  /// exchange took, including the server's own processing.
  duration round_trip_time;

  /// T3 minus T2, the time the server spent handling the request.
  duration server_processing;

  /// The server's reported total round trip delay to its own reference clock.
  duration root_delay;

  /// The server's reported total dispersion, which is its own error bound.
  duration root_dispersion;

  // --- what the reply said -------------------------------------------------

  std::uint8_t stratum;   ///< 1..15 for a usable server, 0 for a Kiss-o'-Death
  std::uint8_t leap;      ///< leap indicator, 0..3
  std::uint8_t version;   ///< protocol version the server answered with
  std::uint8_t mode;      ///< answer mode, 4 (server) or 5 (broadcast)
  std::int8_t poll;       ///< the server's poll interval, log2 seconds
  std::int8_t precision;  ///< the server's clock precision, log2 seconds

  // --- the verdict ---------------------------------------------------------

  bool kiss_of_death;       ///< the server used a Kiss-o'-Death to turn us away
  bool delay_is_plausible;  ///< false when the delay came out negative, i.e. discard
  bool valid;               ///< a usable reply was received; the estimates mean something
  int attempts;             ///< datagrams sent while getting this answer

  /// The offset in seconds, for a caller that wants a number rather than a
  /// struct.  Positive means the local clock is behind.
  double offset_seconds() const { return duration_seconds(offset); }

  /// The round trip delay in seconds.
  double round_trip_delay_seconds() const { return duration_seconds(round_trip_delay); }
};

namespace detail {

/// Translates the public family choice into the socket layer's.
inline address_family to_address_family(ip_version value) {
  switch (value) {
    case ip_version::ipv4:
      return address_family::ipv4;
    case ip_version::ipv6:
      return address_family::ipv6;
    case ip_version::any:
      break;
  }
  return address_family::any;
}

/// The server's reported root delay or dispersion as a duration.
///
/// The field is 16.16 fixed point.  RFC 5905 declares it signed, but a negative
/// root delay is not something a real server produces, and the codec reads it
/// as unsigned, so this simply widens what the codec produced.
inline duration reported_duration(const ntp_short& value) {
  return duration_from_nanoseconds(static_cast<std::int64_t>(ntp_short_to_nanoseconds(value)));
}

/// Copies an exchange into the public result type.
inline void fill_result(const exchange_result& exchange, bool usable, query_result& out) {
  out = query_result();

  out.server = exchange.server.to_string();
  out.local_send = exchange.local_send;
  out.server_receive = exchange.server_receive;
  out.server_send = exchange.server_send;
  out.local_receive = exchange.local_receive;

  out.offset = exchange.sample.offset;
  out.round_trip_delay = exchange.sample.round_trip_delay;
  out.round_trip_time = exchange.sample.round_trip_time;
  out.server_processing = exchange.sample.server_processing;
  out.delay_is_plausible = exchange.sample.delay_is_plausible;
  out.attempts = exchange.attempts;
  out.valid = usable;

  const packet& reply = exchange.reply;
  out.stratum = reply.stratum;
  out.leap = reply.leap;
  out.version = reply.version;
  out.mode = reply.mode;
  out.poll = reply.poll;
  out.precision = reply.precision;
  out.root_delay = reported_duration(reply.root_delay);
  out.root_dispersion = reported_duration(reply.root_dispersion);
  out.kiss_of_death = is_kiss_of_death(reply);

  char text[16];
  format_reference_id(reply.stratum, reply.reference_id, text, sizeof(text));
  out.reference = text;

  if (out.kiss_of_death) {
    const char* name = kiss_code_name(reply.reference_id);
    // An unrecognised code is still four octets of ASCII worth showing.
    out.kiss_code = (name[0] != '\0') ? std::string(name) : out.reference;
  }

  if (usable) {
    out.server_time = add_duration(out.local_receive, out.offset);
  }
}

}  // namespace detail

/// Queries `host` over UDP and reports the clocks.
///
/// `host` may be a name, an IPv4 literal or an IPv6 literal.  Every address the
/// resolver returns is tried in turn, because "the first address of a
/// round-robin name is down" is the most common way a query fails; the budget
/// for the whole walk is `options.total_timeout_ms`.
///
/// Returns:
///   * error_code::ok - a reply was received; `out.valid` is true and the
///     estimates in `out` mean something;
///   * error_code::kiss_of_death - the server refused, and `out.kiss_code`
///     says why ("RATE", "DENY", "RSTR", ...).  Retrying immediately is exactly
///     what the server asked you not to do;
///   * error_code::timeout - nobody answered within the budget;
///   * error_code::resolve_error - the name did not resolve;
///   * error_code::protocol_error - the answer was malformed;
///   * error_code::network_error / invalid_argument - a local failure.
///
/// Nothing here changes the local clock.
inline error_code query(const char* host, query_result& out, const query_options& options) {
  // Clear the result first, before anything can fail.  A caller that reuses one
  // result object for a series of queries must never be able to read a field
  // left over from the previous answer, and an argument that was rejected
  // before any IO is no different from one that failed later.
  out = query_result();

  if (host == NULL || host[0] == '\0') {
    return error_code::invalid_argument;
  }
  if (options.port == 0) {
    return error_code::invalid_argument;
  }
  if (options.version != detail::version_3 && options.version != detail::version_4) {
    return error_code::invalid_argument;
  }
  if (options.server_timeout_ms <= 0 || options.total_timeout_ms <= 0 ||
      options.retry_interval_ms < 0) {
    return error_code::invalid_argument;
  }

  error_code ec = error_code::ok;
  std::vector<detail::endpoint> servers;
  if (!detail::resolve(host, options.port, detail::to_address_family(options.ip), servers, ec)) {
    detail::fill_result(detail::exchange_result(), false, out);
    return ec;
  }

  detail::exchange_settings settings;
  settings.timeout_ms = options.server_timeout_ms;
  settings.retry_interval_ms = options.retry_interval_ms;
  settings.version = options.version;

  detail::exchange_result exchange;
  int datagrams_sent = 0;
  const error_code result =
      detail::exchange(servers, settings, options.total_timeout_ms, exchange, datagrams_sent);

  detail::fill_result(exchange, result == error_code::ok, out);
  // exchange() only copies its result when a server actually answered, so on a
  // timeout the count has to come from here: how many datagrams went out is the
  // first thing anyone debugging a query wants to know.
  out.attempts = datagrams_sent;
  return result;
}

/// Queries `host` with the default options.
inline error_code query(const char* host, query_result& out) {
  return query(host, out, query_options());
}

/// Queries `host` over UDP and reports the clocks.
inline error_code query(const std::string& host, query_result& out, const query_options& options) {
  return query(host.c_str(), out, options);
}

/// Queries `host` with the default options.
inline error_code query(const std::string& host, query_result& out) {
  return query(host.c_str(), out, query_options());
}

}  // namespace ntplite

#endif  // NTP_LITE_CLIENT_HPP
