// ============================================================================
// ntplite - tests/test_socket.cpp
// ----------------------------------------------------------------------------
// The UDP transport.
//
// Everything here runs offline.  Datagrams go from one loopback socket to
// another inside this process, and the timeout tests wait on a socket that
// nobody will ever write to - both are deterministic, so a failure is always a
// real defect rather than a flaky network.
//
// This translation unit is deliberately the only test file that includes the
// platform socket headers.  Everything above the transport is testable without
// them, and keeping the boundary visible in the test suite is worth a little
// duplication.
// ============================================================================

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ntplite/detail/socket.hpp>
#include <string>
#include <utility>
#include <vector>

#include "ntplite_test.hpp"
#include "ntplite_test_support.hpp"

using ntplite::detail::address_family;
using ntplite::detail::deadline;
using ntplite::detail::endpoint;
using ntplite::detail::native_socket;
using ntplite::detail::udp_socket;
using ntplite::detail::wait_result;
using ntplite::error_code;

namespace {

/// The platform's "nothing was available yet" code.
int would_block_code() {
#if NTP_LITE_PLATFORM_WINDOWS
  return WSAEWOULDBLOCK;
#else
  return EAGAIN;
#endif
}

/// The platform's "cut short by a signal" code.
int interrupted_code() {
#if NTP_LITE_PLATFORM_WINDOWS
  return WSAEINTR;
#else
  return EINTR;
#endif
}

/// The platform's "the operation timed out" code.
int timed_out_code() {
#if NTP_LITE_PLATFORM_WINDOWS
  return WSAETIMEDOUT;
#else
  return ETIMEDOUT;
#endif
}

/// Opens a socket, binds it to an ephemeral loopback port, and reports the
/// address the kernel chose.  Port 0 means "pick one for me", which is what
/// keeps these tests from colliding with a real ntpd on the machine.
bool make_bound_loopback_socket(address_family family, udp_socket& socket, endpoint& local) {
  error_code ec = error_code::ok;
  if (!socket.open(family, ec)) {
    return false;
  }
  if (!socket.bind(endpoint::loopback(family, 0), ec)) {
    return false;
  }
  return socket.local_endpoint(local, ec);
}

/// Milliseconds since `start`, for the timing assertions.
std::int64_t elapsed_ms(const std::chrono::steady_clock::time_point& start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
}

}  // namespace

// ---------------------------------------------------------------------------
// Error translation
// ---------------------------------------------------------------------------

NTP_TEST(error, zero_is_not_an_error) {
  NTP_TEST_CHECK_ERROR(ntplite::detail::classify_socket_error(0), error_code::ok);
}

NTP_TEST(error, nothing_available_is_a_timeout) {
  // A non-blocking receive with an empty queue is not a failure, and the
  // client should keep waiting until its deadline.  "Timeout" is exactly that.
  NTP_TEST_CHECK(ntplite::detail::is_would_block_error(would_block_code()));
  NTP_TEST_CHECK_ERROR(ntplite::detail::classify_socket_error(would_block_code()),
                       error_code::timeout);
}

NTP_TEST(error, interrupted_calls_are_a_timeout) {
  NTP_TEST_CHECK(ntplite::detail::is_interrupted_error(interrupted_code()));
  NTP_TEST_CHECK_ERROR(ntplite::detail::classify_socket_error(interrupted_code()),
                       error_code::timeout);
}

NTP_TEST(error, a_real_timeout_is_a_timeout) {
  NTP_TEST_CHECK_ERROR(ntplite::detail::classify_socket_error(timed_out_code()),
                       error_code::timeout);
}

NTP_TEST(error, anything_unrecognised_is_a_network_error) {
  // 12345 is above every errno in use and above the Winsock block, so it can
  // never be one of the codes handled above.
  NTP_TEST_CHECK_ERROR(ntplite::detail::classify_socket_error(12345), error_code::network_error);
}

// ---------------------------------------------------------------------------
// endpoint
// ---------------------------------------------------------------------------

NTP_TEST(endpoint, default_construction_is_invalid) {
  const endpoint empty;
  NTP_TEST_CHECK(!empty.valid());
  NTP_TEST_CHECK(!empty.is_ipv6());
  NTP_TEST_CHECK_EQ(empty.family(), 0);
  NTP_TEST_CHECK_EQ(empty.port(), static_cast<std::uint16_t>(0));
  NTP_TEST_CHECK(empty.address().empty());
  NTP_TEST_CHECK(empty.to_string().empty());
  NTP_TEST_CHECK_EQ(empty.native_length(), 0);
  // The storage is always a real object, even when unused, so the raw pointer
  // never has to be null-checked by callers.
  NTP_TEST_CHECK(empty.native_address() != NULL);
}

NTP_TEST(endpoint, adopts_a_raw_ipv4_address) {
  // 192.0.2.1 is from the RFC 5737 documentation range: routable-looking, and
  // guaranteed never to be a real host.
  const std::uint32_t kDocumentationAddress = 0xC0000201u;

  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = static_cast<decltype(address.sin_family)>(AF_INET);
  address.sin_port = static_cast<decltype(address.sin_port)>(htons(123));
  address.sin_addr.s_addr = htonl(kDocumentationAddress);

  const endpoint parsed = endpoint::from_native(&address, static_cast<int>(sizeof(address)));
  NTP_TEST_CHECK(parsed.valid());
  NTP_TEST_CHECK(!parsed.is_ipv6());
  NTP_TEST_CHECK_EQ(parsed.family(), AF_INET);
  NTP_TEST_CHECK_EQ(parsed.port(), static_cast<std::uint16_t>(123));
  NTP_TEST_CHECK_STREQ(parsed.address().c_str(), "192.0.2.1");
  NTP_TEST_CHECK_STREQ(parsed.to_string().c_str(), "192.0.2.1:123");
}

NTP_TEST(endpoint, rejects_unusable_raw_addresses) {
  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = static_cast<decltype(address.sin_family)>(AF_INET);

  NTP_TEST_CHECK(!endpoint::from_native(NULL, 16).valid());
  NTP_TEST_CHECK(!endpoint::from_native(&address, 0).valid());
  NTP_TEST_CHECK(!endpoint::from_native(&address, -1).valid());
  // A length larger than the storage it has to fit in comes from a kernel
  // call, so it is checked rather than trusted.
  NTP_TEST_CHECK(
      !endpoint::from_native(&address, static_cast<int>(sizeof(sockaddr_storage) + 1)).valid());
  NTP_TEST_CHECK(endpoint::from_native(&address, static_cast<int>(sizeof(address))).valid());
}

NTP_TEST(endpoint, loopback_and_any_use_the_right_addresses) {
  NTP_TEST_CHECK_STREQ(endpoint::loopback(address_family::ipv4, 1).address().c_str(), "127.0.0.1");
  NTP_TEST_CHECK_STREQ(endpoint::any(address_family::ipv4, 1).address().c_str(), "0.0.0.0");
  NTP_TEST_CHECK(!endpoint::loopback(address_family::ipv4, 1).is_ipv6());
  // "any" on a socket means IPv4 here, so loopback/any must agree.
  NTP_TEST_CHECK_STREQ(endpoint::loopback(address_family::any, 1).address().c_str(), "127.0.0.1");

#if NTP_LITE_ENABLE_IPV6
  NTP_TEST_CHECK_STREQ(endpoint::loopback(address_family::ipv6, 1).address().c_str(), "::1");
  NTP_TEST_CHECK_STREQ(endpoint::any(address_family::ipv6, 1).address().c_str(), "::");
  NTP_TEST_CHECK(endpoint::loopback(address_family::ipv6, 1).is_ipv6());
  NTP_TEST_CHECK_EQ(endpoint::loopback(address_family::ipv6, 1).family(), AF_INET6);
  // IPv6 text is bracketed once a port has to follow it.
  NTP_TEST_CHECK_STREQ(endpoint::loopback(address_family::ipv6, 123).to_string().c_str(),
                       "[::1]:123");
#endif
}

NTP_TEST(endpoint, set_port_leaves_the_address_alone) {
  endpoint address = endpoint::loopback(address_family::ipv4, 123);
  address.set_port(9999);
  NTP_TEST_CHECK_EQ(address.port(), static_cast<std::uint16_t>(9999));
  NTP_TEST_CHECK_STREQ(address.address().c_str(), "127.0.0.1");

  // Setting a port on an invalid endpoint must not create one out of nothing.
  endpoint empty;
  empty.set_port(9999);
  NTP_TEST_CHECK(!empty.valid());
}

NTP_TEST(endpoint, equality_compares_address_and_port) {
  const endpoint first = endpoint::loopback(address_family::ipv4, 123);
  const endpoint same = endpoint::loopback(address_family::ipv4, 123);
  const endpoint other_port = endpoint::loopback(address_family::ipv4, 124);
  const endpoint other_family = endpoint::loopback(address_family::ipv6, 123);

  NTP_TEST_CHECK(first == same);
  NTP_TEST_CHECK(first != other_port);
  NTP_TEST_CHECK(first != other_family);
  NTP_TEST_CHECK(endpoint() == endpoint());
  NTP_TEST_CHECK(endpoint() != first);
}

// ---------------------------------------------------------------------------
// Name resolution
// ---------------------------------------------------------------------------

NTP_TEST(resolve, numeric_ipv4_literal_needs_no_dns) {
  std::vector<endpoint> found;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(ntplite::detail::resolve("127.0.0.1", 123, address_family::ipv4, found, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::ok);
  NTP_TEST_CHECK(!found.empty());
  NTP_TEST_CHECK_EQ(found[0].family(), AF_INET);
  NTP_TEST_CHECK_EQ(found[0].port(), static_cast<std::uint16_t>(123));
  NTP_TEST_CHECK_STREQ(found[0].address().c_str(), "127.0.0.1");
}

NTP_TEST(resolve, numeric_ipv6_literal_needs_no_dns) {
#if NTP_LITE_ENABLE_IPV6
  std::vector<endpoint> found;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(ntplite::detail::resolve("::1", 123, address_family::ipv6, found, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::ok);
  NTP_TEST_CHECK(!found.empty());
  NTP_TEST_CHECK(found[0].is_ipv6());
  NTP_TEST_CHECK_EQ(found[0].port(), static_cast<std::uint16_t>(123));
  NTP_TEST_CHECK_STREQ(found[0].address().c_str(), "::1");
#endif
}

NTP_TEST(resolve, a_name_is_offered_each_address_once) {
  // A hosts file that lists a name twice, or a resolver that answers from more
  // than one source, can hand back the same address more than once.  Trying it
  // again cannot produce a different answer - only a second timeout and a second
  // datagram - so the list must not repeat itself.  The count is deliberately
  // not asserted: how many addresses "localhost" has is the machine's business.
  std::vector<endpoint> found;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(ntplite::detail::resolve("localhost", 123, address_family::ipv4, found, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::ok);
  NTP_TEST_CHECK(!found.empty());

  for (std::size_t i = 0; i < found.size(); ++i) {
    for (std::size_t j = i + 1; j < found.size(); ++j) {
      NTP_TEST_CHECK_NE(found[i].to_string(), found[j].to_string());
    }
  }
}

NTP_TEST(resolve, a_host_is_required) {
  std::vector<endpoint> found;
  error_code ec = error_code::ok;

  NTP_TEST_CHECK(!ntplite::detail::resolve(static_cast<const char*>(NULL), 123, address_family::any,
                                           found, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);

  NTP_TEST_CHECK(!ntplite::detail::resolve("", 123, address_family::any, found, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(found.empty());
}

NTP_TEST(resolve, an_unresolvable_name_fails) {
  std::vector<endpoint> found;
  error_code ec = error_code::ok;
  // ".invalid" is reserved by RFC 6761 and can never be registered, so no name
  // under it will ever resolve.  A resolver is still consulted, which is why
  // this is the one test here that touches the network stack.
  NTP_TEST_CHECK(!ntplite::detail::resolve("ntplite-does-not-exist.invalid", 123,
                                           address_family::any, found, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::resolve_error);
  NTP_TEST_CHECK(found.empty());
}

NTP_TEST(resolve, the_family_filter_is_enforced) {
  std::vector<endpoint> found;
  error_code ec = error_code::ok;
  // An IPv6 literal is not an IPv4 address; whichever way the resolver refuses
  // it, no endpoint may come back.
  NTP_TEST_CHECK(!ntplite::detail::resolve("::1", 123, address_family::ipv4, found, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::resolve_error);
  NTP_TEST_CHECK(found.empty());
}

NTP_TEST(resolve, supported_families_are_reported_accurately) {
  NTP_TEST_CHECK(ntplite::detail::address_family_supported(address_family::ipv4));
  NTP_TEST_CHECK(ntplite::detail::address_family_supported(address_family::any));
#if NTP_LITE_ENABLE_IPV6
  NTP_TEST_CHECK(ntplite::detail::address_family_supported(address_family::ipv6));
#else
  NTP_TEST_CHECK(!ntplite::detail::address_family_supported(address_family::ipv6));
#endif
}

NTP_TEST(resolve, a_compiled_out_family_is_rejected_without_touching_the_resolver) {
#if NTP_LITE_ENABLE_IPV6
  // Nothing to assert: the branch only exists for builds with IPv6 turned off.
#else
  std::vector<endpoint> found;
  error_code ec = error_code::ok;
  // A numeric literal, and still no resolver is consulted - the family check
  // runs first, so this cannot be slow or depend on the network.
  NTP_TEST_CHECK(!ntplite::detail::resolve("::1", 123, address_family::ipv6, found, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::unsupported);
  NTP_TEST_CHECK(found.empty());
#endif
}

// ---------------------------------------------------------------------------
// Subsystem lifetime
// ---------------------------------------------------------------------------

NTP_TEST(runtime, is_ready_and_shared_by_every_translation_unit) {
  const ntplite::detail::socket_runtime& first = ntplite::detail::socket_runtime_instance();
  const ntplite::detail::socket_runtime& second = ntplite::detail::socket_runtime_instance();
  // Identity, not equality: WSAStartup() may be called twice but WSACleanup()
  // may not, so there must be exactly one instance per program.  The accessor
  // is inline, which is what makes that true across translation units.
  NTP_TEST_CHECK(&first == &second);
  NTP_TEST_CHECK(first.ready());
  NTP_TEST_CHECK_EQ(first.error(), 0);
}

// ---------------------------------------------------------------------------
// Deadlines
// ---------------------------------------------------------------------------

NTP_TEST(deadline, never_is_unbounded) {
  const deadline never = deadline::never();
  NTP_TEST_CHECK(!never.valid());
  NTP_TEST_CHECK(!never.expired());
  NTP_TEST_CHECK_EQ(never.remaining_milliseconds(), static_cast<std::int64_t>(-1));
  NTP_TEST_CHECK_EQ(never.wait_timeout_ms(), -1);
}

NTP_TEST(deadline, a_zero_length_deadline_is_already_expired) {
  const deadline now = deadline::after(0);
  NTP_TEST_CHECK(now.valid());
  NTP_TEST_CHECK(now.expired());
  NTP_TEST_CHECK_EQ(now.remaining_milliseconds(), static_cast<std::int64_t>(0));
  NTP_TEST_CHECK_EQ(now.wait_timeout_ms(), 0);

  const deadline past = deadline::after(-1000);
  NTP_TEST_CHECK(past.expired());
  NTP_TEST_CHECK_EQ(past.wait_timeout_ms(), 0);
}

NTP_TEST(deadline, counts_down_and_never_reports_a_negative_budget) {
  const deadline soon = deadline::after(5000);
  NTP_TEST_CHECK(soon.valid());
  NTP_TEST_CHECK(!soon.expired());

  const std::int64_t remaining = soon.remaining_milliseconds();
  NTP_TEST_CHECK(remaining > 0);
  NTP_TEST_CHECK(remaining <= 5000);

  const int wait = soon.wait_timeout_ms();
  NTP_TEST_CHECK(wait > 0);
  NTP_TEST_CHECK(wait <= 5000);

  // A deadline further out must never report less time left than a nearer one.
  const deadline later = deadline::after(60000);
  NTP_TEST_CHECK(later.remaining_milliseconds() > remaining);
}

// ---------------------------------------------------------------------------
// Sleeping
// ---------------------------------------------------------------------------

NTP_TEST(sleep, a_non_positive_request_returns_immediately) {
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  ntplite::detail::sleep_ms(0);
  ntplite::detail::sleep_ms(-5);
  NTP_TEST_CHECK(elapsed_ms(start) < 500);
}

NTP_TEST(sleep, waits_at_least_as_long_as_asked) {
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  ntplite::detail::sleep_ms(40);
  // Only the lower bound is asserted tightly: sleep() may always overshoot on
  // a loaded machine, so an upper bound would only buy flakiness.
  NTP_TEST_CHECK(elapsed_ms(start) >= 30);
}

// ---------------------------------------------------------------------------
// udp_socket
// ---------------------------------------------------------------------------

NTP_TEST(socket, a_default_constructed_socket_is_closed) {
  udp_socket socket;
  NTP_TEST_CHECK(!socket.is_open());
  error_code ec = error_code::ok;
  endpoint local;
  NTP_TEST_CHECK(!socket.local_endpoint(local, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  // Closing twice, and closing something never opened, must both be no-ops.
  socket.close();
  socket.close();
  NTP_TEST_CHECK(!socket.is_open());
}

NTP_TEST(socket, opens_udp_sockets_for_both_families) {
  error_code ec = error_code::ok;

  udp_socket ipv4;
  NTP_TEST_CHECK(ipv4.open(address_family::ipv4, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::ok);
  NTP_TEST_CHECK(ipv4.is_open());

  // "any" on a socket means IPv4, so it must open successfully too.
  udp_socket fallback;
  NTP_TEST_CHECK(fallback.open(address_family::any, ec));
  NTP_TEST_CHECK(fallback.is_open());

  // Re-opening an open socket must replace it, not leak the old handle.
  NTP_TEST_CHECK(ipv4.open(address_family::ipv4, ec));
  NTP_TEST_CHECK(ipv4.is_open());

#if NTP_LITE_ENABLE_IPV6
  udp_socket ipv6;
  NTP_TEST_CHECK(ipv6.open(address_family::ipv6, ec));
  NTP_TEST_CHECK(ipv6.is_open());
#else
  udp_socket ipv6;
  NTP_TEST_CHECK(!ipv6.open(address_family::ipv6, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::unsupported);
#endif
}

NTP_TEST(socket, moves_transfer_ownership_of_the_handle) {
  error_code ec = error_code::ok;
  udp_socket source;
  NTP_TEST_REQUIRE(source.open(address_family::ipv4, ec));
  const native_socket handle = source.handle();

  udp_socket moved(std::move(source));
  NTP_TEST_CHECK(!source.is_open());
  NTP_TEST_CHECK(moved.is_open());
  NTP_TEST_CHECK_EQ(moved.handle(), handle);

  udp_socket target;
  target = std::move(moved);
  NTP_TEST_CHECK(!moved.is_open());
  NTP_TEST_CHECK_EQ(target.handle(), handle);

  // Self-move must not destroy the handle.  Going through a reference keeps
  // the intent explicit and the compiler quiet.
  udp_socket& alias = target;
  target = std::move(alias);
  NTP_TEST_CHECK(target.is_open());
  NTP_TEST_CHECK_EQ(target.handle(), handle);

  target.close();
  NTP_TEST_CHECK(!target.is_open());
}

NTP_TEST(socket, reports_the_port_the_kernel_chose) {
  udp_socket socket;
  endpoint local;
  NTP_TEST_REQUIRE(make_bound_loopback_socket(address_family::ipv4, socket, local));
  NTP_TEST_CHECK(local.valid());
  NTP_TEST_CHECK(local.port() != 0);
  NTP_TEST_CHECK_STREQ(local.address().c_str(), "127.0.0.1");
}

NTP_TEST(socket, rejects_arguments_that_cannot_work) {
  error_code ec = error_code::ok;
  udp_socket socket;
  char buffer[16];
  std::size_t received = 0;
  endpoint local;

  NTP_TEST_CHECK(!socket.bind(endpoint::loopback(address_family::ipv4, 0), ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.bind(endpoint(), ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.connect(endpoint(), ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.send(NULL, 0, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.send(buffer, sizeof(buffer), ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.send_to(endpoint(), buffer, sizeof(buffer), ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.receive(buffer, sizeof(buffer), received, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.receive_from(buffer, sizeof(buffer), received, local, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.set_nonblocking(true, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.set_reuse_address(true, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);

  // Now with a real handle: the same calls fail for a different reason, which
  // is the distinction the transport has to make for the client to trust it.
  NTP_TEST_REQUIRE(socket.open(address_family::ipv4, ec));
  NTP_TEST_REQUIRE(socket.bind(endpoint::loopback(address_family::ipv4, 0), ec));
  NTP_TEST_CHECK(!socket.receive(buffer, 0, received, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
  NTP_TEST_CHECK(!socket.receive(NULL, sizeof(buffer), received, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
}

NTP_TEST(socket, socket_options_are_accepted) {
  error_code ec = error_code::ok;
  udp_socket socket;
  NTP_TEST_REQUIRE(socket.open(address_family::ipv4, ec));
  NTP_TEST_CHECK(socket.set_reuse_address(true, ec));
  NTP_TEST_CHECK(socket.set_reuse_address(false, ec));
  NTP_TEST_CHECK(socket.set_broadcast(true, ec));
  NTP_TEST_CHECK(socket.set_nonblocking(true, ec));
  NTP_TEST_CHECK(socket.set_nonblocking(false, ec));
}

NTP_TEST(socket, a_loopback_datagram_round_trip) {
  udp_socket server;
  endpoint server_address;
  NTP_TEST_REQUIRE(make_bound_loopback_socket(address_family::ipv4, server, server_address));
  NTP_TEST_REQUIRE(server_address.port() != 0);

  error_code ec = error_code::ok;
  udp_socket client;
  NTP_TEST_REQUIRE(client.open(address_family::ipv4, ec));
  NTP_TEST_REQUIRE(client.bind(endpoint::loopback(address_family::ipv4, 0), ec));
  endpoint client_address;
  NTP_TEST_REQUIRE(client.local_endpoint(client_address, ec));

  const char payload[] = "ntplite";
  NTP_TEST_CHECK(client.send_to(server_address, payload, sizeof(payload), ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::ok);

  NTP_TEST_CHECK(ntplite::detail::wait_readable(server.handle(), 5000, ec) == wait_result::ready);

  char buffer[64];
  std::memset(buffer, 0, sizeof(buffer));
  std::size_t received = 0;
  endpoint from;
  NTP_TEST_CHECK(server.receive_from(buffer, sizeof(buffer), received, from, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::ok);
  NTP_TEST_CHECK_EQ(received, sizeof(payload));
  NTP_TEST_CHECK_STREQ(buffer, payload);
  // The sender the kernel reports must be the socket we actually sent from,
  // which is how a client recognises a reply to its own request.
  NTP_TEST_CHECK(from == client_address);
}

NTP_TEST(socket, a_datagram_on_a_connected_socket) {
  udp_socket server;
  endpoint server_address;
  NTP_TEST_REQUIRE(make_bound_loopback_socket(address_family::ipv4, server, server_address));

  error_code ec = error_code::ok;
  udp_socket client;
  NTP_TEST_REQUIRE(client.open(address_family::ipv4, ec));
  NTP_TEST_REQUIRE(client.connect(server_address, ec));

  const char payload[] = {'N', 'T', 'P', '\0'};
  NTP_TEST_CHECK(client.send(payload, sizeof(payload), ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::ok);

  NTP_TEST_CHECK(ntplite::detail::wait_readable(server.handle(), 5000, ec) == wait_result::ready);

  char buffer[64];
  std::memset(buffer, 0, sizeof(buffer));
  std::size_t received = 0;
  NTP_TEST_CHECK(server.receive(buffer, sizeof(buffer), received, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::ok);
  NTP_TEST_CHECK_EQ(received, sizeof(payload));
  NTP_TEST_CHECK(std::memcmp(buffer, payload, sizeof(payload)) == 0);
}

NTP_TEST(socket, a_fresh_socket_is_writable) {
  error_code ec = error_code::ok;
  udp_socket socket;
  NTP_TEST_REQUIRE(socket.open(address_family::ipv4, ec));
  NTP_TEST_CHECK(ntplite::detail::wait_writable(socket.handle(), 1000, ec) == wait_result::ready);
}

NTP_TEST(socket, waiting_on_an_invalid_handle_fails) {
  error_code ec = error_code::ok;
  NTP_TEST_CHECK(ntplite::detail::wait_readable(ntplite::detail::invalid_native_socket(), 0, ec) ==
                 wait_result::failed);
  NTP_TEST_CHECK_ERROR(ec, error_code::invalid_argument);
}

NTP_TEST(socket, waiting_for_a_silent_peer_times_out) {
  udp_socket server;
  endpoint server_address;
  NTP_TEST_REQUIRE(make_bound_loopback_socket(address_family::ipv4, server, server_address));

  error_code ec = error_code::ok;
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  const wait_result result = ntplite::detail::wait_readable(server.handle(), 150, ec);
  const std::int64_t waited = elapsed_ms(start);

  NTP_TEST_CHECK(result == wait_result::timed_out);
  NTP_TEST_CHECK_ERROR(ec, error_code::timeout);
  // It has to have actually waited, otherwise the "no reply yet" path is being
  // taken for the wrong reason.
  NTP_TEST_CHECK(waited >= 100);
  NTP_TEST_CHECK(waited < 5000);
}

NTP_TEST(socket, a_non_blocking_receive_reports_nothing_available) {
  udp_socket server;
  endpoint server_address;
  NTP_TEST_REQUIRE(make_bound_loopback_socket(address_family::ipv4, server, server_address));

  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.set_nonblocking(true, ec));

  char buffer[64];
  std::size_t received = 99;
  NTP_TEST_CHECK(!server.receive(buffer, sizeof(buffer), received, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::timeout);
  // Nothing was received, so the count must have been cleared rather than
  // left holding whatever the caller had there before.
  NTP_TEST_CHECK_EQ(received, static_cast<std::size_t>(0));

  endpoint from;
  NTP_TEST_CHECK(!server.receive_from(buffer, sizeof(buffer), received, from, ec));
  NTP_TEST_CHECK_ERROR(ec, error_code::timeout);
  NTP_TEST_CHECK(!from.valid());
}

NTP_TEST(socket, a_zero_length_wait_polls_without_blocking) {
  udp_socket server;
  endpoint server_address;
  NTP_TEST_REQUIRE(make_bound_loopback_socket(address_family::ipv4, server, server_address));

  error_code ec = error_code::ok;
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  const wait_result result = ntplite::detail::wait_readable(server.handle(), 0, ec);
  NTP_TEST_CHECK(result == wait_result::timed_out);
  NTP_TEST_CHECK_ERROR(ec, error_code::timeout);
  NTP_TEST_CHECK(elapsed_ms(start) < 500);
}
