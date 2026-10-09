// ============================================================================
// ntplite - tests/odr/odr_unit_b.cpp
// ----------------------------------------------------------------------------
// Second translation unit.  Also includes <ntplite/ntplite.hpp>; if any
// symbol in the public headers is defined with external linkage but without
// `inline`, linking odr_unit_a.o and odr_unit_b.o together is a hard error.
// ============================================================================

#include <ntplite/ntplite.hpp>

#include "odr_shared.hpp"

namespace odr_test {

int unit_b_version_major() {
  return ntplite::version_major();
}

}  // namespace odr_test
