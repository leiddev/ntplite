// ============================================================================
// ntplite - detail/exchange.hpp
// ----------------------------------------------------------------------------
// The request/reply exchange: how a client actually talks to a server.
//
// This layer sits between the packet codec and the estimator, and implements
// the three steps RFC 5905 section 8 calls the on-wire protocol:
//
//   1. send a client request whose transmit timestamp is T1, read from the wall
//      clock as late as possible before the datagram leaves;
//   2. wait for a datagram that is really the answer to that request - from the
//      server that was asked, carrying our T1 in its origin field - and stamp
//      it with T4 the moment it is read;
//   3. hand T1..T4 to estimate_clock() for the offset and the round trip delay.
//
// Everything here blocks.  The library is meant to be driven from a worker
// thread or a background task, and a blocking implementation with an explicit
// deadline is far easier to reason about than an event driven one - a clock
// step, for instance, can never corrupt a timeout, because every wait is
// measured against the monotonic clock.  A caller that needs an event loop can
// drive the two halves (send_request and await_reply) itself.
//
// Retries: RFC 5905 asks a client to repeat a request when no reply arrives and
// to give up on an unreachable server rather than hammering it.  Each endpoint
// therefore gets a budget (settings.timeout_ms) inside which the request may be
// repeated up to max_attempts_per_endpoint times, waiting
// settings.retry_interval_ms for each answer.  The final attempt is allowed to
// use whatever is left of the budget, so a quick retry does not shorten the
// total wait.
// ============================================================================

#ifndef NTP_LITE_DETAIL_EXCHANGE_HPP
#define NTP_LITE_DETAIL_EXCHANGE_HPP

#include <cstddef>
#include <cstdint>
#include <ntplite/detail/clock.hpp>
#include <ntplite/detail/config.hpp>
#include <ntplite/detail/error.hpp>
#include <ntplite/detail/packet.hpp>
#include <ntplite/detail/socket.hpp>
#include <ntplite/time.hpp>
#include <vector>

namespace ntplite {
namespace detail {

/// How many datagrams one endpoint is asked before it is given up on.
///
/// Three is the count a libntp client uses: enough to ride out one lost
/// datagram in either direction, few enough that a black hole cannot keep a
/// caller waiting for the full budget multiplied by a large number.
const int max_attempts_per_endpoint = 3;

/// Room for any NTP datagram, including extension fields and a MAC.
///
/// The header is all we decode, but reading into a buffer larger than the
/// header means an oversized reply is truncated instead of turning into a
/// receive error, which on Windows would be WSAEMSGSIZE.
const std::size_t max_datagram_size = 512;

/// The address family an endpoint belongs to.
inline address_family family_of(const endpoint& value) {
  return value.is_ipv6() ? address_family::ipv6 : address_family::ipv4;
}

/// What one exchange with one server needs to know.
struct exchange_settings {
  exchange_settings()
      : timeout_ms(1000),
        retry_interval_ms(400),
        version(version_4),
        poll(default_poll),
        precision(default_precision) {}

  std::int64_t timeout_ms;         ///< budget for this endpoint, in milliseconds
  std::int64_t retry_interval_ms;  ///< wait for an answer before repeating, in milliseconds
  std::uint8_t version;            ///< protocol version to speak
  std::int8_t poll;                ///< advertised poll interval, log2 seconds
  std::int8_t precision;           ///< advertised local clock precision, log2 seconds
};

/// The record of one exchange.
///
/// The two packets are zeroed on construction so that a caller which looks at
/// the result after an error can never read uninitialised memory.
struct exchange_result {
  exchange_result()
      : server(),
        request(empty_packet()),
        reply(empty_packet()),
        local_send(),
        server_receive(),
        server_send(),
        local_receive(),
        sample(),
        attempts(0) {}

  endpoint server;           ///< who was asked - and where the answer came from
  packet request;            ///< the request that was answered
  packet reply;              ///< the answer itself
  timestamp local_send;      ///< T1, when the request left
  timestamp server_receive;  ///< T2, when the server received it
  timestamp server_send;     ///< T3, when the server sent its answer
  timestamp local_receive;   ///< T4, when the answer arrived
  clock_sample sample;       ///< offset and delay derived from T1..T4
  int attempts;              ///< datagrams sent for this endpoint
};

/// Sends one client request to `server` and records T1 in `sent_at`.
///
/// The wall clock is read as late as possible before the datagram is handed to
/// the kernel and the request's transmit timestamp is set from it, because that
/// value is what the server echoes back and what makes the round trip
/// measurable.  A request the server cannot place in time is worthless.
///
/// The socket must already be open and connected to `server`, so that the reply
/// arrives with the peer's address already checked by the kernel.
inline error_code send_request(udp_socket& socket, const endpoint& server,
                               const exchange_settings& settings, packet& request,
                               clock_reading& sent_at) {
  if (!socket.is_open() || !server.valid()) {
    return error_code::invalid_argument;
  }
  if (settings.version != version_3 && settings.version != version_4) {
    return error_code::invalid_argument;
  }

  sent_at = read_clocks();
  request =
      make_client_request(unix_to_ntp_timestamp(sent_at.wall.seconds, sent_at.wall.nanoseconds),
                          settings.version, settings.poll, settings.precision);

  std::uint8_t buffer[packet_size];
  encode_packet(request, buffer);

  error_code ec = error_code::ok;
  if (!socket.send_to(server, buffer, sizeof(buffer), ec)) {
    return ec;
  }
  return error_code::ok;
}

/// Waits for a datagram that is an answer to `request` from `server`.
///
/// On success `reply` holds the datagram and `received_at` is the clock reading
/// taken immediately after it was read - that is T4, and it must not be delayed
/// by any of the checking that follows.
///
/// Datagrams that are not the answer are discarded and the wait continues,
/// rather than failing the exchange: a stray packet on a busy host, a reply to
/// a request we sent a moment ago, or an off-path attacker's guess must not be
/// able to stop a legitimate query.  The two tests are the source address and
/// the origin timestamp echo, which together are what ties a reply to a request.
///
/// Returns error_code::timeout when the budget runs out, and some other code
/// only when the wait or the read itself failed.
inline error_code await_reply(udp_socket& socket, const endpoint& server, const packet& request,
                              std::int64_t timeout_ms, packet& reply, clock_reading& received_at) {
  if (!socket.is_open() || !server.valid()) {
    return error_code::invalid_argument;
  }
  if (timeout_ms <= 0) {
    return error_code::invalid_argument;
  }

  const deadline limit = deadline::after(timeout_ms);
  std::uint8_t buffer[max_datagram_size];

  for (;;) {
    if (limit.remaining_milliseconds() == 0) {
      return error_code::timeout;
    }

    error_code ec = error_code::ok;
    const wait_result ready = wait_readable(socket.handle(), limit.wait_timeout_ms(), ec);
    if (ready == wait_result::failed) {
      return ec;
    }
    if (ready == wait_result::timed_out) {
      return error_code::timeout;
    }

    std::size_t received = 0;
    endpoint from;
    if (!socket.receive_from(buffer, sizeof(buffer), received, from, ec)) {
      // select() reported readable but there was nothing to read - an ICMP
      // error surfaced by a connected socket, for instance.  That is a reason
      // to keep waiting, not to fail.
      if (ec == error_code::timeout) {
        continue;
      }
      return ec;
    }

    // T4: the moment the datagram was read, before any of the checks below.
    received_at = read_clocks();

    if (from != server) {
      continue;
    }
    if (received < packet_size) {
      continue;
    }

    packet candidate;
    if (decode_packet(buffer, received, candidate) != NTP_LITE_OK) {
      continue;
    }
    if (!reply_matches_request(candidate, request)) {
      continue;
    }

    reply = candidate;
    return error_code::ok;
  }
}

/// Turns a matched request/reply pair, plus the two clock readings taken around
/// it, into the timestamps and the estimates of an exchange.
///
/// Separate from the exchange loop on purpose: this is the part that has to be
/// exactly right, and keeping it a pure function of its inputs means it can be
/// checked with hand written timestamps instead of a network.
///
/// T2 and T3 are decoded against the local clock, because a 32 bit seconds
/// field alone is ambiguous by 136 years - see decode_timestamp().  The round
/// trip comes from the monotonic clock, not from T4 - T1, so a wall clock that
/// steps while the request is in flight cannot masquerade as network delay.
inline void measure(const packet& request, const packet& reply, const clock_reading& sent_at,
                    const clock_reading& received_at, exchange_result& out) {
  out.request = request;
  out.reply = reply;
  out.local_send = sent_at.wall;
  out.local_receive = received_at.wall;

  const std::int64_t reference_seconds = sent_at.wall.seconds;
  const unix_parts server_receive = ntp_timestamp_to_unix(reply.receive, reference_seconds);
  const unix_parts server_send = ntp_timestamp_to_unix(reply.transmit, reference_seconds);
  out.server_receive = make_timestamp(server_receive.seconds, server_receive.nanoseconds);
  out.server_send = make_timestamp(server_send.seconds, server_send.nanoseconds);

  clock_sample_input input;
  input.local_send = out.local_send;
  input.server_receive = out.server_receive;
  input.server_send = out.server_send;
  input.local_receive = out.local_receive;
  input.round_trip_nanoseconds = elapsed_nanoseconds(sent_at, received_at);
  out.sample = estimate_clock(input);
}

/// Exchanges with one endpoint, retrying within the configured budget.
///
/// A reply that identifies itself as ours but is not usable - a Kiss-o'-Death,
/// or a packet that fails validation - ends the exchange immediately with that
/// reason, because asking again would only earn the same answer.  Only silence
/// is retried.
///
/// On any outcome other than a plain timeout, `out` describes the exchange:
/// which server answered, what it sent, and the timestamps.  That is what lets
/// a caller report the Kiss-o'-Death code a server used to turn it away.
inline error_code exchange_with_server(udp_socket& socket, const endpoint& server,
                                       const exchange_settings& settings, exchange_result& out) {
  if (!socket.is_open() || !server.valid()) {
    return error_code::invalid_argument;
  }
  if (settings.timeout_ms <= 0) {
    return error_code::invalid_argument;
  }

  out.attempts = 0;
  out.server = server;

  const deadline limit = deadline::after(settings.timeout_ms);

  std::int64_t attempt_budget = settings.retry_interval_ms;
  if (attempt_budget <= 0 || attempt_budget > settings.timeout_ms) {
    attempt_budget = settings.timeout_ms;
  }

  for (int attempt = 0; attempt < max_attempts_per_endpoint; ++attempt) {
    packet request;
    clock_reading sent_at;
    const error_code send_error = send_request(socket, server, settings, request, sent_at);
    if (send_error != error_code::ok) {
      return send_error;
    }
    out.attempts += 1;

    const std::int64_t remaining = limit.remaining_milliseconds();
    if (remaining <= 0) {
      break;
    }

    // The last attempt inherits the rest of the budget: there is no point in
    // coming back later if this was the final question we were allowed to ask.
    const bool last_attempt = (attempt + 1 >= max_attempts_per_endpoint);
    std::int64_t wait_ms =
        (last_attempt || attempt_budget > remaining) ? remaining : attempt_budget;
    if (wait_ms <= 0) {
      break;
    }

    packet reply;
    clock_reading received_at;
    const error_code reply_error =
        await_reply(socket, server, request, wait_ms, reply, received_at);
    if (reply_error == error_code::timeout) {
      continue;
    }
    if (reply_error != error_code::ok) {
      return reply_error;
    }

    measure(request, reply, sent_at, received_at, out);
    return from_status(validate_reply(reply, request));
  }

  return error_code::timeout;
}

/// Walks a list of endpoints until one of them answers.
///
/// `total_timeout_ms` bounds the whole walk, so a name with many addresses
/// cannot hold a caller for longer than it asked for.  Each endpoint gets at
/// most `settings.timeout_ms`, shortened when the overall budget is nearly
/// spent.
///
/// A server that answers with something unusable ends the walk: that answer is
/// information about the server, not a reason to ask a different one.
inline error_code exchange(const std::vector<endpoint>& servers, const exchange_settings& settings,
                           std::int64_t total_timeout_ms, exchange_result& out,
                           int& datagrams_sent) {
  datagrams_sent = 0;

  if (servers.empty()) {
    return error_code::invalid_argument;
  }
  if (settings.timeout_ms <= 0 || total_timeout_ms <= 0) {
    return error_code::invalid_argument;
  }

  const deadline total = deadline::after(total_timeout_ms);
  error_code last_error = error_code::timeout;

  for (std::size_t index = 0; index < servers.size(); ++index) {
    const endpoint& server = servers[index];

    const std::int64_t remaining = total.remaining_milliseconds();
    if (remaining <= 0) {
      return last_error;
    }

    error_code ec = error_code::ok;
    udp_socket socket;
    if (!socket.open(family_of(server), ec)) {
      last_error = ec;
      continue;
    }
    if (!socket.connect(server, ec)) {
      last_error = ec;
      continue;
    }

    exchange_settings per_server = settings;
    if (per_server.timeout_ms > remaining) {
      per_server.timeout_ms = remaining;
    }
    if (per_server.timeout_ms <= 0) {
      return last_error;
    }

    exchange_result attempt;
    const error_code exchange_error = exchange_with_server(socket, server, per_server, attempt);
    datagrams_sent += attempt.attempts;

    if (exchange_error == error_code::ok) {
      out = attempt;
      out.attempts = datagrams_sent;
      return error_code::ok;
    }

    last_error = exchange_error;

    if (exchange_error == error_code::protocol_error ||
        exchange_error == error_code::kiss_of_death) {
      // The server answered; its answer is what the caller needs to see, so the
      // walk stops here instead of shopping around for a friendlier server.
      out = attempt;
      out.attempts = datagrams_sent;
      return exchange_error;
    }

    if (exchange_error != error_code::timeout) {
      // A local failure - a socket that cannot be opened, a send that failed.
      // No reply was received, so there is nothing worth reporting.
      return exchange_error;
    }
  }

  return last_error;
}

}  // namespace detail
}  // namespace ntplite

#endif  // NTP_LITE_DETAIL_EXCHANGE_HPP
