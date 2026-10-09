// ============================================================================
// ntplite - detail/error.hpp
// ----------------------------------------------------------------------------
// The C++ side of the error taxonomy.
//
// It exists because <ntplite/ntplite.h> cannot be used from the library's own
// implementation: the C header is a *frozen* ABI surface, and its enumeration
// is unscoped, so every internal switch would have to spell NTP_LITE_ERR_...
// and every intermediate layer would depend on the C API.  Instead the
// implementation speaks `ntplite::error_code` and the C API is a one-line
// mapping.
//
// The two enumerations are kept numerically identical on purpose, and the
// static assertions below turn a future divergence into a compile error rather
// than a silently wrong status code crossing the ABI.
//
// Note the difference from the C enumeration fixed in 0.1.0: a *scoped* enum
// always has a fixed underlying type (`int` here, by default), so converting
// an arbitrary integer to it is well defined.  That is precisely what makes
// the mapping below safe in both directions.
// ============================================================================

#ifndef NTP_LITE_DETAIL_ERROR_HPP
#define NTP_LITE_DETAIL_ERROR_HPP

#include <ntplite/ntplite.h>

#include <ntplite/detail/config.hpp>

namespace ntplite {

/// Everything that can go wrong inside ntplite.
///
/// The values are part of the ABI contract with <ntplite/ntplite.h>; do not
/// renumber them.
enum class error_code {
  ok = 0,                ///< no error
  invalid_argument = 1,  ///< a caller supplied argument was rejected
  network_error = 2,     ///< socket create / send / receive failure
  resolve_error = 3,     ///< the host name could not be resolved
  timeout = 4,           ///< no reply within the requested timeout
  protocol_error = 5,    ///< malformed or unexpected NTP reply
  kiss_of_death = 6,     ///< the server sent a Kiss-o'-Death packet
  unsupported = 7,       ///< unsupported platform or configuration
  internal_error = 8     ///< an invariant inside ntplite was violated
};

// The numbering is load bearing: ntplite_status_t is what crosses the C ABI.
static_assert(static_cast<int>(error_code::ok) == NTP_LITE_OK,
              "error_code::ok must match NTP_LITE_OK");
static_assert(static_cast<int>(error_code::invalid_argument) == NTP_LITE_ERR_INVALID,
              "error_code::invalid_argument must match NTP_LITE_ERR_INVALID");
static_assert(static_cast<int>(error_code::network_error) == NTP_LITE_ERR_NETWORK,
              "error_code::network_error must match NTP_LITE_ERR_NETWORK");
static_assert(static_cast<int>(error_code::resolve_error) == NTP_LITE_ERR_RESOLVE,
              "error_code::resolve_error must match NTP_LITE_ERR_RESOLVE");
static_assert(static_cast<int>(error_code::timeout) == NTP_LITE_ERR_TIMEOUT,
              "error_code::timeout must match NTP_LITE_ERR_TIMEOUT");
static_assert(static_cast<int>(error_code::protocol_error) == NTP_LITE_ERR_PROTOCOL,
              "error_code::protocol_error must match NTP_LITE_ERR_PROTOCOL");
static_assert(static_cast<int>(error_code::kiss_of_death) == NTP_LITE_ERR_KOD,
              "error_code::kiss_of_death must match NTP_LITE_ERR_KOD");
static_assert(static_cast<int>(error_code::unsupported) == NTP_LITE_ERR_UNSUPPORTED,
              "error_code::unsupported must match NTP_LITE_ERR_UNSUPPORTED");
static_assert(static_cast<int>(error_code::internal_error) == NTP_LITE_ERR_INTERNAL,
              "error_code::internal_error must match NTP_LITE_ERR_INTERNAL");

/// Human readable name, for logs and test failures.  Never null.
inline const char* error_code_name(error_code code) NTP_LITE_NOEXCEPT {
  switch (code) {
    case error_code::ok:
      return "ok";
    case error_code::invalid_argument:
      return "invalid argument";
    case error_code::network_error:
      return "network error";
    case error_code::resolve_error:
      return "hostname resolution failed";
    case error_code::timeout:
      return "timed out";
    case error_code::protocol_error:
      return "protocol error";
    case error_code::kiss_of_death:
      return "kiss-o'-death";
    case error_code::unsupported:
      return "unsupported";
    case error_code::internal_error:
      return "internal error";
  }
  return "unknown error";
}

/// Maps an internal error onto the stable C status code.
inline ntplite_status_t to_status(error_code code) NTP_LITE_NOEXCEPT {
  return static_cast<ntplite_status_t>(static_cast<int>(code));
}

/// Maps a C status code back onto the internal taxonomy.
///
/// Values outside the enumeration are passed through unchanged rather than
/// clamped or treated as undefined behaviour - a status received from a
/// foreign FFI caller is just a number, and rejecting it is the caller's job.
inline error_code from_status(ntplite_status_t status) NTP_LITE_NOEXCEPT {
  return static_cast<error_code>(static_cast<int>(status));
}

}  // namespace ntplite

#endif  // NTP_LITE_DETAIL_ERROR_HPP
