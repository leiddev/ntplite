// ============================================================================
// ntplite - examples/cpp_quickstart.cpp
// ----------------------------------------------------------------------------
// Minimal C++ integration example: one include, no extra translation unit.
// ============================================================================

#include <cstdio>
#include <ntplite/ntplite.hpp>

int main() {
  std::printf("ntplite %s\n", ntplite::version_string());
  std::printf("  major: %d\n", ntplite::version_major());
  std::printf("  minor: %d\n", ntplite::version_minor());
  std::printf("  patch: %d\n", ntplite::version_patch());
  return 0;
}
