/* ===========================================================================
 * ntplite - examples/c_quickstart.c
 * ---------------------------------------------------------------------------
 * Minimal C integration example: ask a server what time it is, and print the
 * answer.  Nothing here changes this machine's clock.
 *
 * Build it against the prebuilt library:
 *
 *     cc c_quickstart.c -lntplite -lstdc++ -o c_quickstart
 *
 * (or, inside CMake, `target_link_libraries(app PRIVATE ntplite::c)`).
 *
 * The C++ runtime appears in that link line because ntplite is implemented in
 * C++11 behind the C ABI.  A caller that would rather not ship a .c/.cpp split
 * can use the stb-style route instead: add ONE C++ translation unit containing
 *
 *     #define NTP_LITE_IMPLEMENTATION
 *     #include <ntplite/ntplite.h>
 *
 * and keep including <ntplite/ntplite.h> from C sources as usual.
 * ===========================================================================
 */

#include <ntplite/ntplite.h>
#include <stdio.h>

int main(void) {
  ntplite_options_t options;
  ntplite_options_init(&options);
  options.total_timeout_ms = 2000; /* give up after two seconds */

  ntplite_result_t result;
  ntplite_result_init(&result);

  const ntplite_status_t status = ntplite_query("pool.ntp.org", &options, &result);
  if (status != NTP_LITE_OK) {
    fprintf(stderr, "query failed: %s\n", ntplite_status_string(status));

    /* A refusal is worth more than a sentence: it says which server said it and
     * what it wanted.  Retrying at once is the one thing not to do. */
    if (result.kiss_of_death) {
      fprintf(stderr, "%s refused the request: %s\n", result.server, result.kiss_code);
    }
    return 1;
  }

  char when[NTPLITE_TIME_TEXT_SIZE];
  char offset[NTPLITE_SECONDS_TEXT_SIZE];
  ntplite_format_utc(&result.server_time, when, sizeof(when));
  ntplite_format_seconds(&result.offset, offset, sizeof(offset));

  printf("%s says the time is %s\n", result.server, when);
  printf("this machine's clock is %s seconds from it (stratum %d, %d request(s))\n", offset,
         result.stratum, result.attempts);
  return 0;
}
