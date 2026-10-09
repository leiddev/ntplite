// ============================================================================
// ntplite - tests/odr/odr_main.cpp
// ----------------------------------------------------------------------------
// Drives the ODR test.  Links both translation units together.
// ============================================================================

#include <cstdio>

#include "odr_shared.hpp"

int main() {
  const int from_a = odr_test::unit_a_version_major();
  const int from_b = odr_test::unit_b_version_major();

  if (from_a != from_b) {
    std::fprintf(stderr, "odr: mismatched version_major() %d vs %d\n", from_a, from_b);
    return 1;
  }

  std::printf("odr: ok (version_major=%d)\n", from_a);
  return 0;
}
