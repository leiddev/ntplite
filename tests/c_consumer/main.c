/* ===========================================================================
 * ntplite - tests/c_consumer/main.c
 * ---------------------------------------------------------------------------
 * Proves that the public header is genuinely consumable from plain C and that
 * the prebuilt `ntplite::c` library exposes a usable ABI.
 *
 * This file must stay strictly C99: no `//` comments-only style requirements,
 * no C++ constructs, no casts of function pointers.
 * ===========================================================================
 */

#include <ntplite/ntplite.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char* version = ntplite_version_string();

  if (version == NULL || version[0] == '\0') {
    fprintf(stderr, "c_consumer: empty version string\n");
    return 1;
  }

  if (ntplite_c_api_version() != NTP_LITE_C_API_VERSION) {
    fprintf(stderr, "c_consumer: ABI mismatch, header %d vs library %d\n", NTP_LITE_C_API_VERSION,
            ntplite_c_api_version());
    return 1;
  }

  if (ntplite_version_major() < 0 || ntplite_version_minor() < 0 || ntplite_version_patch() < 0) {
    fprintf(stderr, "c_consumer: negative version component\n");
    return 1;
  }

  if (strcmp(ntplite_status_string(NTP_LITE_OK), "OK") != 0) {
    fprintf(stderr, "c_consumer: unexpected status text for NTP_LITE_OK\n");
    return 1;
  }

  printf("c_consumer: ok (ntplite %s, c abi %d)\n", version, ntplite_c_api_version());
  return 0;
}
