// ============================================================================
// ntplite - tools/ntplite_cli.cpp
// ----------------------------------------------------------------------------
// The `ntplite` command line tool.
//
// It is written against the C API - the same header a C, Rust or Python caller
// would use - and not against the C++ one.  That keeps the C ABI honest: if this
// file compiles and runs, the flat `extern "C"` surface really is enough to
// build a program out of, and is not merely a formality kept for show.
// ============================================================================

#include <ntplite/ntplite.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const char* const kDefaultServer = "pool.ntp.org";

/// Everything the command line can ask for.
struct settings {
  settings();

  const char* server; /* the name as typed, for the report */
  ntplite_options_t options;
  int quiet;       /* print only the server's time   */
  int offset_only; /* print only the offset          */
  int json;        /* print the whole result as JSON */
  int done;        /* --help or --version already answered */
};

settings::settings()
    : server(kDefaultServer), options(), quiet(0), offset_only(0), json(0), done(0) {
  ntplite_options_init(&options);
}

void print_usage(const char* program) {
  std::printf(
      "ntplite %s - a tiny NTP client\n"
      "\n"
      "Reports what a server says the time is, and how far this machine's clock\n"
      "is from it.  The local clock is never modified: the offset is printed,\n"
      "never applied.\n"
      "\n"
      "usage: %s [options] [server]\n"
      "\n"
      "options:\n"
      "  -h, --help             show this message and exit\n"
      "  -V, --version          print the library version and exit\n"
      "\n"
      "  -4                     use IPv4 only\n"
      "  -6                     use IPv6 only\n"
      "  -p, --port PORT        server port (default 123)\n"
      "      --ntp-version N    protocol version to speak, 3 or 4 (default 4)\n"
      "\n"
      "  -t, --timeout MS       budget for one server (default 1000)\n"
      "  -T, --total-timeout MS budget for the whole query (default 5000)\n"
      "      --retry-interval MS wait before resending to a silent server\n"
      "                         (default 400)\n"
      "\n"
      "  -q, --quiet            print only the server's time, ISO 8601 UTC\n"
      "  -o, --offset           print only the offset, in seconds\n"
      "      --json             print the whole result as one JSON object\n"
      "\n"
      "server defaults to %s\n"
      "\n"
      "exit codes: 0 success, 1 the query failed, 2 the command line was wrong\n",
      ntplite_version_string(), program, kDefaultServer);
}

void print_version() {
  std::printf("ntplite %s (C API %d)\n", ntplite_version_string(), ntplite_c_api_version());
}

/// Reports a bad option value and returns the usage exit code.
int fail_value(const char* program, const char* option, const char* value, const char* expected) {
  std::fprintf(stderr, "%s: %s expects %s, got '%s'\n", program, option, expected, value);
  std::fprintf(stderr, "run '%s --help' for the full list\n", program);
  return 2;
}

/// Reads a whole number.  Returns 0 on success and leaves `*out` untouched
/// otherwise; nothing is accepted but digits and an optional leading sign, so
/// "12abc" is a mistake rather than a 12.
int parse_number(const char* text, long long* out) {
  if (text == NULL || text[0] == '\0') {
    return -1;
  }

  char* end = NULL;
  const long long value = std::strtoll(text, &end, 10);
  if (end == text || *end != '\0') {
    return -1;
  }

  *out = value;
  return 0;
}

/// Moves `index` on to the value of an option that takes one.
int take_value(int argc, char** argv, int& index, const char** value) {
  if (index + 1 >= argc) {
    std::fprintf(stderr, "%s: option %s needs a value\n", argv[0], argv[index]);
    return 2;
  }
  ++index;
  *value = argv[index];
  return 0;
}

/// A duration as a plain decimal number, for the JSON report.
///
/// Built from the integer fields rather than from a double, so nothing is
/// rounded on the way out: a nanosecond is exactly nine decimals, and the offset
/// is the one number in the whole output that a caller may act on.  JSON has no
/// leading plus, so the sign that ntplite_format_seconds() always prints is
/// dropped when it is positive.
void json_seconds(const ntplite_duration_t& value, char* out, std::size_t size) {
  if (size == 0) {
    return;
  }
  ntplite_format_seconds(&value, out, size);
  if (out[0] == '+') {
    std::memmove(out, out + 1, std::strlen(out));
  }
}

/// True when `text` is one of the options that takes a value.
int takes_a_value(const char* arg) {
  return std::strcmp(arg, "-p") == 0 || std::strcmp(arg, "--port") == 0 ||
         std::strcmp(arg, "--ntp-version") == 0 || std::strcmp(arg, "-t") == 0 ||
         std::strcmp(arg, "--timeout") == 0 || std::strcmp(arg, "-T") == 0 ||
         std::strcmp(arg, "--total-timeout") == 0 || std::strcmp(arg, "--retry-interval") == 0;
}

/// Parses the command line.  Returns 0 to carry on, or an exit code.
int parse_arguments(int argc, char** argv, settings& config) {
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];

    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      print_usage(argv[0]);
      config.done = 1;
      return 0;
    }
    if (std::strcmp(arg, "-V") == 0 || std::strcmp(arg, "--version") == 0) {
      print_version();
      config.done = 1;
      return 0;
    }

    if (takes_a_value(arg)) {
      const char* text = NULL;
      const int taken = take_value(argc, argv, i, &text);
      if (taken != 0) {
        return taken;
      }

      long long number = 0;
      if (parse_number(text, &number) != 0) {
        return fail_value(argv[0], arg, text, "a whole number");
      }

      if (std::strcmp(arg, "-p") == 0 || std::strcmp(arg, "--port") == 0) {
        if (number < 1 || number > 65535) {
          return fail_value(argv[0], arg, text, "a port from 1 to 65535");
        }
        config.options.port = static_cast<uint16_t>(number);
      } else if (std::strcmp(arg, "--ntp-version") == 0) {
        if (number != 3 && number != 4) {
          return fail_value(argv[0], arg, text, "3 or 4");
        }
        config.options.ntp_version = static_cast<int>(number);
      } else if (std::strcmp(arg, "-t") == 0 || std::strcmp(arg, "--timeout") == 0) {
        if (number < 1) {
          return fail_value(argv[0], arg, text, "a positive number of milliseconds");
        }
        config.options.server_timeout_ms = number;
      } else if (std::strcmp(arg, "-T") == 0 || std::strcmp(arg, "--total-timeout") == 0) {
        if (number < 1) {
          return fail_value(argv[0], arg, text, "a positive number of milliseconds");
        }
        config.options.total_timeout_ms = number;
      } else if (number < 0) {
        return fail_value(argv[0], arg, text, "a number of milliseconds");
      } else {
        config.options.retry_interval_ms = number;
      }
      continue;
    }

    if (std::strcmp(arg, "-4") == 0) {
      config.options.ip_version = 4;
      continue;
    }
    if (std::strcmp(arg, "-6") == 0) {
      config.options.ip_version = 6;
      continue;
    }
    if (std::strcmp(arg, "-q") == 0 || std::strcmp(arg, "--quiet") == 0) {
      config.quiet = 1;
      continue;
    }
    if (std::strcmp(arg, "-o") == 0 || std::strcmp(arg, "--offset") == 0) {
      config.offset_only = 1;
      continue;
    }
    if (std::strcmp(arg, "--json") == 0) {
      config.json = 1;
      continue;
    }

    /* Anything else that starts with a dash is a mistake; a bare word is the
     * server.  A name beginning with '-' would need './' or the -- form, which
     * is the usual convention. */
    if (arg[0] == '-') {
      std::fprintf(stderr, "%s: unknown option '%s'\n", argv[0], arg);
      std::fprintf(stderr, "run '%s --help' for the full list\n", argv[0]);
      return 2;
    }
    config.server = arg;
  }

  return 0;
}

/// Renders the server's time as ISO 8601 UTC, or an empty string when the
/// result does not describe a time after all.
void time_text(const ntplite_result_t& result, char* out, std::size_t size) {
  if (size == 0) {
    return;
  }
  if (!result.valid) {
    /* A zeroed timestamp is 1970, which is a time but not *the* time.  Better to
     * say nothing than to print an epoch that was never observed. */
    out[0] = '\0';
    return;
  }
  ntplite_format_utc(&result.server_time, out, size);
}

/// Which way the local clock is wrong, in words.
///
/// Reported next to the offset because a signed number is easy to read the
/// wrong way round, and the whole point of the exercise is to know which clock
/// is ahead.
const char* offset_note(const ntplite_duration_t& offset) {
  if (offset.seconds == 0 && offset.nanoseconds == 0) {
    return "(already in agreement)";
  }
  /* `nanoseconds` is unsigned, so a negative duration is the one with a
   * negative whole part; a value of -1 ns is {-1, 999999999}. */
  if (offset.seconds < 0) {
    return "(the local clock is ahead)";
  }
  return "(the local clock is behind)";
}

void print_report(const settings& config, const ntplite_result_t& result) {
  char when[NTPLITE_TIME_TEXT_SIZE];
  char offset[NTPLITE_SECONDS_TEXT_SIZE];
  char delay[NTPLITE_SECONDS_TEXT_SIZE];
  char processing[NTPLITE_SECONDS_TEXT_SIZE];

  time_text(result, when, sizeof(when));
  ntplite_format_seconds(&result.offset, offset, sizeof(offset));
  ntplite_format_seconds(&result.round_trip_delay, delay, sizeof(delay));
  ntplite_format_seconds(&result.server_processing, processing, sizeof(processing));

  std::printf("%-9s %s -> %s\n", "server", config.server, result.server);
  std::printf("%-9s %s\n", "time", when);
  std::printf("%-9s %s s %s\n", "offset", offset, offset_note(result.offset));
  std::printf("%-9s %s s (the server itself took %s s)\n", "delay", delay, processing);

  std::printf("%-9s %d", "stratum", result.stratum);
  if (result.reference[0] != '\0') {
    std::printf(", reference %s", result.reference);
  }
  std::printf("\n");

  std::printf("%-9s NTPv%d, %d request%s\n", "protocol", result.version, result.attempts,
              result.attempts == 1 ? "" : "s");
}

void print_json(const settings& config, const ntplite_status_t status,
                const ntplite_result_t& result) {
  char when[NTPLITE_TIME_TEXT_SIZE];
  char offset[NTPLITE_SECONDS_TEXT_SIZE];
  char delay[NTPLITE_SECONDS_TEXT_SIZE];
  char round_trip[NTPLITE_SECONDS_TEXT_SIZE];
  char processing[NTPLITE_SECONDS_TEXT_SIZE];
  char root_delay[NTPLITE_SECONDS_TEXT_SIZE];
  char root_dispersion[NTPLITE_SECONDS_TEXT_SIZE];

  time_text(result, when, sizeof(when));
  json_seconds(result.offset, offset, sizeof(offset));
  json_seconds(result.round_trip_delay, delay, sizeof(delay));
  json_seconds(result.round_trip_time, round_trip, sizeof(round_trip));
  json_seconds(result.server_processing, processing, sizeof(processing));
  json_seconds(result.root_delay, root_delay, sizeof(root_delay));
  json_seconds(result.root_dispersion, root_dispersion, sizeof(root_dispersion));

  /* Printed as a single object on one line, so a caller can parse it a line at a
   * time: the status is the first field, which is the one that decides whether
   * the rest means anything. */
  std::printf(
      "{"
      "\"ok\":%s,"
      "\"status\":%d,"
      "\"status_text\":\"%s\","
      "\"query\":\"%s\","
      "\"server\":\"%s\","
      "\"valid\":%s,"
      "\"time\":\"%s\","
      "\"offset_seconds\":%s,"
      "\"round_trip_delay_seconds\":%s,"
      "\"round_trip_time_seconds\":%s,"
      "\"server_processing_seconds\":%s,"
      "\"root_delay_seconds\":%s,"
      "\"root_dispersion_seconds\":%s,"
      "\"stratum\":%d,"
      "\"reference\":\"%s\","
      "\"kiss_of_death\":%s,"
      "\"kiss_code\":\"%s\","
      "\"delay_is_plausible\":%s,"
      "\"leap\":%d,"
      "\"version\":%d,"
      "\"mode\":%d,"
      "\"poll\":%d,"
      "\"precision\":%d,"
      "\"attempts\":%d"
      "}\n",
      status == NTP_LITE_OK ? "true" : "false", static_cast<int>(status),
      ntplite_status_string(status), config.server, result.server, result.valid ? "true" : "false",
      when, offset, delay, round_trip, processing, root_delay, root_dispersion, result.stratum,
      result.reference, result.kiss_of_death ? "true" : "false", result.kiss_code,
      result.delay_is_plausible ? "true" : "false", result.leap, result.version, result.mode,
      result.poll, result.precision, result.attempts);
}

}  // namespace

int main(int argc, char** argv) {
  settings config;

  const int parsed = parse_arguments(argc, argv, config);
  if (parsed != 0) {
    return parsed;
  }
  if (config.done) {
    return 0;
  }

  ntplite_result_t result;
  ntplite_result_init(&result);

  const ntplite_status_t status = ntplite_query(config.server, &config.options, &result);

  if (config.json) {
    print_json(config, status, result);
    return status == NTP_LITE_OK ? 0 : 1;
  }

  if (status != NTP_LITE_OK) {
    std::fprintf(stderr, "%s: %s: %s\n", argv[0], config.server, ntplite_status_string(status));
    /* A refusal is the one failure that is worth more than a sentence: it says
     * which server said it and what it asked for. */
    if (result.kiss_of_death) {
      std::fprintf(stderr, "%s: the server at %s sent a Kiss-o'-Death: %s\n", argv[0],
                   result.server, result.kiss_code);
    } else if (status == NTP_LITE_ERR_PROTOCOL && result.server[0] != '\0') {
      std::fprintf(stderr, "%s: the server at %s answered with something malformed\n", argv[0],
                   result.server);
    }
    return 1;
  }

  if (config.offset_only) {
    char offset[NTPLITE_SECONDS_TEXT_SIZE];
    ntplite_format_seconds(&result.offset, offset, sizeof(offset));
    std::printf("%s\n", offset);
    return 0;
  }

  if (config.quiet) {
    char when[NTPLITE_TIME_TEXT_SIZE];
    time_text(result, when, sizeof(when));
    std::printf("%s\n", when);
    return 0;
  }

  print_report(config, result);
  return 0;
}
