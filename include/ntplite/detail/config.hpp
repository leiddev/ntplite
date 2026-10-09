// ============================================================================
// ntplite - detail/config.hpp
// ----------------------------------------------------------------------------
// Platform / compiler detection and small portability helpers.
//
// This header is C++ only.  The public C API lives in <ntplite/ntplite.h>,
// which is deliberately self-contained and never includes this file.
//
// ntplite targets C++11 strictly: no C++14/17 facilities may be used anywhere
// in the library headers.
// ============================================================================

#ifndef NTP_LITE_DETAIL_CONFIG_HPP
#define NTP_LITE_DETAIL_CONFIG_HPP

// ---------------------------------------------------------------------------
// Compiler detection
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#define NTP_LITE_COMPILER_MSVC _MSC_VER
#elif defined(__clang__)
#define NTP_LITE_COMPILER_CLANG (__clang_major__ * 100 + __clang_minor__)
#elif defined(__GNUC__)
#define NTP_LITE_COMPILER_GCC (__GNUC__ * 100 + __GNUC_MINOR__)
#endif

// ---------------------------------------------------------------------------
// Language standard guard - fail loudly instead of with 500 template errors.
// ---------------------------------------------------------------------------
#if !defined(__cplusplus)
#error "ntplite headers require a C++ compiler; include <ntplite/ntplite.h> from C code."
#elif __cplusplus < 201103L && !defined(_MSC_VER)
#error "ntplite requires C++11 or newer (-std=c++11)."
#elif defined(_MSC_VER) && _MSC_VER < 1900
#error "ntplite requires Visual Studio 2015 (MSVC 19.00) or newer."
#endif

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------
#if defined(_WIN32) || defined(_WIN64)
#define NTP_LITE_PLATFORM_WINDOWS 1
#else
#define NTP_LITE_PLATFORM_WINDOWS 0
#endif

#if defined(__linux__)
#define NTP_LITE_PLATFORM_LINUX 1
#else
#define NTP_LITE_PLATFORM_LINUX 0
#endif

#if defined(__APPLE__)
#define NTP_LITE_PLATFORM_MACOS 1
#else
#define NTP_LITE_PLATFORM_MACOS 0
#endif

#if defined(unix) || defined(__unix__) || defined(__unix) || defined(NTP_LITE_PLATFORM_MACOS)
#define NTP_LITE_PLATFORM_POSIX 1
#else
#define NTP_LITE_PLATFORM_POSIX 0
#endif

#if !NTP_LITE_PLATFORM_WINDOWS && !NTP_LITE_PLATFORM_POSIX
#error "ntplite: unsupported platform (expected Windows, Linux or macOS)."
#endif

// ---------------------------------------------------------------------------
// Attribute helpers
// ---------------------------------------------------------------------------
#if defined(__cplusplus) && __cplusplus >= 201103L
#define NTP_LITE_NOEXCEPT  noexcept
#define NTP_LITE_CONSTEXPR constexpr
#elif defined(_MSC_VER) && _MSC_VER >= 1900
#define NTP_LITE_NOEXCEPT  noexcept
#define NTP_LITE_CONSTEXPR constexpr
#else
#define NTP_LITE_NOEXCEPT
#define NTP_LITE_CONSTEXPR
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NTP_LITE_DEPRECATED(msg) __attribute__((deprecated(msg)))
#define NTP_LITE_NODISCARD       __attribute__((warn_unused_result))
#elif defined(_MSC_VER)
#define NTP_LITE_DEPRECATED(msg) __declspec(deprecated(msg))
#define NTP_LITE_NODISCARD
#else
#define NTP_LITE_DEPRECATED(msg)
#define NTP_LITE_NODISCARD
#endif

/// Any symbol tagged with this macro is part of the implementation detail and
/// may change without notice.  In a -fvisibility=hidden build it would also be
/// hidden; ntplite is header-only, so this is documentation only.
#define NTP_LITE_DETAIL

// ---------------------------------------------------------------------------
// Feature toggles (overridable by the consumer before including ntplite.hpp)
// ---------------------------------------------------------------------------
/// Enable IPv6 support.  Requires a dual-stack capable resolver; on by default.
#ifndef NTP_LITE_ENABLE_IPV6
#define NTP_LITE_ENABLE_IPV6 1
#endif

/// Enable the millisecond/microsecond resolution API surface.
#ifndef NTP_LITE_ENABLE_SUBSECOND
#define NTP_LITE_ENABLE_SUBSECOND 1
#endif

#endif  // NTP_LITE_DETAIL_CONFIG_HPP
