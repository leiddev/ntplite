/* ===========================================================================
 * ntplite - examples/c_quickstart.c
 * ---------------------------------------------------------------------------
 * Minimal C integration example.
 *
 * Build it against the prebuilt library:
 *
 *     cc c_quickstart.c -lntplite -lstdc++ -o c_quickstart
 *
 * (or, inside CMake, `target_link_libraries(app PRIVATE ntplite::c)`).
 *
 * If you would rather avoid shipping a .c/.cpp split, use the stb-style route
 * instead: add ONE C++ translation unit to your project containing
 *
 *     #define NTP_LITE_IMPLEMENTATION
 *     #include <ntplite/ntplite.h>
 *
 * and keep including <ntplite/ntplite.h> from your C sources.
 * ===========================================================================
 */

#include <ntplite/ntplite.h>
#include <stdio.h>

int main(void) {
  printf("ntplite %s (C ABI %d)\n", ntplite_version_string(), ntplite_c_api_version());
  return 0;
}
