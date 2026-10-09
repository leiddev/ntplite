// End to end tests: a real client, a real socket, and a server on loopback.
//
// These drive `ntplite::query()` over the wire rather than through the library's
// own building blocks, so they cover what unit tests cannot reach: resolving a
// name, timing a real exchange, the retry schedule, and the exact reason a bad
// answer is turned away.  The server is `mock_ntp_server.hpp`, which shares the
// packet codec with the client under test - a deliberate weakness, which is why
// `scripts/cross_validate_ntp.py` parses the same wire format independently.

#include <cmath>
#include <cstdint>
#include <ntplite/ntplite.hpp>
#include <string>

#include "mock_ntp_server.hpp"
#include "ntplite_test.hpp"

namespace {

using ::ntplite::compare;
using ::ntplite::duration_seconds;
using ::ntplite::error_code;
using ::ntplite::ip_version;
using ::ntplite::query;
using ::ntplite::query_options;
using ::ntplite::query_result;

/// Options pointed at `server`, with budgets short enough that the cases which
/// must fail by timing out do not hold the suite up.
///
/// The address family is pinned to IPv4 because the mock binds a v4 loopback
/// socket: with the default "any", a resolver that offered "::1" first would
/// make every case here wait out an extra timeout and count an extra attempt.
query_options options_for(const ::ntplite_test::mock_server& server) {
  query_options options;
  options.port = server.address().port();
  options.ip = ip_version::ipv4;
  options.server_timeout_ms = 2000;
  options.total_timeout_ms = 4000;
  options.retry_interval_ms = 50;
  return options;
}

/// The same, but with a budget that runs out quickly.
///
/// 600 ms with a 50 ms retry interval leaves room for three datagrams with a
/// wide margin, so the attempt count asserted below does not depend on how
/// quickly the machine gets round the loop.
query_options impatient_options_for(const ::ntplite_test::mock_server& server) {
  query_options options = options_for(server);
  options.server_timeout_ms = 600;
  return options;
}

}  // namespace

NTP_TEST(end_to_end, answers_from_a_local_server) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result result;
  const error_code status = query(ntplite_test::mock_server_host, result, options_for(server));

  NTP_TEST_CHECK(status == error_code::ok);
  NTP_TEST_CHECK(result.valid);
  NTP_TEST_CHECK(!result.kiss_of_death);
  NTP_TEST_CHECK(result.delay_is_plausible);

  // Who answered, and what it said about itself.
  NTP_TEST_CHECK_EQ(result.server, server.address().to_string());
  NTP_TEST_CHECK_EQ(result.stratum, 2);
  NTP_TEST_CHECK_EQ(result.mode, ::ntplite::detail::mode::server);
  NTP_TEST_CHECK_EQ(result.version, ::ntplite::detail::version_4);
  NTP_TEST_CHECK_EQ(result.leap, ::ntplite::detail::leap::no_warning);
  NTP_TEST_CHECK_EQ(result.reference, std::string("192.0.2.1"));
  NTP_TEST_CHECK_EQ(result.kiss_code, std::string());
  NTP_TEST_CHECK_EQ(result.poll, ::ntplite::detail::default_poll);
  NTP_TEST_CHECK_EQ(result.precision, ::ntplite::detail::default_precision);
  NTP_TEST_CHECK_NEAR(duration_seconds(result.root_delay), 0.001, 1e-5);
  NTP_TEST_CHECK_NEAR(duration_seconds(result.root_dispersion), 0.002, 1e-5);

  // One question asked, one answer given.
  NTP_TEST_CHECK_EQ(result.attempts, 1);
  NTP_TEST_CHECK_EQ(server.requests_seen(), 1);
  NTP_TEST_CHECK_EQ(server.replies_sent(), 1);

  // The four timestamps are in the order a well behaved exchange produces: the
  // request leaves, the server reads its clock once, answers from that same
  // reading, and the answer arrives.  Comparing them is the strongest statement
  // available without a second clock.
  NTP_TEST_CHECK(compare(result.local_send, result.server_receive) < 0);
  NTP_TEST_CHECK(compare(result.server_receive, result.server_send) == 0);
  NTP_TEST_CHECK(compare(result.server_send, result.local_receive) < 0);

  // Both machines keep the same time here, so the offset and the network delay
  // are both small.
  NTP_TEST_CHECK(std::fabs(result.offset_seconds()) < 0.01);
  NTP_TEST_CHECK(duration_seconds(result.round_trip_delay) >= 0.0);
  NTP_TEST_CHECK(duration_seconds(result.round_trip_time) > 0.0);

  // The estimate of "now" is the local receive time plus the offset, so it must
  // agree with the server's own answer to well within the round trip.
  NTP_TEST_CHECK(
      std::fabs(duration_seconds(::ntplite::difference(result.server_time, result.local_receive)) -
                result.offset_seconds()) < 1e-9);
}

NTP_TEST(end_to_end, reports_a_shifted_clock) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  behaviour.clock_offset_nanoseconds = 5000000000LL;  // five seconds ahead
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result ahead;
  NTP_TEST_CHECK(query(ntplite_test::mock_server_host, ahead, options_for(server)) ==
                 error_code::ok);
  NTP_TEST_REQUIRE(ahead.valid);

  // Absolute rather than CHECK_NEAR's relative fallback, which would let a five
  // second offset hide a fifty millisecond error.  A loopback exchange should be
  // three orders of magnitude better than the bound used here: if the four
  // timestamps were misused at all the error would be seconds, not milliseconds.
  NTP_TEST_CHECK(std::fabs(ahead.offset_seconds() - 5.0) < 0.01);
  NTP_TEST_CHECK(duration_seconds(::ntplite::difference(ahead.server_time, ahead.local_receive)) >
                 4.99);

  // The same server, now behind us: the sign has to follow the data.
  behaviour.clock_offset_nanoseconds = -2000000000LL;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result behind;
  NTP_TEST_CHECK(query(ntplite_test::mock_server_host, behind, options_for(server)) ==
                 error_code::ok);
  NTP_TEST_REQUIRE(behind.valid);

  NTP_TEST_CHECK(std::fabs(behind.offset_seconds() + 2.0) < 0.01);
  NTP_TEST_CHECK(compare(behind.server_time, behind.local_receive) < 0);
}

NTP_TEST(end_to_end, answers_a_retry_after_ignoring_the_first_request) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  behaviour.answer_from_attempt = 2;  // let the first request go by
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result result;
  NTP_TEST_CHECK(query(ntplite_test::mock_server_host, result, options_for(server)) ==
                 error_code::ok);
  NTP_TEST_REQUIRE(result.valid);

  // One retry, and the retry is the one that was answered: a client that gave up
  // after the first silent interval, or that kept asking after an answer, would
  // show a different count.
  NTP_TEST_CHECK_EQ(result.attempts, 2);
  NTP_TEST_CHECK_EQ(server.requests_seen(), 2);
  NTP_TEST_CHECK_EQ(server.replies_sent(), 1);
}

NTP_TEST(end_to_end, reports_a_kiss_of_death) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  behaviour.stratum = 0;
  behaviour.reference_id = ::ntplite::detail::kiss_rate;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result result;
  const error_code status = query(ntplite_test::mock_server_host, result, options_for(server));

  NTP_TEST_CHECK(status == error_code::kiss_of_death);
  NTP_TEST_CHECK(!result.valid);
  NTP_TEST_CHECK(result.kiss_of_death);
  NTP_TEST_CHECK_EQ(result.kiss_code, std::string("RATE"));
  NTP_TEST_CHECK_EQ(result.reference, std::string("RATE"));
  NTP_TEST_CHECK_EQ(result.stratum, 0);

  // A refusal is final: asking again would earn the same refusal, and the point
  // of the code is to stop a client from hammering a server.
  NTP_TEST_CHECK_EQ(result.attempts, 1);
  NTP_TEST_CHECK_EQ(server.requests_seen(), 1);
}

NTP_TEST(end_to_end, ends_the_exchange_on_an_unusable_answer) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  behaviour.mode = ::ntplite::detail::mode::client;  // a reply that is not a reply
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result result;
  const error_code status = query(ntplite_test::mock_server_host, result, options_for(server));

  NTP_TEST_CHECK(status == error_code::protocol_error);
  NTP_TEST_CHECK(!result.valid);
  NTP_TEST_CHECK(!result.kiss_of_death);

  // It is ours - the origin field echoed the request - so it is an answer of the
  // wrong shape, not a stray datagram.  Reporting it beats waiting for a better
  // one that is never coming.
  NTP_TEST_CHECK_EQ(result.attempts, 1);
  NTP_TEST_CHECK_EQ(server.requests_seen(), 1);
}

NTP_TEST(end_to_end, discards_a_truncated_datagram) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  behaviour.kind = ntplite_test::reply_kind::truncated;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result result;
  const error_code status =
      query(ntplite_test::mock_server_host, result, impatient_options_for(server));

  // A short datagram is a runt, not an answer: it is dropped and the request is
  // sent again until the budget is gone.  The server answers every time.
  NTP_TEST_CHECK(status == error_code::timeout);
  NTP_TEST_CHECK(!result.valid);
  NTP_TEST_CHECK_EQ(result.attempts, 3);
  NTP_TEST_CHECK_EQ(server.requests_seen(), 3);
  NTP_TEST_CHECK_EQ(server.replies_sent(), 3);
}

NTP_TEST(end_to_end, discards_a_garbage_datagram) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  behaviour.kind = ntplite_test::reply_kind::garbage;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result result;
  const error_code status =
      query(ntplite_test::mock_server_host, result, impatient_options_for(server));

  // Noise neither matches the request's origin nor survives decoding, so it must
  // never be mistaken for an answer - and must not be mistaken for a refusal
  // either, because a spoofed stratum 0 packet would otherwise be a cheap way to
  // take a client out of service.
  NTP_TEST_CHECK(status == error_code::timeout);
  NTP_TEST_CHECK(!result.valid);
  NTP_TEST_CHECK(!result.kiss_of_death);
  NTP_TEST_CHECK_EQ(result.attempts, 3);
  NTP_TEST_CHECK_EQ(server.requests_seen(), 3);
}

NTP_TEST(end_to_end, accepts_a_duplicated_answer_once) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  behaviour.kind = ntplite_test::reply_kind::duplicate;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result result;
  NTP_TEST_CHECK(query(ntplite_test::mock_server_host, result, options_for(server)) ==
                 error_code::ok);
  NTP_TEST_REQUIRE(result.valid);

  // The second copy arrives after the first has been accepted, and one answer is
  // all it takes: no extra request goes out, so the duplicate cannot turn into a
  // second exchange.
  NTP_TEST_CHECK_EQ(result.attempts, 1);
  NTP_TEST_CHECK_EQ(server.requests_seen(), 1);
  NTP_TEST_CHECK_EQ(server.replies_sent(), 2);
}

NTP_TEST(end_to_end, times_out_on_a_silent_server) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  behaviour.kind = ntplite_test::reply_kind::silence;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result result;
  const error_code status =
      query(ntplite_test::mock_server_host, result, impatient_options_for(server));

  NTP_TEST_CHECK(status == error_code::timeout);
  NTP_TEST_CHECK(!result.valid);
  NTP_TEST_CHECK(!result.kiss_of_death);
  NTP_TEST_CHECK_EQ(result.attempts, 3);
  NTP_TEST_CHECK_EQ(server.requests_seen(), 3);
  NTP_TEST_CHECK_EQ(server.replies_sent(), 0);

  // A timeout leaves the timestamps of the last attempt in place, so a caller
  // can still say how long it waited.
  NTP_TEST_CHECK(compare(result.local_send, result.local_receive) <= 0);
}

NTP_TEST(end_to_end, flags_a_server_that_overstates_its_processing_time) {
  ntplite_test::mock_server server;
  ntplite_test::server_behaviour behaviour;
  // 200 ms of "processing" on a loopback exchange that takes a fraction of a
  // millisecond: a server claiming this has either a broken clock or an agenda,
  // and either way the delay it implies is impossible.
  behaviour.hold_nanoseconds = 200000000LL;
  error_code ec = error_code::ok;
  NTP_TEST_REQUIRE(server.start(behaviour, ec));

  query_result result;
  NTP_TEST_CHECK(query(ntplite_test::mock_server_host, result, options_for(server)) ==
                 error_code::ok);
  NTP_TEST_REQUIRE(result.valid);

  NTP_TEST_CHECK_NEAR(duration_seconds(result.server_processing), 0.2, 1e-6);

  // RFC 5905 computes the delay as (T4 - T1) - (T3 - T2); when the server
  // overstates T3 - T2 that goes negative, which is the library's signal that
  // the offset - half the delay away from the truth at best - cannot be trusted.
  NTP_TEST_CHECK(duration_seconds(result.round_trip_delay) < 0.0);
  NTP_TEST_CHECK(!result.delay_is_plausible);
  NTP_TEST_CHECK_EQ(result.attempts, 1);

  // The offset is still reported, because it is what the timestamps say; the
  // half of the invented processing time the estimate absorbs is exactly the
  // error the flag warns about.
  NTP_TEST_CHECK_NEAR(result.offset_seconds(), 0.1, 0.005);
}
