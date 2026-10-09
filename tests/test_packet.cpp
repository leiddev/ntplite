// ============================================================================
// ntplite - tests/test_packet.cpp
// ----------------------------------------------------------------------------
// Unit tests for the NTPv4 wire format.
//
// The interesting property of this layer is that decoding is total: any 48
// byte pattern decodes to *some* packet.  All the "that reply is nonsense"
// logic therefore lives in validate_reply(), and that is where most of these
// tests point.
//
// The byte arrays below are written out by hand from RFC 5905 section 7.3.
// ============================================================================

#include <cstring>
#include <ntplite/detail/packet.hpp>

#include "ntplite_test.hpp"

namespace {

using ntplite::detail::decode_packet;
using ntplite::detail::encode_packet;
using ntplite::detail::flags_leap;
using ntplite::detail::flags_mode;
using ntplite::detail::flags_version;
using ntplite::detail::is_kiss_of_death;
using ntplite::detail::is_usable_stratum;
using ntplite::detail::kiss_code_name;
using ntplite::detail::kiss_rate;
using ntplite::detail::make_client_request;
using ntplite::detail::make_flags;
using ntplite::detail::narrow_int8;
using ntplite::detail::ntp_short;
using ntplite::detail::ntp_timestamp;
using ntplite::detail::pack_short;
using ntplite::detail::packet;
using ntplite::detail::packet_size;
using ntplite::detail::read_u32;
using ntplite::detail::reference_id_text;
using ntplite::detail::unpack_short;
using ntplite::detail::validate_reply;
using ntplite::detail::write_u32;

// 2026-01-01T00:00:00Z expressed as NTP seconds.
const std::uint32_t kYear2026NtpSeconds = 3976214400U;

// ---------------------------------------------------------------------------
// The li | vn | mode octet
// ---------------------------------------------------------------------------
NTP_TEST(packet, flags_are_packed_into_one_octet) {
  // LI=0, VN=4, mode=3 (client) -> 0b00_100_011
  NTP_TEST_CHECK_EQ(0x23, static_cast<int>(make_flags(0, 4, 3)));

  // LI=3, VN=4, mode=4 (server) -> 0b11_100_100
  NTP_TEST_CHECK_EQ(0xE4, static_cast<int>(make_flags(3, 4, 4)));

  // LI=1, VN=3, mode=5 (broadcast) -> 0b01_011_101
  NTP_TEST_CHECK_EQ(0x5D, static_cast<int>(make_flags(1, 3, 5)));
}

NTP_TEST(packet, flags_are_masked_to_their_width) {
  // Only two bits of LI, three of VN and three of mode survive.
  NTP_TEST_CHECK_EQ(0xFF, static_cast<int>(make_flags(0xFF, 0xFF, 0xFF)));
}

NTP_TEST(packet, flags_round_trip) {
  for (int leap = 0; leap <= 3; ++leap) {
    for (int version = 0; version <= 7; ++version) {
      for (int mode = 0; mode <= 7; ++mode) {
        const std::uint8_t packed =
            make_flags(static_cast<std::uint8_t>(leap), static_cast<std::uint8_t>(version),
                       static_cast<std::uint8_t>(mode));

        NTP_TEST_CHECK_EQ(leap, static_cast<int>(flags_leap(packed)));
        NTP_TEST_CHECK_EQ(version, static_cast<int>(flags_version(packed)));
        NTP_TEST_CHECK_EQ(mode, static_cast<int>(flags_mode(packed)));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Scalar helpers
// ---------------------------------------------------------------------------
NTP_TEST(packet, narrow_int8_is_portable) {
  NTP_TEST_CHECK_EQ(0, static_cast<int>(narrow_int8(0x00U)));
  NTP_TEST_CHECK_EQ(127, static_cast<int>(narrow_int8(0x7FU)));
  NTP_TEST_CHECK_EQ(-128, static_cast<int>(narrow_int8(0x80U)));
  NTP_TEST_CHECK_EQ(-1, static_cast<int>(narrow_int8(0xFFU)));
  NTP_TEST_CHECK_EQ(-20, static_cast<int>(narrow_int8(0xECU)));
}

NTP_TEST(packet, u32_helpers_are_big_endian) {
  const std::uint8_t bytes[4] = {0xEDU, 0x00U, 0x37U, 0x80U};
  NTP_TEST_CHECK_EQ(0xED003780UL, static_cast<unsigned long>(read_u32(bytes)));

  std::uint8_t out[4] = {0U, 0U, 0U, 0U};
  write_u32(out, 0xED003780UL);
  NTP_TEST_CHECK_EQ(0, std::memcmp(bytes, out, 4));
}

NTP_TEST(packet, short_helpers_pack_16_16) {
  ntp_short value;
  value.seconds = 0;
  value.fraction = 0x2000;
  NTP_TEST_CHECK_EQ(0x00002000UL, static_cast<unsigned long>(pack_short(value)));

  value.seconds = 1;
  value.fraction = 0x8000;
  NTP_TEST_CHECK_EQ(0x00018000UL, static_cast<unsigned long>(pack_short(value)));

  const ntp_short back = unpack_short(0x00018000UL);
  NTP_TEST_CHECK_EQ(1, static_cast<int>(back.seconds));
  NTP_TEST_CHECK_EQ(0x8000, static_cast<int>(back.fraction));
}

// ---------------------------------------------------------------------------
// Encoding a request
// ---------------------------------------------------------------------------

/// A NTPv4 client request with the default poll and precision fields, asking
/// at 2026-01-01T00:00:00.5Z.
const std::uint8_t kExpectedRequest[48] = {
    /*  0 */ 0x23, 0x00, 0x03, 0xEC,
    /*  4 */ 0x00, 0x00, 0x00, 0x00,
    /*  8 */ 0x00, 0x00, 0x00, 0x00,
    /* 12 */ 0x00, 0x00, 0x00, 0x00,
    /* 16 */ 0x00, 0x00, 0x00, 0x00,
    /* 20 */ 0x00, 0x00, 0x00, 0x00,
    /* 24 */ 0x00, 0x00, 0x00, 0x00,
    /* 28 */ 0x00, 0x00, 0x00, 0x00,
    /* 32 */ 0x00, 0x00, 0x00, 0x00,
    /* 36 */ 0x00, 0x00, 0x00, 0x00,
    /* 40 */ 0xED, 0x00, 0x37, 0x80, 0x80, 0x00, 0x00, 0x00};

/// The request that kExpectedRequest was built from.
packet request_for_kExpectedRequest() {
  ntp_timestamp transmit;
  transmit.seconds = kYear2026NtpSeconds;
  transmit.fraction = 0x80000000U;
  return make_client_request(transmit);
}

NTP_TEST(packet, encodes_a_request_byte_for_byte) {
  const packet request = request_for_kExpectedRequest();

  NTP_TEST_CHECK_EQ(4, static_cast<int>(request.version));
  NTP_TEST_CHECK_EQ(3, static_cast<int>(request.mode));
  NTP_TEST_CHECK_EQ(0, static_cast<int>(request.leap));
  NTP_TEST_CHECK_EQ(3, static_cast<int>(request.poll));
  NTP_TEST_CHECK_EQ(-20, static_cast<int>(request.precision));

  std::uint8_t encoded[packet_size];
  encode_packet(request, encoded);

  NTP_TEST_CHECK_EQ(0, std::memcmp(encoded, kExpectedRequest, packet_size));
}

// ---------------------------------------------------------------------------
// Decoding a reply
// ---------------------------------------------------------------------------

/// A stratum 2 server reply to kExpectedRequest, hand assembled.
const std::uint8_t kServerReply[48] = {
    /*  0 li/vn/mode */ 0x24,   0x02, 0x04, 0xEC,
    /*  4 root delay  */ 0x00,  0x00, 0x20, 0x00,
    /*  8 dispersion  */ 0x00,  0x00, 0x40, 0x00,
    /* 12 reference id*/ 0x0A,  0x00, 0x00, 0x01,
    /* 16 reference    */ 0xED, 0x00, 0x37, 0x40, 0x00, 0x00, 0x00, 0x00,
    /* 24 origin       */ 0xED, 0x00, 0x37, 0x80, 0x80, 0x00, 0x00, 0x00,
    /* 32 receive      */ 0xED, 0x00, 0x37, 0x80, 0xC0, 0x00, 0x00, 0x00,
    /* 40 transmit     */ 0xED, 0x00, 0x37, 0x80, 0xE0, 0x00, 0x00, 0x00};

NTP_TEST(packet, decodes_a_reply_field_by_field) {
  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kServerReply, packet_size, reply));

  NTP_TEST_CHECK_EQ(0, static_cast<int>(reply.leap));
  NTP_TEST_CHECK_EQ(4, static_cast<int>(reply.version));
  NTP_TEST_CHECK_EQ(4, static_cast<int>(reply.mode));
  NTP_TEST_CHECK_EQ(2, static_cast<int>(reply.stratum));
  NTP_TEST_CHECK_EQ(4, static_cast<int>(reply.poll));
  NTP_TEST_CHECK_EQ(-20, static_cast<int>(reply.precision));

  NTP_TEST_CHECK_EQ(0, static_cast<int>(reply.root_delay.seconds));
  NTP_TEST_CHECK_EQ(0x2000, static_cast<int>(reply.root_delay.fraction));
  NTP_TEST_CHECK_EQ(0, static_cast<int>(reply.root_dispersion.seconds));
  NTP_TEST_CHECK_EQ(0x4000, static_cast<int>(reply.root_dispersion.fraction));
  NTP_TEST_CHECK_EQ(0x0A000001UL, static_cast<unsigned long>(reply.reference_id));

  NTP_TEST_CHECK_EQ(kYear2026NtpSeconds - 64U, reply.reference.seconds);
  NTP_TEST_CHECK_EQ(0U, reply.reference.fraction);

  NTP_TEST_CHECK_EQ(kYear2026NtpSeconds, reply.origin.seconds);
  NTP_TEST_CHECK_EQ(0x80000000UL, static_cast<unsigned long>(reply.origin.fraction));

  NTP_TEST_CHECK_EQ(kYear2026NtpSeconds, reply.receive.seconds);
  NTP_TEST_CHECK_EQ(0xC0000000UL, static_cast<unsigned long>(reply.receive.fraction));

  NTP_TEST_CHECK_EQ(kYear2026NtpSeconds, reply.transmit.seconds);
  NTP_TEST_CHECK_EQ(0xE0000000UL, static_cast<unsigned long>(reply.transmit.fraction));
}

NTP_TEST(packet, encode_decode_round_trip) {
  packet original = request_for_kExpectedRequest();
  original.leap = 1;
  original.stratum = 7;
  original.poll = -3;
  original.precision = -6;
  original.root_delay.seconds = 0;
  original.root_delay.fraction = 0x1234;
  original.root_dispersion.seconds = 2;
  original.root_dispersion.fraction = 0xABCD;
  original.reference_id = 0xDEADBEEFUL;
  original.reference.seconds = 0x11111111UL;
  original.reference.fraction = 0x22222222UL;
  original.origin.seconds = 0x33333333UL;
  original.origin.fraction = 0x44444444UL;
  original.receive.seconds = 0x55555555UL;
  original.receive.fraction = 0x66666666UL;

  std::uint8_t encoded[packet_size];
  encode_packet(original, encoded);

  packet decoded;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(encoded, packet_size, decoded));

  NTP_TEST_CHECK_EQ(1, static_cast<int>(decoded.leap));
  NTP_TEST_CHECK_EQ(4, static_cast<int>(decoded.version));
  NTP_TEST_CHECK_EQ(3, static_cast<int>(decoded.mode));
  NTP_TEST_CHECK_EQ(7, static_cast<int>(decoded.stratum));
  NTP_TEST_CHECK_EQ(-3, static_cast<int>(decoded.poll));
  NTP_TEST_CHECK_EQ(-6, static_cast<int>(decoded.precision));
  NTP_TEST_CHECK_EQ(0x1234, static_cast<int>(decoded.root_delay.fraction));
  NTP_TEST_CHECK_EQ(2, static_cast<int>(decoded.root_dispersion.seconds));
  NTP_TEST_CHECK_EQ(0xABCD, static_cast<int>(decoded.root_dispersion.fraction));
  NTP_TEST_CHECK_EQ(0xDEADBEEFUL, static_cast<unsigned long>(decoded.reference_id));
  NTP_TEST_CHECK_EQ(0x11111111UL, static_cast<unsigned long>(decoded.reference.seconds));
  NTP_TEST_CHECK_EQ(0x22222222UL, static_cast<unsigned long>(decoded.reference.fraction));
  NTP_TEST_CHECK_EQ(0x33333333UL, static_cast<unsigned long>(decoded.origin.seconds));
  NTP_TEST_CHECK_EQ(0x44444444UL, static_cast<unsigned long>(decoded.origin.fraction));
  NTP_TEST_CHECK_EQ(0x55555555UL, static_cast<unsigned long>(decoded.receive.seconds));
  NTP_TEST_CHECK_EQ(0x66666666UL, static_cast<unsigned long>(decoded.receive.fraction));
  NTP_TEST_CHECK_EQ(kYear2026NtpSeconds, decoded.transmit.seconds);
  NTP_TEST_CHECK_EQ(0x80000000UL, static_cast<unsigned long>(decoded.transmit.fraction));
}

NTP_TEST(packet, decoding_never_fails_for_a_full_header) {
  // Every bit pattern is a legal encoding: decode is total, validation is not.
  for (int fill = 0; fill < 256; fill += 17) {
    std::uint8_t buffer[48];
    std::memset(buffer, fill, sizeof(buffer));

    packet decoded;
    NTP_TEST_CHECK_EQ(NTP_LITE_OK, decode_packet(buffer, sizeof(buffer), decoded));
  }
}

NTP_TEST(packet, decode_rejects_short_or_missing_buffers) {
  packet decoded;
  const std::uint8_t buffer[64] = {0};

  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_INVALID, decode_packet(NULL, 0, decoded));
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_INVALID, decode_packet(buffer, 0, decoded));
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_INVALID, decode_packet(buffer, 1, decoded));
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_INVALID, decode_packet(buffer, packet_size - 1, decoded));
  NTP_TEST_CHECK_EQ(NTP_LITE_OK, decode_packet(buffer, packet_size, decoded));
}

NTP_TEST(packet, decode_ignores_trailing_bytes) {
  // A server may pad the packet with extension fields or a MAC; everything
  // past the fixed header must be ignored rather than rejected.
  std::uint8_t buffer[68];
  std::memset(buffer, 0, sizeof(buffer));
  std::memcpy(buffer, kServerReply, packet_size);

  packet decoded;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(buffer, sizeof(buffer), decoded));
  NTP_TEST_CHECK_EQ(2, static_cast<int>(decoded.stratum));
  NTP_TEST_CHECK_EQ(0xE0000000UL, static_cast<unsigned long>(decoded.transmit.fraction));
}

// ---------------------------------------------------------------------------
// validate_reply
// ---------------------------------------------------------------------------
NTP_TEST(validate, accepts_a_well_formed_reply) {
  const packet request = request_for_kExpectedRequest();

  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kServerReply, packet_size, reply));

  NTP_TEST_CHECK_EQ(NTP_LITE_OK, validate_reply(reply, request));
}

NTP_TEST(validate, accepts_a_broadcast_reply) {
  const packet request = request_for_kExpectedRequest();

  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kServerReply, packet_size, reply));
  reply.mode = ntplite::detail::mode::broadcast;

  NTP_TEST_CHECK_EQ(NTP_LITE_OK, validate_reply(reply, request));
}

NTP_TEST(validate, rejects_a_client_mode_echo) {
  const packet request = request_for_kExpectedRequest();

  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kServerReply, packet_size, reply));
  reply.mode = ntplite::detail::mode::client;

  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_PROTOCOL, validate_reply(reply, request));
}

NTP_TEST(validate, rejects_an_unsupported_version) {
  const packet request = request_for_kExpectedRequest();

  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kServerReply, packet_size, reply));

  reply.version = 2;
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_PROTOCOL, validate_reply(reply, request));

  reply.version = 3;
  NTP_TEST_CHECK_EQ(NTP_LITE_OK, validate_reply(reply, request));

  reply.version = 5;
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_PROTOCOL, validate_reply(reply, request));
}

NTP_TEST(validate, rejects_an_unsynchronized_clock) {
  const packet request = request_for_kExpectedRequest();

  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kServerReply, packet_size, reply));
  reply.leap = ntplite::detail::leap::unsynchronized;

  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_PROTOCOL, validate_reply(reply, request));
}

NTP_TEST(validate, rejects_an_impossible_stratum) {
  const packet request = request_for_kExpectedRequest();

  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kServerReply, packet_size, reply));

  reply.stratum = 16;  // "unsynchronized" per RFC 5905
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_PROTOCOL, validate_reply(reply, request));

  reply.stratum = 255;
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_PROTOCOL, validate_reply(reply, request));
}

NTP_TEST(validate, rejects_a_mismatched_origin_timestamp) {
  const packet request = request_for_kExpectedRequest();

  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kServerReply, packet_size, reply));

  // One nanosecond of fraction is enough: this is what stops an off-path
  // attacker from guessing their way into a valid reply.
  reply.origin.fraction = reply.origin.fraction + 1U;
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_PROTOCOL, validate_reply(reply, request));

  reply.origin.fraction = reply.origin.fraction - 1U;
  reply.origin.seconds = reply.origin.seconds + 1U;
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_PROTOCOL, validate_reply(reply, request));
}

NTP_TEST(validate, rejects_a_zero_transmit_timestamp) {
  const packet request = request_for_kExpectedRequest();

  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kServerReply, packet_size, reply));

  reply.transmit.seconds = 0;
  reply.transmit.fraction = 0;
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_PROTOCOL, validate_reply(reply, request));
}

// ---------------------------------------------------------------------------
// Kiss-o'-Death
// ---------------------------------------------------------------------------

/// A stratum 0 reply carrying the "RATE" reference identifier.
const std::uint8_t kKissOfDeath[48] = {
    /*  0 */ 0x24, 0x00, 0x04, 0xEC,
    /*  4 */ 0x00, 0x00, 0x00, 0x00,
    /*  8 */ 0x00, 0x00, 0x00, 0x00,
    /* 12 */ 0x52, 0x41, 0x54, 0x45, /* "RATE" */
    /* 16 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 24 */ 0xED, 0x00, 0x37, 0x80, 0x80, 0x00, 0x00, 0x00,
    /* 32 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* 40 */ 0xED, 0x00, 0x37, 0x80, 0xE0, 0x00, 0x00, 0x00};

NTP_TEST(kiss, detects_a_rate_limit_packet) {
  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kKissOfDeath, packet_size, reply));

  NTP_TEST_CHECK(is_kiss_of_death(reply));
  NTP_TEST_CHECK_EQ(0, static_cast<int>(reply.stratum));
  NTP_TEST_CHECK_EQ(kiss_rate, reply.reference_id);
  NTP_TEST_CHECK_STREQ("RATE", kiss_code_name(reply.reference_id));
}

NTP_TEST(kiss, a_kiss_packet_is_reported_as_such_not_as_a_protocol_error) {
  const packet request = request_for_kExpectedRequest();

  packet reply;
  NTP_TEST_REQUIRE(NTP_LITE_OK == decode_packet(kKissOfDeath, packet_size, reply));

  // Note the ordering: stratum 0 also fails the "usable stratum" rule, but the
  // caller deserves the specific answer.
  NTP_TEST_CHECK_EQ(NTP_LITE_ERR_KOD, validate_reply(reply, request));
}

NTP_TEST(kiss, recognises_every_defined_code) {
  const std::uint32_t codes[] = {ntplite::detail::kiss_rate, ntplite::detail::kiss_deny,
                                 ntplite::detail::kiss_rstr, ntplite::detail::kiss_nkey,
                                 ntplite::detail::kiss_step, ntplite::detail::kiss_init,
                                 ntplite::detail::kiss_info, ntplite::detail::kiss_auth,
                                 ntplite::detail::kiss_tsrv, ntplite::detail::kiss_bcst,
                                 ntplite::detail::kiss_cryp};

  const char* const names[] = {"RATE", "DENY", "RSTR", "NKEY", "STEP", "INIT",
                               "INFO", "AUTH", "TSRV", "BCST", "CRYP"};

  const std::size_t count = sizeof(codes) / sizeof(codes[0]);
  for (std::size_t i = 0; i < count; ++i) {
    NTP_TEST_CHECK_STREQ(names[i], kiss_code_name(codes[i]));

    // The packed value must be the four ASCII characters, in order.
    const std::uint32_t expected = (static_cast<std::uint32_t>(names[i][0]) << 24) |
                                   (static_cast<std::uint32_t>(names[i][1]) << 16) |
                                   (static_cast<std::uint32_t>(names[i][2]) << 8) |
                                   static_cast<std::uint32_t>(names[i][3]);
    NTP_TEST_CHECK_EQ(expected, codes[i]);
  }
}

NTP_TEST(kiss, unknown_code_has_no_name) {
  NTP_TEST_CHECK_STREQ("", kiss_code_name(0x00000000UL));
  NTP_TEST_CHECK_STREQ("", kiss_code_name(0xDEADBEEFUL));
}

// ---------------------------------------------------------------------------
// Reference identifiers
// ---------------------------------------------------------------------------
NTP_TEST(reference_id, renders_printable_ascii) {
  char text[5];

  reference_id_text(kiss_rate, text);
  NTP_TEST_CHECK_STREQ("RATE", text);

  // A stratum 1 reference clock identifier, e.g. "GPS ".
  reference_id_text(0x47505320UL, text);
  NTP_TEST_CHECK_STREQ("GPS ", text);
}

NTP_TEST(reference_id, replaces_non_printable_octets) {
  char text[5];

  // An IPv4 address is not text: every octet below 0x20 becomes '.'.
  reference_id_text(0x0A000001UL, text);
  NTP_TEST_CHECK_STREQ("....", text);

  reference_id_text(0x00000000UL, text);
  NTP_TEST_CHECK_STREQ("....", text);
}

// ---------------------------------------------------------------------------
// Stratum helper
// ---------------------------------------------------------------------------
NTP_TEST(stratum, usable_range) {
  NTP_TEST_CHECK(!is_usable_stratum(0));  // Kiss-o'-Death
  NTP_TEST_CHECK(is_usable_stratum(1));   // primary server
  NTP_TEST_CHECK(is_usable_stratum(2));
  NTP_TEST_CHECK(is_usable_stratum(15));   // furthest usable
  NTP_TEST_CHECK(!is_usable_stratum(16));  // unsynchronized
  NTP_TEST_CHECK(!is_usable_stratum(255));
}

}  // namespace
