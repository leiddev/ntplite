// ============================================================================
// ntplite - examples/cpp_quickstart.cpp
// ----------------------------------------------------------------------------
// Minimal C++ integration example: one include, no extra translation unit.
//
// Nothing here changes this machine's clock: the offset is reported, not
// applied.
// ============================================================================

#include <cstdio>
#include <ntplite/ntplite.hpp>

int main() {
  ntplite::query_result result;
  const ntplite::error_code status = ntplite::query("pool.ntp.org", result);

  if (status != ntplite::error_code::ok) {
    std::printf("query failed: %s\n", ntplite::error_code_name(status));
    if (result.kiss_of_death) {
      std::printf("the server refused the request: %s\n", result.kiss_code.c_str());
    }
    return 1;
  }

  std::printf("offset: %+.6f s (%s)\n", result.offset_seconds(),
              result.offset_seconds() >= 0.0 ? "the local clock is behind"
                                             : "the local clock is ahead");
  std::printf("round trip delay: %.6f s\n", result.round_trip_delay_seconds());
  std::printf("stratum %u, %d request(s)\n", static_cast<unsigned>(result.stratum),
              result.attempts);
  return 0;
}
