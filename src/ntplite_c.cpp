// ============================================================================
// ntplite - src/ntplite_c.cpp
// ----------------------------------------------------------------------------
// The one and only compiled translation unit of the project.
//
// Its sole purpose is to give C (and other FFI) consumers a prebuilt,
// pre-instantiated library, `ntplite::c`, so that they never have to touch the
// stb-style NTP_LITE_IMPLEMENTATION macro themselves.
//
// C++ consumers do NOT need this file: <ntplite/ntplite.hpp> is fully inline.
// ============================================================================

#define NTP_LITE_IMPLEMENTATION 1
#include <ntplite/ntplite.h>
