/* ===========================================================================
 * ntplite - tests/consumer/main.c
 * ---------------------------------------------------------------------------
 * Consumes an installed ntplite through the prebuilt C ABI target.
 * ===========================================================================
 */

#include <ntplite/ntplite.h>
#include <stdio.h>

int main(void) {
  const char* version = ntplite_version_string();

  if (version == NULL || version[0] == '\0') {
    fprintf(stderr, "consumer_c: empty version string\n");
    return 1;
  }

  if (ntplite_c_api_version() != NTP_LITE_C_API_VERSION) {
    fprintf(stderr, "consumer_c: ABI mismatch\n");
    return 1;
  }

  printf("consumer_c: ok (ntplite %s)\n", version);
  return 0;
}
