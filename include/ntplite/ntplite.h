/* ===========================================================================
 * ntplite - ntplite.h
 * ---------------------------------------------------------------------------
 * Stable, C-compatible public API for ntplite.
 *
 * ntplite is implemented in C++11 but exposes a flat `extern "C"` ABI, so this
 * header can be consumed from plain C, C++, or any language with C FFI support
 * (Rust, Python ctypes, Go cgo, ...).
 *
 * ---------------------------------------------------------------------------
 * How to use it
 * ---------------------------------------------------------------------------
 * Option A - prebuilt library (recommended; nothing to remember):
 *
 *     target_link_libraries(my_app PRIVATE ntplite::c)
 *     #include <ntplite/ntplite.h>
 *
 * Option B - header-only, stb style.  In EXACTLY ONE C++ translation unit:
 *
 *     #define NTP_LITE_IMPLEMENTATION
 *     #include <ntplite/ntplite.h>
 *
 *   Every other translation unit (C or C++) just includes the header normally.
 *
 *   Because the implementation is C++ you cannot compile the implementation
 *   unit with a C compiler; the `#error` below makes that explicit.
 *
 * Option C - C++ consumers should prefer <ntplite/ntplite.hpp> instead, which
 * is fully inline and needs no implementation macro at all.
 *
 * ---------------------------------------------------------------------------
 * Never mix option A with option B: doing so defines every symbol twice and
 * you will get a linker error ("duplicate symbol" / LNK2005).
 * ===========================================================================
 */

#ifndef NTP_LITE_NTP_LITE_H
#define NTP_LITE_NTP_LITE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * API / ABI versioning
 * ---------------------------------------------------------------------------
 * NTP_LITE_C_API_VERSION is bumped whenever the C ABI changes in an
 * incompatible way.  Call ntplite_c_api_version() at runtime to detect a
 * mismatch between the header you compiled against and the library you linked.
 */
#define NTP_LITE_C_API_VERSION 1

/* ---------------------------------------------------------------------------
 * Status codes
 * ---------------------------------------------------------------------------
 * The numeric values are spelled out explicitly because they are part of the
 * ABI and must never move.
 *
 * The underlying type is pinned to `int` in C++ only.  Without that, C++ would
 * give the enumeration the narrowest bit-field capable of holding the
 * enumerators (0..8 -> 4 bits), and storing anything outside that range -
 * which happens routinely when the value arrives from C, from an FFI binding,
 * or from a corrupted buffer - would be undefined behaviour on load.  C
 * already treats enumeration objects as int-sized, so both declarations agree
 * on every supported compiler.
 */
#if defined(__cplusplus)
typedef enum ntplite_status : int {
#else
typedef enum ntplite_status {
#endif
  NTP_LITE_OK = 0,              /* success                                */
  NTP_LITE_ERR_INVALID = 1,     /* invalid argument                       */
  NTP_LITE_ERR_NETWORK = 2,     /* socket create / send / recv failure    */
  NTP_LITE_ERR_RESOLVE = 3,     /* hostname could not be resolved         */
  NTP_LITE_ERR_TIMEOUT = 4,     /* no reply within the requested timeout  */
  NTP_LITE_ERR_PROTOCOL = 5,    /* malformed / unexpected NTP reply       */
  NTP_LITE_ERR_KOD = 6,         /* server sent a Kiss-o'-Death packet     */
  NTP_LITE_ERR_UNSUPPORTED = 7, /* unsupported platform or configuration  */
  NTP_LITE_ERR_INTERNAL = 8     /* unexpected internal failure            */
} ntplite_status_t;

/* Returns the ABI revision this library was built with. */
int ntplite_c_api_version(void);

/* Human readable library version, e.g. "0.1.0".  Never NULL. */
const char* ntplite_version_string(void);

/* Individual version components. */
int ntplite_version_major(void);
int ntplite_version_minor(void);
int ntplite_version_patch(void);

/* Human readable description of a status code.  Never NULL. */
const char* ntplite_status_string(ntplite_status_t status);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ===========================================================================
 * Optional implementation block (option B above)
 * ===========================================================================
 */
#if defined(NTP_LITE_IMPLEMENTATION)

#if !defined(__cplusplus)
#error \
    "NTP_LITE_IMPLEMENTATION requires a C++ compiler: ntplite is implemented in C++11. Use option A (link ntplite::c) from C instead."
#endif

#include <ntplite/detail/config.hpp>
#include <ntplite/ntplite.hpp>

extern "C" {

int ntplite_c_api_version(void) {
  return NTP_LITE_C_API_VERSION;
}

const char* ntplite_version_string(void) {
  return ntplite::version_string();
}

int ntplite_version_major(void) {
  return ntplite::version_major();
}

int ntplite_version_minor(void) {
  return ntplite::version_minor();
}

int ntplite_version_patch(void) {
  return ntplite::version_patch();
}

const char* ntplite_status_string(ntplite_status_t status) {
  switch (status) {
    case NTP_LITE_OK:
      return "OK";
    case NTP_LITE_ERR_INVALID:
      return "invalid argument";
    case NTP_LITE_ERR_NETWORK:
      return "network error";
    case NTP_LITE_ERR_RESOLVE:
      return "hostname resolution failed";
    case NTP_LITE_ERR_TIMEOUT:
      return "operation timed out";
    case NTP_LITE_ERR_PROTOCOL:
      return "protocol error";
    case NTP_LITE_ERR_KOD:
      return "server sent a Kiss-o'-Death packet";
    case NTP_LITE_ERR_UNSUPPORTED:
      return "unsupported platform or configuration";
    case NTP_LITE_ERR_INTERNAL:
      return "internal error";
  }
  return "unknown status";
}

} /* extern "C" */

#endif /* NTP_LITE_IMPLEMENTATION */

#endif /* NTP_LITE_NTP_LITE_H */
