// ============================================================================
// ntplite - tests/mock_ntp_server.hpp
// ----------------------------------------------------------------------------
// A real NTP server, in miniature, on loopback: a bound UDP socket and a thread,
// answering the wire protocol well enough that the client cannot tell the
// difference between it and the real thing.
//
// The unit tests drive the layers below `query()` directly, with a hand-built
// packet and a socket the test owns.  That is precise but it never runs the
// whole path at once: name resolution, the request, the retry loop, the reply
// binding, the measurement and the result, in the order a real query happens.
// This server is what lets a test do that, and it is the only way to test the
// failures that only exist end to end - a refused request, a malformed answer, a
// server that answers on the second attempt, a duplicate that arrives twice.
//
// It is a test double and lives with the tests.  It is built on the library's
// own packet codec, which is a real weakness: a codec that is wrong in the same
// way on both sides of the wire would go unnoticed here.  That is what the
// Python cross-validation script is for - it builds and parses packets by hand,
// from the RFC, and is the one check that shares no code with the thing it is
// checking.
// ============================================================================

#ifndef NTPLITE_TEST_MOCK_NTP_SERVER_HPP
#define NTPLITE_TEST_MOCK_NTP_SERVER_HPP

#include <atomic>
#include <cstdint>
#include <ntplite/detail/clock.hpp>
#include <ntplite/detail/error.hpp>
#include <ntplite/detail/exchange.hpp>
#include <ntplite/detail/ntp_time.hpp>
#include <ntplite/detail/packet.hpp>
#include <ntplite/detail/socket.hpp>
#include <thread>

namespace ntplite_test {

using ::ntplite::detail::address_family;
using ::ntplite::detail::clock_reading;
using ::ntplite::detail::endpoint;
// Not `::ntplite::detail::error_code`: the taxonomy lives directly in `ntplite`,
// and only its *values* mirror the C enumeration.
using ::ntplite::error_code;
using ::ntplite::detail::max_datagram_size;
using ::ntplite::detail::packet;
using ::ntplite::detail::packet_size;
using ::ntplite::detail::udp_socket;
using ::ntplite::detail::wait_readable;
using ::ntplite::detail::wait_result;

/// The ways a server can misbehave that are worth testing against.
///
/// Anything else - a refused request, a wrong version, a wrong mode, an empty
/// stratum - is just a field set differently in `server_behaviour`, which is
/// simpler than a case per refusal.
enum class reply_kind {
  answer,     ///< a normal, usable answer
  silence,    ///< no reply at all, so the client retries and then times out
  truncated,  ///< eight bytes on the wire instead of the whole packet
  garbage,    ///< a full sized datagram of noise
  duplicate   ///< the answer sent twice, as a retransmitting network would
};

struct server_behaviour {
  server_behaviour()
      : kind(reply_kind::answer),
        clock_offset_nanoseconds(0),
        hold_nanoseconds(0),
        stratum(2),
        reference_id(0xC0000201U) /* 192.0.2.1 */,
        version(::ntplite::detail::version_4),
        mode(::ntplite::detail::mode::server),
        leap(::ntplite::detail::leap::no_warning),
        answer_from_attempt(1) {}

  reply_kind kind;

  /// How far this server's clock is from the local one.  Positive means the
  /// server is ahead, so a correct client reports a positive offset.
  std::int64_t clock_offset_nanoseconds;

  /// How long the server pretends to hold the request before answering.
  std::int64_t hold_nanoseconds;

  /// Copied into the reply as they stand, so a test can set any of them to
  /// something a well behaved server would not send.
  std::uint8_t stratum;
  std::uint32_t reference_id;
  std::uint8_t version;
  std::uint8_t mode;
  std::uint8_t leap;

  /// Stay silent until this attempt: 1 answers the first request, 2 lets the
  /// first go by and answers the retry.
  int answer_from_attempt;
};

/// A server on loopback, answering on a thread of its own.
///
/// One server per test case: the behaviour is fixed at start() and never
/// synchronised afterwards, which is why it can be read from the worker thread
/// without a lock.
class mock_server {
 public:
  mock_server()
      : address_(),
        behaviour_(),
        worker_(),
        requests_seen_(0),
        replies_sent_(0),
        stop_requested_(false) {}

  ~mock_server() { stop(); }

  // Copying would give two objects one thread between them.
  mock_server(const mock_server&) = delete;
  mock_server& operator=(const mock_server&) = delete;

  /// Binds a loopback port and starts answering.  Returns false, with `ec` set,
  /// if the socket could not be opened or bound.
  bool start(const server_behaviour& behaviour, error_code& ec) {
    stop();

    behaviour_ = behaviour;
    requests_seen_.store(0);
    replies_sent_.store(0);
    stop_requested_.store(false);

    if (!socket_.open(address_family::ipv4, ec)) {
      return false;
    }
    if (!socket_.bind(endpoint::loopback(address_family::ipv4, 0), ec)) {
      socket_.close();
      return false;
    }
    if (!socket_.local_endpoint(address_, ec)) {
      socket_.close();
      return false;
    }

    worker_ = std::thread(&mock_server::run, this);
    return true;
  }

  /// Stops the thread and closes the socket.  Safe to call more than once, and
  /// called by the destructor.
  void stop() {
    stop_requested_.store(true);
    if (worker_.joinable()) {
      worker_.join();
    }
    socket_.close();
  }

  /// The address a client should be pointed at.
  const endpoint& address() const { return address_; }

  /// How many datagrams arrived, whether they were answered or not.
  int requests_seen() const { return requests_seen_.load(); }

  /// How many datagrams were sent back.
  int replies_sent() const { return replies_sent_.load(); }

 private:
  /// How long the worker waits for a datagram before going round the loop.
  /// Short, because this timeout is the only way the thread notices that it has
  /// been asked to stop: a blocking read would need the socket to be closed
  /// under it, which is exactly what a cross-thread close must not do.
  static int poll_milliseconds() { return 10; }

  void run() {
    while (!stop_requested_.load()) {
      error_code ec = error_code::ok;
      const wait_result ready = wait_readable(socket_.handle(), poll_milliseconds(), ec);

      // A timeout is the normal path: it is how the loop checks the flag.  A
      // failure is left to the next iteration rather than ending the server,
      // because a spurious one would otherwise look like a silent server and
      // turn into a mystifying test failure somewhere else.
      if (ready != wait_result::ready) {
        continue;
      }

      std::uint8_t buffer[max_datagram_size];
      endpoint from;
      std::size_t received = 0;
      if (!socket_.receive_from(buffer, sizeof(buffer), received, from, ec) ||
          received < packet_size) {
        continue;
      }

      packet request = ::ntplite::detail::empty_packet();
      if (::ntplite::detail::decode_packet(buffer, received, request) != NTP_LITE_OK) {
        continue;
      }

      const int attempt = requests_seen_.fetch_add(1) + 1;
      if (behaviour_.kind == reply_kind::silence || attempt < behaviour_.answer_from_attempt) {
        continue;
      }

      answer(request, from);
    }
  }

  void answer(const packet& request, const endpoint& from) {
    // The server's clock, as this server sees it: read the real clock and shift
    // it.  Reading the clock rather than reusing the request's timestamps is
    // what keeps this end to end - the four timestamps really do cross the wire
    // and really are the times the two sides observed.
    const clock_reading now = ::ntplite::detail::read_clocks();
    const ::ntplite::timestamp received_at = ::ntplite::add_duration(
        now.wall, ::ntplite::make_duration(0, behaviour_.clock_offset_nanoseconds));
    const ::ntplite::timestamp answered_at = ::ntplite::add_duration(
        received_at, ::ntplite::make_duration(0, behaviour_.hold_nanoseconds));

    packet reply = ::ntplite::detail::empty_packet();
    reply.leap = behaviour_.leap;
    reply.version = behaviour_.version;
    reply.mode = behaviour_.mode;
    reply.stratum = behaviour_.stratum;
    reply.poll = ::ntplite::detail::default_poll;
    reply.precision = ::ntplite::detail::default_precision;
    reply.root_delay = ::ntplite::detail::nanoseconds_to_ntp_short(1000000ULL);      /* 1 ms */
    reply.root_dispersion = ::ntplite::detail::nanoseconds_to_ntp_short(2000000ULL); /* 2 ms */
    reply.reference_id = behaviour_.reference_id;
    // The reference timestamp says when this server last set its clock, so it is
    // deliberately older than the exchange: a client that confused it with a
    // wire timestamp would show it up here.
    reply.reference =
        ::ntplite::detail::unix_to_ntp_timestamp(received_at.seconds - 60, received_at.nanoseconds);
    reply.origin = request.transmit;
    reply.receive =
        ::ntplite::detail::unix_to_ntp_timestamp(received_at.seconds, received_at.nanoseconds);
    reply.transmit =
        ::ntplite::detail::unix_to_ntp_timestamp(answered_at.seconds, answered_at.nanoseconds);

    std::uint8_t encoded[packet_size];
    ::ntplite::detail::encode_packet(reply, encoded);

    if (behaviour_.kind == reply_kind::garbage) {
      for (std::size_t i = 0; i < sizeof(encoded); ++i) {
        encoded[i] = static_cast<std::uint8_t>(0xA5U ^ (i * 31U));
      }
      send(from, encoded, sizeof(encoded));
      return;
    }

    if (behaviour_.kind == reply_kind::truncated) {
      send(from, encoded, 8);
      return;
    }

    send(from, encoded, sizeof(encoded));
    if (behaviour_.kind == reply_kind::duplicate) {
      send(from, encoded, sizeof(encoded));
    }
  }

  void send(const endpoint& to, const std::uint8_t* data, std::size_t size) {
    error_code ec = error_code::ok;
    if (socket_.send_to(to, data, size, ec)) {
      replies_sent_.fetch_add(1);
    }
  }

  udp_socket socket_;
  endpoint address_;
  server_behaviour behaviour_;
  std::thread worker_;
  std::atomic<int> requests_seen_;
  std::atomic<int> replies_sent_;
  std::atomic<bool> stop_requested_;
};

/// An address for the client to resolve, pointing at the mock.  A name rather
/// than a literal, so the end-to-end tests exercise resolution too; "localhost"
/// is in the hosts file on every platform, so this needs no DNS.
const char* const mock_server_host = "localhost";

}  // namespace ntplite_test

#endif  // NTPLITE_TEST_MOCK_NTP_SERVER_HPP
