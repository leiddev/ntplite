// ============================================================================
// ntplite - tools/ntplite_cli.cpp
// ----------------------------------------------------------------------------
// The `ntplite` command line tool.
//
// Milestone 0: only the version/usage surface exists.  The actual query
// plumbing is added together with the client core.
// ============================================================================

#include <cstdio>
#include <cstring>
#include <ntplite/ntplite.hpp>

namespace {

void print_usage(const char* program) {
  std::printf(
      "ntplite %s - a tiny NTP client\n"
      "\n"
      "usage: %s [options] [server]\n"
      "\n"
      "options:\n"
      "  -h, --help      show this message and exit\n"
      "  -V, --version   print the library version and exit\n"
      "\n"
      "server defaults to pool.ntp.org\n",
      ntplite::version_string(), program);
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];

    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      print_usage(argv[0]);
      return 0;
    }

    if (std::strcmp(arg, "-V") == 0 || std::strcmp(arg, "--version") == 0) {
      std::printf("ntplite %s\n", ntplite::version_string());
      return 0;
    }
  }

  std::fprintf(stderr,
               "%s: querying is not implemented yet (milestone 0 scaffold)\n"
               "run '%s --help' for the planned interface\n",
               argv[0], argv[0]);
  return 2;
}
