// ============================================================================
// ntplite - tests/consumer/main.cpp
// ----------------------------------------------------------------------------
// Consumes an installed ntplite through the header-only C++ target.
// ============================================================================

#include <cstdio>
#include <ntplite/ntplite.hpp>

int main() {
  const char* version = ntplite::version_string();

  if (version == nullptr || version[0] == '\0') {
    std::fprintf(stderr, "consumer_cpp: empty version string\n");
    return 1;
  }

  if (ntplite::version_major() != NTP_LITE_VERSION_MAJOR) {
    std::fprintf(stderr, "consumer_cpp: macro/function mismatch\n");
    return 1;
  }

  std::printf("consumer_cpp: ok (ntplite %s)\n", version);
  return 0;
}
