// ============================================================================
// ntplite - tests/odr/odr_unit_a.cpp
// ----------------------------------------------------------------------------
// First of the two translation units that exercise the header-only API.
// ============================================================================

#include <ntplite/ntplite.hpp>

#include "odr_shared.hpp"

namespace odr_test {

int unit_a_version_major() {
  return ntplite::version_major();
}

}  // namespace odr_test
