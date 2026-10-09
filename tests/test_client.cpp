// ============================================================================
// ntplite - tests/test_client.cpp
// ----------------------------------------------------------------------------
// The client: rendering, validation, measurement, and the datagram exchange.
//
// The exchange is split into send_request(), await_reply() and measure() rather
// than being one blocking call, and this file is the reason why.  A test can
// send a real request over a real loopback socket, read it on the far side,
// build the answer a server would have sent, queue it, and only then call the
// waiting half - so the whole path runs for real, with no threads, no sleeps
// and no timing assumptions.  What is left for the end-to-end tests in a later
// milestone is the one thing that needs a background thread: a server that
// answers while the client is blocked.
//
// The three "discard" tests below are the other half of that argument.  A reply
// is only accepted if it comes from the server that was asked *and* echoes the
// transmit timestamp of the request it answers.  Both conditions are tested
// negatively, because accepting a stray datagram is a security bug: an off-path
// attacker who can guess a request gets to choose the time you believe.
// ============================================================================

#include <cstdint>
#include <ntplite/client.hpp>
#include <ntplite/detail/exchange.hpp>
#include <ntplite/detail/packet.hpp>
#include <ntplite/detail/socket.hpp>
#include <ntplite/time.hpp>
#include <string>

#include "ntplite_test.hpp"
#include "ntplite_test_support.hpp"

namespace {

using ::ntplite::detail::clock_reading;
using ::ntplite::detail::endpoint;
using ::ntplite::detail::packet;
using ::ntplite::detail::udp_socket;
// `error_code` lives one level up: the taxonomy is shared between the C API and
// the C++ implementation, so it is `ntplite::error_code`, not a detail type.
using ::ntplite::error_code;
using ::ntplite::duration;
using ::ntplite::timestamp;

/// The instant the fake servers use as their era reference.  Any value inside
/// the era being tested works; this one is 2026-01-01T00:00:00Z.
const std::int64_t kEraReference = 1767225600;

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// A loopback socket that stands in for a server's, bound to an ephemeral port
/// so that two of them can exist at once and parallel test runs cannot collide.
struct loopback_peer {
  udp_socket socket;
  endpoint address;
};

inline bool open_peer(loopback_peer& peer, error_code& ec) {
  if (!peer.socket.open(::ntplite::detail::address_family::ipv4, ec)) {
    return false;
  }
  if (!peer.socket.bind(endpoint::loopback(::ntplite::detail::address_family::ipv4, 0), ec)) {
    return false;
  }
  return peer.socket.local_endpoint(peer.address, ec) && peer.address.valid();
}

/// A bound client socket, left unconnected so that the tests can also send from
/// a second address and check that the exchange is not fooled by it.
///
/// Bound to loopback rather than to the wildcard address: a peer can only answer
/// a datagram whose source address it can send back to, and the local address of
/// a socket bound to 0.0.0.0 is 0.0.0.0, which is not a destination.
inline bool open_bound_client(udp_socket& socket, error_code& ec) {
  if (!socket.open(::ntplite::detail::address_family::ipv4, ec)) {
    return false;
  }
  return socket.bind(endpoint::loopback(::ntplite::detail::address_family::ipv4, 0), ec);
}

/// The address a bound client is sending from, which is what a server sees as
/// the source of a request and therefore where its answer has to go.  A failed
/// lookup yields an invalid endpoint rather than throwing, matching the rest of
/// the detail layer; a test that depends on it would fail either way.
inline endpoint client_local_address(udp_socket& socket) {
  error_code ec = error_code::ok;
  endpoint address;
  if (!socket.local_endpoint(address, ec)) {
    return endpoint();
  }
  return address;
}

inline std::uint16_t client_local_port(udp_socket& socket) {
  return client_local_address(socket).port();
}

/// How many datagrams are queued on a peer, without waiting for any.
inline int drain(loopback_peer& peer) {
  error_code ec = error_code::ok;
  if (!peer.socket.set_nonblocking(true, ec)) {
    return -1;
  }
  int count = 0;
  std::uint8_t buffer[::ntplite::detail::max_datagram_size];
  for (;;) {
    std::size_t received = 0;
    endpoint from;
    if (!peer.socket.receive_from(buffer, sizeof(buffer), received, from, ec)) {
      break;
    }
    ++count;
  }
  return count;
}

/// Reads one datagram and decodes it, reporting whether that worked.
inline bool receive_packet(udp_socket& socket, packet& out, endpoint& from) {
  error_code ec = error_code::ok;
  std::uint8_t buffer[::ntplite::detail::max_datagram_size];
  std::size_t received = 0;
  if (!socket.receive_from(buffer, sizeof(buffer), received, from, ec)) {
    return false;
  }
  if (received < ::ntplite::detail::packet_size) {
    return false;
  }
  return ::ntplite::detail::decode_packet(buffer, received, out) == NTP_LITE_OK;
}

inline void send_packet(udp_socket& socket, const endpoint& to, const packet& value) {
  error_code ec = error_code::ok;
  std::uint8_t buffer[::ntplite::detail::packet_size];
  ::ntplite::detail::encode_packet(value, buffer);
  NTP_TEST_CHECK(socket.send_to(to, buffer, sizeof(buffer), ec));
}

/// The answer a real server would send to `request`.
///
/// Its clock is `skew_seconds` ahead of ours; it sees the request 10 ms after it
/// was sent and holds it for `processing_nanoseconds` before answering.  A
/// simulated server has to leave the answer exactly as long as the real round
/// trip took, otherwise `round_trip_delay = elapsed - server_processing` comes
/// out negative - the real path over loopback is far quicker than any processing
/// time worth inventing.  Tests that care about that arithmetic pass 0.
inline packet make_reply(const packet& request, std::int64_t skew_seconds, std::uint8_t stratum,
                         std::uint32_t reference_id,
                         std::int64_t processing_nanoseconds = 5000000LL) {
  packet reply = ::ntplite::detail::empty_packet();
  reply.leap = ::ntplite::detail::leap::no_warning;
  reply.version = ::ntplite::detail::version_4;
  reply.mode = ::ntplite::detail::mode::server;
  reply.stratum = stratum;
  reply.poll = 3;
  reply.precision = ::ntplite::detail::default_precision;
  reply.reference_id = reference_id;
  reply.origin = request.transmit;

  const ::ntplite::detail::unix_parts sent =
      ::ntplite::detail::ntp_timestamp_to_unix(request.transmit, kEraReference);
  const timestamp arrived = ::ntplite::make_timestamp(
      sent.seconds + skew_seconds, static_cast<std::int64_t>(sent.nanoseconds) + 10000000LL);
  const timestamp answered = ::ntplite::make_timestamp(
      arrived.seconds, static_cast<std::int64_t>(arrived.nanoseconds) + processing_nanoseconds);

  reply.receive = ::ntplite::detail::unix_to_ntp_timestamp(arrived.seconds, arrived.nanoseconds);
  reply.transmit = ::ntplite::detail::unix_to_ntp_timestamp(answered.seconds, answered.nanoseconds);
  reply.reference = reply.transmit;
  return reply;
}

/// A clock reading with a hand-picked wall clock and monotonic instant.
inline clock_reading reading_at(std::int64_t unix_seconds, std::int64_t unix_nanoseconds,
                                std::int64_t monotonic_milliseconds) {
  clock_reading value;
  value.wall = ::ntplite::make_timestamp(unix_seconds, unix_nanoseconds);
  value.monotonic = std::chrono::steady_clock::time_point(
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::milliseconds(monotonic_milliseconds)));
  return value;
}

inline void set_short_fixed(::ntplite::detail::ntp_short& value, std::uint16_t seconds,
                            std::uint16_t fraction) {
  value.seconds = seconds;
  value.fraction = fraction;
}

/// True when a nanosecond count read back off the wire is the expected one.
///
/// The 32.32 fraction has a resolution of about 233 ps, and a whole nanosecond
/// is not a multiple of it, so encoding an instant and decoding it again can
/// land one nanosecond away from where it started.  The round trip is still
/// exact whenever the instant happens to be representable, which is why the
/// wire format tests assert a tolerance of one nanosecond rather than a rule.
inline bool within_one_nanosecond(std::uint32_t actual, std::int64_t expected) {
  const std::int64_t value = static_cast<std::int64_t>(actual);
  return value >= expected - 1 && value <= expected + 1;
}

/// A client request whose transmit timestamp is a chosen instant.
inline packet request_at(std::int64_t unix_seconds, std::uint32_t nanoseconds) {
  return ::ntplite::detail::make_client_request(
      ::ntplite::detail::unix_to_ntp_timestamp(unix_seconds, nanoseconds),
      ::ntplite::detail::version_4, ::ntplite::detail::default_poll,
      ::ntplite::detail::default_precision);
}

}  // namespace

// ---------------------------------------------------------------------------
// Rendering the reference identifier
// ---------------------------------------------------------------------------

NTP_TEST(reference_id, a_primary_server_shows_the_name_of_its_clock_source) {
  char text[16];
  ::ntplite::detail::format_reference_id(1, 0x47505300U, text, sizeof(text));  // "GPS\0"
  NTP_TEST_CHECK_STREQ(text, "GPS");

  ::ntplite::detail::format_reference_id(1, 0x50505300U, text, sizeof(text));  // "PPS\0"
  NTP_TEST_CHECK_STREQ(text, "PPS");
}

NTP_TEST(reference_id, a_kiss_of_death_shows_its_four_letter_code) {
  char text[16];
  ::ntplite::detail::format_reference_id(0, 0x52415445U, text, sizeof(text));  // "RATE"
  NTP_TEST_CHECK_STREQ(text, "RATE");
}

NTP_TEST(reference_id, a_secondary_server_shows_the_address_of_its_own_source) {
  char text[16];

  ::ntplite::detail::format_reference_id(2, 0xC0000201U, text, sizeof(text));  // 192.0.2.1
  NTP_TEST_CHECK_STREQ(text, "192.0.2.1");

  ::ntplite::detail::format_reference_id(3, 0x00000000U, text, sizeof(text));
  NTP_TEST_CHECK_STREQ(text, "0.0.0.0");

  ::ntplite::detail::format_reference_id(2, 0xFFFFFFFFU, text, sizeof(text));
  NTP_TEST_CHECK_STREQ(text, "255.255.255.255");

  // The octets are most significant first, and a byte such as 0x0A must print
  // as "10" rather than "A" or "010".
  ::ntplite::detail::format_reference_id(2, 0x0A0B0C0DU, text, sizeof(text));
  NTP_TEST_CHECK_STREQ(text, "10.11.12.13");
}

NTP_TEST(reference_id, a_capacity_too_small_yields_nothing_rather_than_half_an_address) {
  char text[16];

  // Exactly enough for the four characters and the terminator.
  ::ntplite::detail::format_reference_id(1, 0x47505300U, text, 5);
  NTP_TEST_CHECK_STREQ(text, "GPS");

  ::ntplite::detail::format_reference_id(1, 0x47505300U, text, 4);
  NTP_TEST_CHECK_STREQ(text, "");

  // "255.255.255.255" needs 16 bytes; 15 must produce nothing.
  ::ntplite::detail::format_reference_id(2, 0xFFFFFFFFU, text, 16);
  NTP_TEST_CHECK_STREQ(text, "255.255.255.255");
  ::ntplite::detail::format_reference_id(2, 0xFFFFFFFFU, text, 15);
  NTP_TEST_CHECK_STREQ(text, "");

  // A null buffer or a zero capacity must not be written to at all.
  ::ntplite::detail::format_reference_id(2, 0xC0000201U, NULL, 16);
  text[0] = 'x';
  ::ntplite::detail::format_reference_id(2, 0xC0000201U, text, 0);
  NTP_TEST_CHECK_EQ(text[0], 'x');
}

// ---------------------------------------------------------------------------
// Binding a reply to a request
// ---------------------------------------------------------------------------

NTP_TEST(validate, a_reply_is_only_accepted_when_it_echoes_our_transmit_timestamp) {
  const packet request = request_at(100, 0);
  const packet reply = make_reply(request, 1000, 2, 0xC0000201U);

  NTP_TEST_CHECK(::ntplite::detail::reply_matches_request(reply, request));

  // One nanosecond of the fraction is enough to disqualify it: the origin field
  // is the whole of the binding, and "close enough" is not a thing.
  packet almost = reply;
  almost.origin.fraction += 1U;
  NTP_TEST_CHECK(!::ntplite::detail::reply_matches_request(almost, request));

  packet other_second = reply;
  other_second.origin.seconds += 1U;
  NTP_TEST_CHECK(!::ntplite::detail::reply_matches_request(other_second, request));

  const packet unrelated = request_at(101, 0);
  NTP_TEST_CHECK(!::ntplite::detail::reply_matches_request(reply, unrelated));
}

NTP_TEST(validate, a_well_formed_reply_is_accepted) {
  const packet request = request_at(100, 0);
  const packet reply = make_reply(request, 1000, 2, 0xC0000201U);

  NTP_TEST_CHECK_EQ(::ntplite::detail::validate_reply(reply, request), NTP_LITE_OK);
}

NTP_TEST(validate, a_kiss_of_death_is_reported_as_such_and_not_as_a_protocol_error) {
  const packet request = request_at(100, 0);
  const packet reply = make_reply(request, 1000, 0, 0x52415445U);  // stratum 0, "RATE"

  NTP_TEST_CHECK_EQ(::ntplite::detail::validate_reply(reply, request), NTP_LITE_ERR_KOD);
  NTP_TEST_CHECK(::ntplite::detail::is_kiss_of_death(reply));
  NTP_TEST_CHECK_STREQ(::ntplite::detail::kiss_code_name(reply.reference_id), "RATE");
}

NTP_TEST(validate, a_malformed_reply_is_rejected) {
  const packet request = request_at(100, 0);

  // A version we do not speak.
  packet old = make_reply(request, 0, 2, 0);
  old.version = 2;
  NTP_TEST_CHECK_EQ(::ntplite::detail::validate_reply(old, request), NTP_LITE_ERR_PROTOCOL);

  // Our own request echoed back at us: mode 3 is not an answer.
  packet echoed = make_reply(request, 0, 2, 0);
  echoed.mode = ::ntplite::detail::mode::client;
  NTP_TEST_CHECK_EQ(::ntplite::detail::validate_reply(echoed, request), NTP_LITE_ERR_PROTOCOL);

  // A stratum of 16 is reserved and means "not synchronised".
  packet reserved = make_reply(request, 0, 16, 0);
  NTP_TEST_CHECK_EQ(::ntplite::detail::validate_reply(reserved, request), NTP_LITE_ERR_PROTOCOL);

  // The server says its own clock is not set.
  packet unsynchronised = make_reply(request, 0, 2, 0);
  unsynchronised.leap = ::ntplite::detail::leap::unsynchronized;
  NTP_TEST_CHECK_EQ(::ntplite::detail::validate_reply(unsynchronised, request),
                    NTP_LITE_ERR_PROTOCOL);

  // A zero transmit timestamp would make the delay meaningless.
  packet timeless = make_reply(request, 0, 2, 0);
  timeless.transmit = ::ntplite::detail::ntp_timestamp();
  NTP_TEST_CHECK_EQ(::ntplite::detail::validate_reply(timeless, request), NTP_LITE_ERR_PROTOCOL);

  // An origin that is not ours.
  packet stray = make_reply(request, 0, 2, 0);
  stray.origin.fraction += 1U;
  NTP_TEST_CHECK_EQ(::ntplite::detail::validate_reply(stray, request), NTP_LITE_ERR_PROTOCOL);
}

// ---------------------------------------------------------------------------
// Measuring
// ---------------------------------------------------------------------------

NTP_TEST(measure, the_four_timestamps_produce_the_offset_and_the_delay) {
  // A hand-built exchange with round numbers: the server's clock is 1000 s
  // ahead, its answer takes 10 ms to reach us and 5 ms to produce, and the
  // monotonic clock says 25 ms passed end to end.
  //
  //   T1 = 100.000       T2 = 1100.010
  //   T4 = 100.025       T3 = 1100.015
  //
  //   theta = ((1100.010 - 100.000) + (1100.015 - 100.025)) / 2 = 1000.000 s
  //   delta = 25 ms - 5 ms                                     = 20 ms
  //
  // T1 and T4 come from the local clock and are exact; T2 and T3 come back
  // through the 32.32 wire format, so they are asserted to the nearest
  // nanosecond.  The offset still comes out exact because the two decoding
  // errors cancel: +10 ms decodes one nanosecond short, +15 ms decodes one
  // nanosecond short, and each is subtracted from a value that is exact.
  const packet request = request_at(100, 0);
  packet reply = make_reply(request, 1000, 2, 0xC0000201U);
  set_short_fixed(reply.root_delay, 1, 0x8000U);       // 1.5 s
  set_short_fixed(reply.root_dispersion, 0, 0x0100U);  // 3906250 ns

  const clock_reading sent_at = reading_at(100, 0, 0);
  const clock_reading received_at = reading_at(100, 25000000, 25);

  ::ntplite::detail::exchange_result result;
  ::ntplite::detail::measure(request, reply, sent_at, received_at, result);

  NTP_TEST_CHECK_EQ(result.local_send.seconds, 100LL);
  NTP_TEST_CHECK_EQ(result.local_send.nanoseconds, 0U);
  NTP_TEST_CHECK_EQ(result.local_receive.seconds, 100LL);
  NTP_TEST_CHECK_EQ(result.local_receive.nanoseconds, 25000000U);

  NTP_TEST_CHECK_EQ(result.server_receive.seconds, 1100LL);
  NTP_TEST_CHECK(within_one_nanosecond(result.server_receive.nanoseconds, 10000000LL));
  NTP_TEST_CHECK_EQ(result.server_send.seconds, 1100LL);
  NTP_TEST_CHECK(within_one_nanosecond(result.server_send.nanoseconds, 15000000LL));

  NTP_TEST_CHECK_EQ(result.sample.offset.seconds, 1000LL);
  NTP_TEST_CHECK_EQ(result.sample.offset.nanoseconds, 0U);
  NTP_TEST_CHECK_EQ(result.sample.round_trip_delay.seconds, 0LL);
  NTP_TEST_CHECK(within_one_nanosecond(result.sample.round_trip_delay.nanoseconds, 20000000LL));
  NTP_TEST_CHECK(within_one_nanosecond(result.sample.server_processing.nanoseconds, 5000000LL));
  NTP_TEST_CHECK_EQ(result.sample.round_trip_time.nanoseconds, 25000000U);
  NTP_TEST_CHECK(result.sample.delay_is_plausible);
}

NTP_TEST(measure, t2_and_t3_are_placed_in_the_era_of_the_local_clock) {
  // The server's answer carries 2208989900 s, which is 1100 s after 1970 but is
  // also, read naively, an instant in 2040.  The local clock decides.
  const packet request = request_at(100, 0);
  const packet reply = make_reply(request, 1000, 2, 0);

  const clock_reading sent_at = reading_at(100, 0, 0);
  const clock_reading received_at = reading_at(100, 25000000, 25);

  ::ntplite::detail::exchange_result result;
  ::ntplite::detail::measure(request, reply, sent_at, received_at, result);

  NTP_TEST_CHECK_EQ(result.server_receive.seconds, 1100LL);
  NTP_TEST_CHECK_EQ(result.sample.offset.seconds, 1000LL);
}

// ---------------------------------------------------------------------------
// The exchange, over a real loopback socket
// ---------------------------------------------------------------------------

NTP_TEST(wire, a_request_and_its_answer_round_trip_over_loopback) {
  error_code ec = error_code::ok;
  loopback_peer server;
  NTP_TEST_REQUIRE(open_peer(server, ec));

  udp_socket client;
  NTP_TEST_REQUIRE(open_bound_client(client, ec));

  ::ntplite::detail::exchange_settings settings;
  settings.timeout_ms = 2000;
  settings.retry_interval_ms = 2000;

  // The client half: send, then the far side reads a real datagram off a real
  // socket and checks it looks like a client request.
  packet request;
  clock_reading sent_at;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::send_request(client, server.address, settings, request,
                                                       sent_at),
                       error_code::ok);
  NTP_TEST_CHECK_EQ(static_cast<int>(request.mode),
                    static_cast<int>(::ntplite::detail::mode::client));
  NTP_TEST_CHECK_EQ(request.version, ::ntplite::detail::version_4);
  NTP_TEST_CHECK_EQ(request.stratum, 0U);
  NTP_TEST_CHECK_EQ(
      ::ntplite::detail::ntp_timestamp_to_unix(request.transmit, kEraReference).seconds,
      sent_at.wall.seconds);

  packet on_the_wire;
  endpoint from;
  NTP_TEST_REQUIRE(receive_packet(server.socket, on_the_wire, from));
  // The far side must see exactly the request we sent.  reply_matches_request()
  // is about answers - it compares the reply's origin field with the request's
  // transmit field - and a request carries zero in origin, so compare the
  // transmit field, which is the one the answer will echo.
  NTP_TEST_CHECK_EQ(on_the_wire.transmit.seconds, request.transmit.seconds);
  NTP_TEST_CHECK_EQ(on_the_wire.transmit.fraction, request.transmit.fraction);
  NTP_TEST_CHECK_EQ(from.port(), client_local_port(client));

  // The server half: build the answer, queue it, and only then let the client
  // wait for it.  Ordering it this way is what makes the whole test
  // deterministic without a thread.  No processing time is claimed, so the
  // measured round trip delay is the real time the loopback path took.
  const packet reply = make_reply(on_the_wire, 1000, 2, 0xC0000201U, 0);
  send_packet(server.socket, from, reply);

  packet received;
  clock_reading received_at;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::await_reply(client, server.address, request, 2000,
                                                      received, received_at),
                       error_code::ok);
  NTP_TEST_CHECK_EQ(::ntplite::detail::validate_reply(received, request), NTP_LITE_OK);

  ::ntplite::detail::exchange_result result;
  ::ntplite::detail::measure(request, received, sent_at, received_at, result);

  // The offset is dominated by the 1000 s skew; the residual is the real time
  // this test spent between the two halves, so only a generous bound is fair.
  const double offset = ::ntplite::duration_seconds(result.sample.offset);
  NTP_TEST_CHECK_NEAR(offset, 1000.0, 0.5);
  const double delay = ::ntplite::duration_seconds(result.sample.round_trip_delay);
  NTP_TEST_CHECK(delay > 0.0);
  NTP_TEST_CHECK(delay < 0.5);
  NTP_TEST_CHECK(result.sample.delay_is_plausible);
}

NTP_TEST(wire, a_reply_that_does_not_echo_our_request_is_discarded) {
  error_code ec = error_code::ok;
  loopback_peer server;
  NTP_TEST_REQUIRE(open_peer(server, ec));

  udp_socket client;
  NTP_TEST_REQUIRE(open_bound_client(client, ec));

  const packet request = request_at(100, 0);
  packet stale = make_reply(request, 0, 2, 0);
  stale.origin.fraction += 1U;  // a reply to some earlier request
  send_packet(server.socket, client_local_address(client), stale);

  packet received;
  clock_reading received_at;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::await_reply(client, server.address, request, 150,
                                                      received, received_at),
                       error_code::timeout);
}

NTP_TEST(wire, a_reply_from_another_address_is_discarded) {
  error_code ec = error_code::ok;
  loopback_peer server;
  loopback_peer stranger;
  NTP_TEST_REQUIRE(open_peer(server, ec));
  NTP_TEST_REQUIRE(open_peer(stranger, ec));

  udp_socket client;
  NTP_TEST_REQUIRE(open_bound_client(client, ec));

  const packet request = request_at(100, 0);
  const packet reply = make_reply(request, 0, 2, 0);

  // A perfectly well formed answer, echoing our timestamp, sent by somebody who
  // is not the server we asked.  It must not be accepted.
  send_packet(stranger.socket, client_local_address(client), reply);

  packet received;
  clock_reading received_at;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::await_reply(client, server.address, request, 150,
                                                      received, received_at),
                       error_code::timeout);
}

NTP_TEST(wire, a_runt_datagram_is_discarded) {
  error_code ec = error_code::ok;
  loopback_peer server;
  NTP_TEST_REQUIRE(open_peer(server, ec));

  udp_socket client;
  NTP_TEST_REQUIRE(open_bound_client(client, ec));

  const packet request = request_at(100, 0);
  const std::uint8_t runt[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  NTP_TEST_CHECK(server.socket.send_to(client_local_address(client), runt, sizeof(runt), ec));

  packet received;
  clock_reading received_at;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::await_reply(client, server.address, request, 150,
                                                      received, received_at),
                       error_code::timeout);
}

NTP_TEST(wire, sending_on_a_closed_socket_is_an_argument_error) {
  udp_socket closed;
  ::ntplite::detail::exchange_settings settings;

  packet request;
  clock_reading sent_at;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::send_request(closed, endpoint(), settings, request,
                                                       sent_at),
                       error_code::invalid_argument);
}

NTP_TEST(wire, waiting_on_a_closed_socket_is_an_argument_error) {
  udp_socket closed;
  packet request = request_at(100, 0);
  packet received;
  clock_reading received_at;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::await_reply(closed, endpoint(), request, 100, received,
                                                      received_at),
                       error_code::invalid_argument);
}

// ---------------------------------------------------------------------------
// Retries and giving up
// ---------------------------------------------------------------------------

NTP_TEST(exchange, a_silent_server_is_retried_and_then_given_up_on) {
  error_code ec = error_code::ok;
  loopback_peer server;                     // bound, so no ICMP port-unreachable is generated,
  NTP_TEST_REQUIRE(open_peer(server, ec));  // but nothing ever reads from it

  udp_socket client;
  NTP_TEST_REQUIRE(open_bound_client(client, ec));

  ::ntplite::detail::exchange_settings settings;
  settings.timeout_ms = 200;
  settings.retry_interval_ms = 50;

  ::ntplite::detail::exchange_result result;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::exchange_with_server(client, server.address, settings,
                                                               result),
                       error_code::timeout);

  NTP_TEST_CHECK_EQ(result.attempts, ::ntplite::detail::max_attempts_per_endpoint);
  NTP_TEST_CHECK_EQ(drain(server), ::ntplite::detail::max_attempts_per_endpoint);
}

NTP_TEST(exchange, a_zero_budget_is_an_argument_error) {
  error_code ec = error_code::ok;
  loopback_peer server;
  NTP_TEST_REQUIRE(open_peer(server, ec));

  udp_socket client;
  NTP_TEST_REQUIRE(open_bound_client(client, ec));

  ::ntplite::detail::exchange_settings settings;
  settings.timeout_ms = 0;

  ::ntplite::detail::exchange_result result;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::exchange_with_server(client, server.address, settings,
                                                               result),
                       error_code::invalid_argument);
}

NTP_TEST(exchange, an_empty_server_list_is_an_argument_error) {
  const std::vector<endpoint> none;
  ::ntplite::detail::exchange_settings settings;
  settings.timeout_ms = 100;

  ::ntplite::detail::exchange_result result;
  int datagrams = -1;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::exchange(none, settings, 100, result, datagrams),
                       error_code::invalid_argument);
  NTP_TEST_CHECK_EQ(datagrams, 0);
}

NTP_TEST(exchange, the_walk_moves_on_to_the_next_server_when_the_first_one_is_silent) {
  error_code ec = error_code::ok;
  loopback_peer first;
  loopback_peer second;
  NTP_TEST_REQUIRE(open_peer(first, ec));
  NTP_TEST_REQUIRE(open_peer(second, ec));

  std::vector<endpoint> servers;
  servers.push_back(first.address);
  servers.push_back(second.address);

  ::ntplite::detail::exchange_settings settings;
  settings.timeout_ms = 100;
  settings.retry_interval_ms = 50;

  ::ntplite::detail::exchange_result result;
  int datagrams = 0;
  NTP_TEST_CHECK_ERROR(::ntplite::detail::exchange(servers, settings, 200, result, datagrams),
                       error_code::timeout);

  // Both servers must have been asked: if only the first one had, the walk
  // would never fail over to a working address.
  NTP_TEST_CHECK(datagrams >= 2);
  NTP_TEST_CHECK(drain(first) >= 1);
  NTP_TEST_CHECK(drain(second) >= 1);
  NTP_TEST_CHECK_EQ(result.attempts, 0);  // nothing answered, so nothing to report
}

// ---------------------------------------------------------------------------
// The public query entry point
// ---------------------------------------------------------------------------

NTP_TEST(query, arguments_that_cannot_work_are_rejected_before_any_io) {
  ::ntplite::query_result result;

  NTP_TEST_CHECK_ERROR(::ntplite::query(static_cast<const char*>(NULL), result),
                       error_code::invalid_argument);
  NTP_TEST_CHECK_ERROR(::ntplite::query("", result), error_code::invalid_argument);

  ::ntplite::query_options options;

  options.port = 0;
  NTP_TEST_CHECK_ERROR(::ntplite::query("127.0.0.1", result, options),
                       error_code::invalid_argument);

  options = ::ntplite::query_options();
  options.version = 2;
  NTP_TEST_CHECK_ERROR(::ntplite::query("127.0.0.1", result, options),
                       error_code::invalid_argument);

  options = ::ntplite::query_options();
  options.server_timeout_ms = 0;
  NTP_TEST_CHECK_ERROR(::ntplite::query("127.0.0.1", result, options),
                       error_code::invalid_argument);

  options = ::ntplite::query_options();
  options.total_timeout_ms = 0;
  NTP_TEST_CHECK_ERROR(::ntplite::query("127.0.0.1", result, options),
                       error_code::invalid_argument);

  options = ::ntplite::query_options();
  options.retry_interval_ms = -1;
  NTP_TEST_CHECK_ERROR(::ntplite::query("127.0.0.1", result, options),
                       error_code::invalid_argument);
}

NTP_TEST(query, an_unresolvable_name_is_reported_as_a_resolve_error) {
  ::ntplite::query_options options;
  options.server_timeout_ms = 50;
  options.total_timeout_ms = 100;

  ::ntplite::query_result result;
  // ".invalid" is reserved by RFC 2606 precisely so that this can never resolve.
  NTP_TEST_CHECK_ERROR(::ntplite::query("no-such-host.invalid", result, options),
                       error_code::resolve_error);
  NTP_TEST_CHECK(!result.valid);
  NTP_TEST_CHECK_EQ(result.attempts, 0);
}

NTP_TEST(query, a_silent_server_produces_a_timeout_and_nothing_else) {
  error_code ec = error_code::ok;
  loopback_peer server;
  NTP_TEST_REQUIRE(open_peer(server, ec));

  ::ntplite::query_options options;
  options.ip = ::ntplite::ip_version::ipv4;
  options.port = server.address.port();
  options.server_timeout_ms = 100;
  options.total_timeout_ms = 150;
  options.retry_interval_ms = 40;

  ::ntplite::query_result result;
  NTP_TEST_CHECK_ERROR(::ntplite::query("127.0.0.1", result, options), error_code::timeout);

  NTP_TEST_CHECK(!result.valid);
  NTP_TEST_CHECK(result.attempts >= 1);
  NTP_TEST_CHECK(!result.kiss_of_death);
  NTP_TEST_CHECK_EQ(result.stratum, 0U);
  NTP_TEST_CHECK_EQ(result.offset.seconds, 0LL);
  NTP_TEST_CHECK_STREQ(result.server.c_str(), "");

  // The requests really did reach the port we named.
  NTP_TEST_CHECK(drain(server) >= 1);
}

NTP_TEST(query, a_result_is_zeroed_before_a_query_runs) {
  // A caller that reuses one result object must not be able to read the
  // previous answer out of it after the next query fails.
  ::ntplite::query_result result;
  result.server = "192.0.2.1:123";
  result.valid = true;
  result.attempts = 7;
  result.stratum = 2;

  ::ntplite::query_options options;
  options.server_timeout_ms = 50;
  options.total_timeout_ms = 100;
  NTP_TEST_CHECK_ERROR(::ntplite::query("no-such-host.invalid", result, options),
                       error_code::resolve_error);

  NTP_TEST_CHECK_STREQ(result.server.c_str(), "");
  NTP_TEST_CHECK(!result.valid);
  NTP_TEST_CHECK_EQ(result.attempts, 0);
  NTP_TEST_CHECK_EQ(result.stratum, 0U);

  // The same has to hold for an argument rejected before any IO: a caller that
  // passes a bad port should get a cleared result back, not the last good one.
  result.server = "192.0.2.1:123";
  result.valid = true;
  result.attempts = 7;
  result.stratum = 2;

  options = ::ntplite::query_options();
  options.port = 0;
  NTP_TEST_CHECK_ERROR(::ntplite::query("127.0.0.1", result, options),
                       error_code::invalid_argument);

  NTP_TEST_CHECK_STREQ(result.server.c_str(), "");
  NTP_TEST_CHECK(!result.valid);
  NTP_TEST_CHECK_EQ(result.attempts, 0);
  NTP_TEST_CHECK_EQ(result.stratum, 0U);
}
