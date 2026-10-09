// ============================================================================
// ntplite - detail/packet.hpp
// ----------------------------------------------------------------------------
// NTPv4 packet encoding and decoding (RFC 5905 section 7.3).
//
// The fixed part of an NTP packet is exactly 48 bytes, all fields in network
// byte order:
//
//     0                   1                   2                   3
//     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |LI | VN  |Mode |    Stratum    |     Poll      |   Precision   |  4
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                         Root Delay                            |  8
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                      Root Dispersion                          | 12
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                     Reference Identifier                      | 16
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                   Reference Timestamp (64 bits)               | 24
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                    Origin Timestamp (64 bits)                 | 32
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                   Receive Timestamp (64 bits)                 | 40
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                  Transmit Timestamp (64 bits)                 | 48
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//
// The design separates two concerns on purpose:
//
//   * encode_packet / decode_packet are purely mechanical.  Decoding never
//     fails for any 48 byte input: every bit pattern is representable, so a
//     malformed packet is a *semantic* problem, not a decoding problem.
//   * validate_reply applies the protocol rules, and is where "that is not an
//     answer to my question" is decided.  Keeping it separate is what makes
//     both halves easy to test with hand built byte arrays.
//
// Optional extension fields and MACs beyond byte 48 are ignored: ntplite is a
// plain unauthenticated client, so there is nothing useful past the header.
// ============================================================================

#ifndef NTP_LITE_DETAIL_PACKET_HPP
#define NTP_LITE_DETAIL_PACKET_HPP

#include <ntplite/ntplite.h>

#include <cstddef>
#include <cstdint>
#include <ntplite/detail/config.hpp>
#include <ntplite/detail/ntp_time.hpp>

namespace ntplite {
namespace detail {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Size of the fixed NTP header, in bytes.
const std::size_t packet_size = 48;

/// Largest value the 4 bit mode field can hold.
const std::uint8_t max_mode = 7;

/// Protocol versions ntplite understands on the wire.
const std::uint8_t version_3 = 3;
const std::uint8_t version_4 = 4;

/// Leap indicator values.
namespace leap {
const std::uint8_t no_warning = 0;              ///< no leap second pending
const std::uint8_t last_minute_61_seconds = 1;  ///< a leap second is inserted
const std::uint8_t last_minute_59_seconds = 2;  ///< a leap second is deleted
const std::uint8_t unsynchronized = 3;          ///< server clock is not set
}  // namespace leap

/// Association modes.
namespace mode {
const std::uint8_t reserved = 0;
const std::uint8_t symmetric_active = 1;
const std::uint8_t symmetric_passive = 2;
const std::uint8_t client = 3;
const std::uint8_t server = 4;
const std::uint8_t broadcast = 5;
const std::uint8_t control = 6;
const std::uint8_t private_use = 7;
}  // namespace mode

/// Stratum values with a special meaning.
const std::uint8_t stratum_kiss_of_death = 0;
const std::uint8_t stratum_unsynchronized = 16;

/// Default `poll` and `precision` a client advertises.
///
/// Neither field is used by the server when answering a client, so these are
/// advisory.  `poll` is log2 of the requested poll interval, clamped by
/// RFC 5905 to [4, 17]; we ask for 8 s.  `precision` is log2 of the local clock
/// resolution, and -20 stands for roughly one microsecond.
const std::int8_t default_poll = 3;
const std::int8_t default_precision = -20;

// ---------------------------------------------------------------------------
// Big endian primitives
// ---------------------------------------------------------------------------

inline std::uint16_t read_u16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                    static_cast<std::uint16_t>(data[1]));
}

inline std::uint32_t read_u32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

inline void write_u16(std::uint8_t* data, std::uint16_t value) {
  data[0] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
  data[1] = static_cast<std::uint8_t>(value & 0xFFU);
}

inline void write_u32(std::uint8_t* data, std::uint32_t value) {
  data[0] = static_cast<std::uint8_t>((value >> 24) & 0xFFU);
  data[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFU);
  data[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFU);
  data[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

/// Widens an unsigned octet to a signed one without relying on the
/// implementation defined conversion that C++11 would otherwise perform.
inline std::int8_t narrow_int8(std::uint8_t value) {
  return value < 128U ? static_cast<std::int8_t>(value)
                      : static_cast<std::int8_t>(static_cast<int>(value) - 256);
}

/// Rounds a signed octet back to its unsigned representation.
inline std::uint8_t widen_int8(std::int8_t value) {
  return static_cast<std::uint8_t>(value);
}

inline ntp_timestamp read_ntp_timestamp(const std::uint8_t* data) {
  ntp_timestamp value;
  value.seconds = read_u32(data);
  value.fraction = read_u32(data + 4);
  return value;
}

inline void write_ntp_timestamp(std::uint8_t* data, const ntp_timestamp& value) {
  write_u32(data, value.seconds);
  write_u32(data + 4, value.fraction);
}

/// Packs a 16.16 value into the 32 bit representation used on the wire.
inline std::uint32_t pack_short(const ntp_short& value) {
  return (static_cast<std::uint32_t>(value.seconds) << 16) |
         static_cast<std::uint32_t>(value.fraction);
}

/// Unpacks a 32 bit wire value into its 16.16 halves.
inline ntp_short unpack_short(std::uint32_t value) {
  ntp_short result;
  result.seconds = static_cast<std::uint16_t>((value >> 16) & 0xFFFFU);
  result.fraction = static_cast<std::uint16_t>(value & 0xFFFFU);
  return result;
}

// ---------------------------------------------------------------------------
// The li | vn | mode octet
// ---------------------------------------------------------------------------

inline std::uint8_t make_flags(std::uint8_t leap_indicator, std::uint8_t version,
                               std::uint8_t association_mode) {
  return static_cast<std::uint8_t>(((leap_indicator & 0x03U) << 6) | ((version & 0x07U) << 3) |
                                   (association_mode & 0x07U));
}

inline std::uint8_t flags_leap(std::uint8_t flags) {
  return static_cast<std::uint8_t>((flags >> 6) & 0x03U);
}

inline std::uint8_t flags_version(std::uint8_t flags) {
  return static_cast<std::uint8_t>((flags >> 3) & 0x07U);
}

inline std::uint8_t flags_mode(std::uint8_t flags) {
  return static_cast<std::uint8_t>(flags & 0x07U);
}

// ---------------------------------------------------------------------------
// The packet
// ---------------------------------------------------------------------------

/// The fixed 48 byte NTP header, in host representation.
struct packet {
  std::uint8_t leap;      ///< leap indicator, 0..3
  std::uint8_t version;   ///< protocol version, 3 or 4
  std::uint8_t mode;      ///< association mode, 0..7
  std::uint8_t stratum;   ///< 0 = KoD, 1..15 = distance from a reference clock
  std::int8_t poll;       ///< log2 of the poll interval, in seconds
  std::int8_t precision;  ///< log2 of the clock precision, in seconds

  ntp_short root_delay;        ///< 16.16, total round trip to the reference clock
  ntp_short root_dispersion;   ///< 16.16, total dispersion to the reference clock
  std::uint32_t reference_id;  ///< meaning depends on the stratum

  ntp_timestamp reference;  ///< when the server last set its clock
  ntp_timestamp origin;     ///< transmit timestamp of the request we sent
  ntp_timestamp receive;    ///< when the server received our request
  ntp_timestamp transmit;   ///< when the server sent this reply
};

/// A packet with every field zeroed, ready to be filled in.
inline packet empty_packet() {
  packet value;
  value.leap = leap::no_warning;
  value.version = version_4;
  value.mode = mode::reserved;
  value.stratum = 0;
  value.poll = 0;
  value.precision = 0;

  value.root_delay.seconds = 0;
  value.root_delay.fraction = 0;
  value.root_dispersion.seconds = 0;
  value.root_dispersion.fraction = 0;
  value.reference_id = 0;

  value.reference.seconds = 0;
  value.reference.fraction = 0;
  value.origin.seconds = 0;
  value.origin.fraction = 0;
  value.receive.seconds = 0;
  value.receive.fraction = 0;
  value.transmit.seconds = 0;
  value.transmit.fraction = 0;

  return value;
}

/// Builds a client mode request.
///
/// The four timestamps are zero except `transmit`, which carries the moment
/// the request left the client.  The server is required to echo it back in the
/// reply's origin field, which is how a client matches a reply to its request.
inline packet make_client_request(const ntp_timestamp& transmit, std::uint8_t version = version_4,
                                  std::int8_t poll = default_poll,
                                  std::int8_t precision = default_precision) {
  packet value = empty_packet();
  value.version = version;
  value.mode = mode::client;
  value.poll = poll;
  value.precision = precision;
  value.transmit = transmit;
  return value;
}

/// Serialises a packet into exactly `packet_size` bytes.
inline void encode_packet(const packet& value, std::uint8_t* out) {
  out[0] = make_flags(value.leap, value.version, value.mode);
  out[1] = value.stratum;
  out[2] = widen_int8(value.poll);
  out[3] = widen_int8(value.precision);

  write_u32(out + 4, pack_short(value.root_delay));
  write_u32(out + 8, pack_short(value.root_dispersion));
  write_u32(out + 12, value.reference_id);

  write_ntp_timestamp(out + 16, value.reference);
  write_ntp_timestamp(out + 24, value.origin);
  write_ntp_timestamp(out + 32, value.receive);
  write_ntp_timestamp(out + 40, value.transmit);
}

/// Deserialises a packet.
///
/// `size` is the number of bytes actually received; anything beyond the fixed
/// header is ignored.  Returns NTP_LITE_ERR_INVALID when the buffer is null or
/// shorter than a header.
inline ntplite_status_t decode_packet(const std::uint8_t* data, std::size_t size, packet& out) {
  if (data == NULL || size < packet_size) {
    return NTP_LITE_ERR_INVALID;
  }

  const std::uint8_t flags = data[0];

  out.leap = flags_leap(flags);
  out.version = flags_version(flags);
  out.mode = flags_mode(flags);
  out.stratum = data[1];
  out.poll = narrow_int8(data[2]);
  out.precision = narrow_int8(data[3]);

  out.root_delay = unpack_short(read_u32(data + 4));
  out.root_dispersion = unpack_short(read_u32(data + 8));
  out.reference_id = read_u32(data + 12);

  out.reference = read_ntp_timestamp(data + 16);
  out.origin = read_ntp_timestamp(data + 24);
  out.receive = read_ntp_timestamp(data + 32);
  out.transmit = read_ntp_timestamp(data + 40);

  return NTP_LITE_OK;
}

// ---------------------------------------------------------------------------
// Kiss-o'-Death
// ---------------------------------------------------------------------------

/// Four character reference identifiers used by Kiss-o'-Death packets, packed
/// as they appear on the wire.
const std::uint32_t kiss_rate = 0x52415445U;  ///< "RATE" - rate exceeded
const std::uint32_t kiss_deny = 0x44454E59U;  ///< "DENY" - access denied
const std::uint32_t kiss_rstr = 0x52535452U;  ///< "RSTR" - access restricted
const std::uint32_t kiss_nkey = 0x4E4B4559U;  ///< "NKEY" - bad or missing key
const std::uint32_t kiss_step = 0x53544550U;  ///< "STEP" - step, not slew
const std::uint32_t kiss_init = 0x494E4954U;  ///< "INIT" - association reset
const std::uint32_t kiss_info = 0x494E464FU;  ///< "INFO" - informational
const std::uint32_t kiss_auth = 0x41555448U;  ///< "AUTH" - authentication failure
const std::uint32_t kiss_tsrv = 0x54535256U;  ///< "TSRV" - no longer reserved
const std::uint32_t kiss_bcst = 0x42435354U;  ///< "BCST" - broadcast denied
const std::uint32_t kiss_cryp = 0x43525950U;  ///< "CRYP" - crypto unavailable

/// True when the reply is a Kiss-o'-Death: stratum 0 with an ASCII reference
/// identifier.
inline bool is_kiss_of_death(const packet& value) {
  return value.stratum == stratum_kiss_of_death;
}

/// Human readable name for a Kiss-o'-Death code, or "" when it is not one we
/// recognise.  The string is never null.
inline const char* kiss_code_name(std::uint32_t code) {
  switch (code) {
    case kiss_rate:
      return "RATE";
    case kiss_deny:
      return "DENY";
    case kiss_rstr:
      return "RSTR";
    case kiss_nkey:
      return "NKEY";
    case kiss_step:
      return "STEP";
    case kiss_init:
      return "INIT";
    case kiss_info:
      return "INFO";
    case kiss_auth:
      return "AUTH";
    case kiss_tsrv:
      return "TSRV";
    case kiss_bcst:
      return "BCST";
    case kiss_cryp:
      return "CRYP";
    default:
      return "";
  }
}

/// Writes the four octets of a reference identifier as text.
///
/// `out` must have room for five bytes.  Octets that are not printable ASCII
/// become '.', so the result is always safe to print.
inline void reference_id_text(std::uint32_t reference_id, char* out) {
  const std::uint32_t masks[4] = {0xFF000000U, 0x00FF0000U, 0x0000FF00U, 0x000000FFU};
  for (int i = 0; i < 4; ++i) {
    const std::uint8_t octet = static_cast<std::uint8_t>((reference_id & masks[i]) >> (24 - 8 * i));
    out[i] = (octet >= 0x20U && octet <= 0x7EU) ? static_cast<char>(octet) : '.';
  }
  out[4] = '\0';
}

// ---------------------------------------------------------------------------
// Semantic validation
// ---------------------------------------------------------------------------

/// The stratum is the distance to a reference clock: 1 is a primary server,
/// 15 the furthest usable, 16 means unsynchronized and 0 is a
/// Kiss-o'-Death.
inline bool is_usable_stratum(std::uint8_t stratum) {
  return stratum >= 1U && stratum < stratum_unsynchronized;
}

/// Checks whether `reply` is a well formed answer to `request`.
///
/// This is where the protocol rules live, kept apart from the pure decoding
/// so that each can be tested independently.  The checks are, in order:
///
///   1. the mode must be a server mode (4) or a broadcast (5);
///   2. the version must be one we speak (3 or 4);
///   3. a stratum of 0 is a Kiss-o'-Death and is reported as NTP_LITE_ERR_KOD;
///   4. a leap indicator of 3 means the server's clock is not set;
///   5. the stratum must be in 1..15;
///   6. the origin timestamp must echo the request's transmit timestamp,
///      which is what binds a reply to a request and defeats off-path
///      spoofing by a blind attacker;
///   7. the transmit timestamp must not be zero, otherwise the round trip
///      delay would be meaningless.
inline ntplite_status_t validate_reply(const packet& reply, const packet& request) {
  if (reply.mode != mode::server && reply.mode != mode::broadcast) {
    return NTP_LITE_ERR_PROTOCOL;
  }

  if (reply.version != version_3 && reply.version != version_4) {
    return NTP_LITE_ERR_PROTOCOL;
  }

  if (is_kiss_of_death(reply)) {
    return NTP_LITE_ERR_KOD;
  }

  if (reply.leap == leap::unsynchronized) {
    return NTP_LITE_ERR_PROTOCOL;
  }

  if (!is_usable_stratum(reply.stratum)) {
    return NTP_LITE_ERR_PROTOCOL;
  }

  if (reply.origin.seconds != request.transmit.seconds ||
      reply.origin.fraction != request.transmit.fraction) {
    return NTP_LITE_ERR_PROTOCOL;
  }

  if (reply.transmit.seconds == 0U && reply.transmit.fraction == 0U) {
    return NTP_LITE_ERR_PROTOCOL;
  }

  return NTP_LITE_OK;
}

}  // namespace detail
}  // namespace ntplite

#endif  // NTP_LITE_DETAIL_PACKET_HPP
