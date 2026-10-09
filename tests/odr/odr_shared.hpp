// ============================================================================
// ntplite - tests/odr/odr_shared.hpp
// ----------------------------------------------------------------------------
// Shared declaration for the ODR test.  Both odr_unit_a.cpp and odr_unit_b.cpp
// include <ntplite/ntplite.hpp> and then link together into one binary.  A
// header-only library that ODR-violates (non-inline function, non-inline
// global, ...) fails here at link time.
// ============================================================================

#ifndef NTP_LITE_TESTS_ODR_SHARED_HPP
#define NTP_LITE_TESTS_ODR_SHARED_HPP

namespace odr_test {

// Implemented in odr_unit_a.cpp and odr_unit_b.cpp respectively.
int unit_a_version_major();
int unit_b_version_major();

}  // namespace odr_test

#endif  // NTP_LITE_TESTS_ODR_SHARED_HPP
