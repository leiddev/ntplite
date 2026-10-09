// ============================================================================
// ntplite - ntplite.hpp
// ----------------------------------------------------------------------------
// The single umbrella header for C++ consumers:
//
//     #include <ntplite/ntplite.hpp>
//
// Everything in ntplite is header-only and inline, so no additional
// translation unit and no extra build step are required.  On Windows the
// Winsock library must be linked (`ws2_32`); the exported CMake target
// `ntplite::ntplite` takes care of that for you.
// ============================================================================

#ifndef NTP_LITE_NTP_LITE_HPP
#define NTP_LITE_NTP_LITE_HPP

#include <ntplite/detail/config.hpp>
#include <ntplite/detail/ntp_time.hpp>
#include <ntplite/time.hpp>
#include <ntplite/version.hpp>

namespace ntplite {

/// \name Version information
/// Mirrors the C API in <ntplite/ntplite.h>.
/// @{

/// Human readable version string, e.g. "0.1.0".
inline const char* version_string() NTP_LITE_NOEXCEPT {
  return NTP_LITE_VERSION_STRING;
}

/// Major version component.
inline int version_major() NTP_LITE_NOEXCEPT {
  return NTP_LITE_VERSION_MAJOR;
}

/// Minor version component.
inline int version_minor() NTP_LITE_NOEXCEPT {
  return NTP_LITE_VERSION_MINOR;
}

/// Patch version component.
inline int version_patch() NTP_LITE_NOEXCEPT {
  return NTP_LITE_VERSION_PATCH;
}

/// @}

}  // namespace ntplite

#endif  // NTP_LITE_NTP_LITE_HPP
