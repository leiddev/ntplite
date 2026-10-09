// ============================================================================
// ntplite - version.hpp
// ----------------------------------------------------------------------------
// Compile-time version information.
//
// Keep NTP_LITE_VERSION_* in sync with `project(ntplite VERSION x.y.z)` in the
// top-level CMakeLists.txt.  CI enforces this with
// `scripts/check_version_sync.py`.
// ============================================================================

#ifndef NTP_LITE_VERSION_HPP
#define NTP_LITE_VERSION_HPP

#define NTP_LITE_VERSION_MAJOR 0
#define NTP_LITE_VERSION_MINOR 1
#define NTP_LITE_VERSION_PATCH 0

/// Human readable version, e.g. "0.1.0".
#define NTP_LITE_VERSION_STRING "0.1.0"

/// Single integer suitable for preprocessor comparisons:
///   #if NTP_LITE_VERSION_NUMBER >= 10200  // 1.2.0 or newer
#define NTP_LITE_VERSION_NUMBER \
  (NTP_LITE_VERSION_MAJOR * 10000 + NTP_LITE_VERSION_MINOR * 100 + NTP_LITE_VERSION_PATCH)

#endif  // NTP_LITE_VERSION_HPP
