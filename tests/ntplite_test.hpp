// ============================================================================
// ntplite - tests/ntplite_test.hpp
// ----------------------------------------------------------------------------
// A deliberately tiny, dependency-free xUnit-style test framework.
//
// Why not GoogleTest / Catch2?
//   * no network access needed at configure time -> CI can never fail because
//     a package registry was slow or a tarball moved;
//   * nothing to install for contributors;
//   * the whole framework is ~250 lines, auditable in one sitting.
//
// Usage
// -----
//     #include "ntplite_test.hpp"
//
//     NTP_TEST(time, converts_ntp_to_unix) {
//       NTP_TEST_CHECK_EQ(42, compute());
//       NTP_TEST_REQUIRE(true);
//       NTP_TEST_CHECK_NEAR(1.5, 1.5001, 1e-3);
//     }
//
// Tests are registered automatically at static-initialisation time.  Exactly
// one translation unit must call ntplite_test::run_all() (see test_main.cpp).
// ============================================================================

#ifndef NTP_LITE_TESTS_NTP_LITE_TEST_HPP
#define NTP_LITE_TESTS_NTP_LITE_TEST_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define NTP_TEST_UNUSED __attribute__((unused))
#else
#define NTP_TEST_UNUSED
#endif

// Assertion macros inevitably produce conditions the compiler can fold, e.g.
// NTP_TEST_CHECK_EQ(0, 0).  MSVC reports those as C4127 ("conditional
// expression is constant"), which -WX turns into an error, so the macros push
// and pop the warning state around the generated `if`.
#if defined(_MSC_VER)
#define NTP_TEST_DIAGNOSTICS_PUSH                        __pragma(warning(push))
#define NTP_TEST_DIAGNOSTICS_SUPPRESS_CONSTANT_CONDITION __pragma(warning(disable : 4127))
#define NTP_TEST_DIAGNOSTICS_POP                         __pragma(warning(pop))
#else
#define NTP_TEST_DIAGNOSTICS_PUSH
#define NTP_TEST_DIAGNOSTICS_SUPPRESS_CONSTANT_CONDITION
#define NTP_TEST_DIAGNOSTICS_POP
#endif

namespace ntplite_test {

typedef void (*test_function)();

struct test_case {
  const char* suite;
  const char* name;
  test_function function;
};

inline std::vector<test_case>& registry() {
  static std::vector<test_case> cases;
  return cases;
}

inline int& total_checks() {
  static int count = 0;
  return count;
}

inline int& total_failures() {
  static int count = 0;
  return count;
}

inline int& current_failures() {
  static int count = 0;
  return count;
}

struct registrar {
  registrar(const char* suite, const char* name, test_function fn) {
    test_case entry;
    entry.suite = suite;
    entry.name = name;
    entry.function = fn;
    registry().push_back(entry);
  }
};

// ---------------------------------------------------------------------------
// Value rendering.  Anything that can be written to an ostream works; the
// overloads below fix up the cases where operator<< does something unhelpful.
// ---------------------------------------------------------------------------
template <typename T>
inline std::string repr(const T& value) {
  std::ostringstream os;
  os << value;
  return os.str();
}

inline std::string repr(const bool& value) {
  return value ? "true" : "false";
}

inline std::string repr(const signed char& value) {
  std::ostringstream os;
  os << static_cast<int>(value);
  return os.str();
}

inline std::string repr(const unsigned char& value) {
  std::ostringstream os;
  os << static_cast<unsigned int>(value);
  return os.str();
}

inline std::string repr(const char* value) {
  if (value == NULL)
    return "(null)";
  std::ostringstream os;
  os << '"' << value << '"';
  return os.str();
}

inline std::string repr(char* value) {
  return repr(static_cast<const char*>(value));
}

inline void report_failure(const char* file, int line, const std::string& what) {
  ++total_failures();
  ++current_failures();
  std::fprintf(stderr, "      %s:%d: error: %s\n", file, line, what.c_str());
}

inline bool nearly_equal(double a, double b, double epsilon) {
  if (a == b)
    return true;
  const double diff = std::fabs(a - b);
  if (diff <= epsilon)
    return true;
  return diff <= epsilon * std::fabs(a > b ? a : b);
}

inline bool string_equal(const char* a, const char* b) {
  if (a == NULL || b == NULL)
    return a == b;
  return std::strcmp(a, b) == 0;
}

/// Copies a C string into a std::string that the caller owns.
///
/// CHECK_STREQ has to hold on to its operands for the duration of the
/// assertion, and an argument such as `some_function().c_str()` - or
/// `std::string(...).c_str()` - is a pointer *into a temporary*, which is
/// destroyed at the end of the declaration that captures it.  Copying first
/// is what makes the obvious spelling safe.
inline std::string copy_string(const char* value) {
  return value == NULL ? std::string() : std::string(value);
}

// ---------------------------------------------------------------------------
// Case ordering: deterministic, so CI logs diff cleanly between runs.
// ---------------------------------------------------------------------------
inline bool case_less(const test_case& a, const test_case& b) {
  const int suite_cmp = std::strcmp(a.suite, b.suite);
  if (suite_cmp != 0)
    return suite_cmp < 0;
  return std::strcmp(a.name, b.name) < 0;
}

inline void print_usage(const char* program) {
  std::printf(
      "usage: %s [options]\n"
      "\n"
      "options:\n"
      "  -f, --filter <substring>   only run tests whose \"suite/name\" contains\n"
      "                             the given substring\n"
      "  -l, --list                 list the registered tests and exit\n"
      "  -q, --quiet                only print failures and the summary\n"
      "  -h, --help                 show this message\n",
      program);
}

inline int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg == "-l" || arg == "--list") {
      list_only = true;
    } else if (arg == "-q" || arg == "--quiet") {
      quiet = true;
    } else if (arg.rfind("--filter=", 0) == 0) {
      filter = arg.substr(9);
    } else if ((arg == "-f" || arg == "--filter") && i + 1 < argc) {
      filter = argv[++i];
    } else {
      std::fprintf(stderr, "%s: unknown argument '%s'\n\n", argv[0], arg.c_str());
      print_usage(argv[0]);
      return 2;
    }
  }

  std::vector<test_case> cases = registry();
  std::sort(cases.begin(), cases.end(), case_less);

  if (list_only) {
    for (std::size_t i = 0; i < cases.size(); ++i) {
      std::printf("%s/%s\n", cases[i].suite, cases[i].name);
    }
    return 0;
  }

  int run_count = 0;
  int failed_count = 0;

  if (!quiet) {
    std::printf("Running ntplite test suite");
    if (!filter.empty())
      std::printf(" (filter: \"%s\")", filter.c_str());
    std::printf("\n");
  }

  for (std::size_t i = 0; i < cases.size(); ++i) {
    const test_case& entry = cases[i];
    std::string full_name = std::string(entry.suite) + "/" + entry.name;

    if (!filter.empty() && full_name.find(filter) == std::string::npos)
      continue;

    ++run_count;
    current_failures() = 0;

    if (!quiet)
      std::printf("[ RUN      ] %s\n", full_name.c_str());

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    entry.function();
    const std::chrono::steady_clock::time_point finish = std::chrono::steady_clock::now();

    const double elapsed_ms =
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(finish - start)
            .count();

    if (current_failures() == 0) {
      if (!quiet)
        std::printf("[       OK ] %s (%.3f ms)\n", full_name.c_str(), elapsed_ms);
    } else {
      ++failed_count;
      std::printf("[  FAILED  ] %s (%.3f ms)\n", full_name.c_str(), elapsed_ms);
    }
  }

  std::printf("\n");
  std::printf("-------------------------------------------------------------------------------\n");
  std::printf("tests run: %d   passed: %d   failed: %d\n", run_count, run_count - failed_count,
              failed_count);
  std::printf("checks   : %d   failed: %d\n", total_checks(), total_failures());
  std::printf("-------------------------------------------------------------------------------\n");

  if (run_count == 0) {
    std::fprintf(stderr, "error: no test matched%s%s\n", filter.empty() ? "" : " filter '",
                 filter.empty() ? "" : filter.c_str());
    if (!filter.empty())
      std::fprintf(stderr, "'\n");
    return 1;
  }

  return failed_count == 0 ? 0 : 1;
}

}  // namespace ntplite_test

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------
#define NTP_TEST(suite, name)                                                                      \
  static void ntplite_test_case_##suite##_##name(void);                                            \
  static const ::ntplite_test::registrar ntplite_test_registrar_##suite##_##name NTP_TEST_UNUSED = \
      ::ntplite_test::registrar(#suite, #name, &ntplite_test_case_##suite##_##name);               \
  static void ntplite_test_case_##suite##_##name(void)

#define NTP_TEST_CHECK(expr)                                                                      \
  do {                                                                                            \
    NTP_TEST_DIAGNOSTICS_PUSH                                                                     \
    NTP_TEST_DIAGNOSTICS_SUPPRESS_CONSTANT_CONDITION                                              \
    ++::ntplite_test::total_checks();                                                             \
    if (!(expr)) {                                                                                \
      ::ntplite_test::report_failure(__FILE__, __LINE__, std::string("CHECK(" #expr ") failed")); \
    }                                                                                             \
    NTP_TEST_DIAGNOSTICS_POP                                                                      \
  } while (false)

#define NTP_TEST_REQUIRE(expr)                                                                \
  do {                                                                                        \
    NTP_TEST_DIAGNOSTICS_PUSH                                                                 \
    NTP_TEST_DIAGNOSTICS_SUPPRESS_CONSTANT_CONDITION                                          \
    ++::ntplite_test::total_checks();                                                         \
    if (!(expr)) {                                                                            \
      ::ntplite_test::report_failure(                                                         \
          __FILE__, __LINE__, std::string("REQUIRE(" #expr ") failed - aborting this test")); \
      return;                                                                                 \
    }                                                                                         \
    NTP_TEST_DIAGNOSTICS_POP                                                                  \
  } while (false)

#define NTP_TEST_CHECK_EQ(a, b)                                                            \
  do {                                                                                     \
    NTP_TEST_DIAGNOSTICS_PUSH                                                              \
    NTP_TEST_DIAGNOSTICS_SUPPRESS_CONSTANT_CONDITION                                       \
    ++::ntplite_test::total_checks();                                                      \
    const auto ntplite_test_lhs = (a);                                                     \
    const auto ntplite_test_rhs = (b);                                                     \
    if (!(ntplite_test_lhs == ntplite_test_rhs)) {                                         \
      ::ntplite_test::report_failure(__FILE__, __LINE__,                                   \
                                     std::string("CHECK_EQ(" #a ", " #b ") failed: ") +    \
                                         ::ntplite_test::repr(ntplite_test_lhs) +          \
                                         " != " + ::ntplite_test::repr(ntplite_test_rhs)); \
    }                                                                                      \
    NTP_TEST_DIAGNOSTICS_POP                                                               \
  } while (false)

#define NTP_TEST_CHECK_NE(a, b)                                                                  \
  do {                                                                                           \
    NTP_TEST_DIAGNOSTICS_PUSH                                                                    \
    NTP_TEST_DIAGNOSTICS_SUPPRESS_CONSTANT_CONDITION                                             \
    ++::ntplite_test::total_checks();                                                            \
    const auto ntplite_test_lhs = (a);                                                           \
    const auto ntplite_test_rhs = (b);                                                           \
    if (ntplite_test_lhs == ntplite_test_rhs) {                                                  \
      ::ntplite_test::report_failure(__FILE__, __LINE__,                                         \
                                     std::string("CHECK_NE(" #a ", " #b ") failed: both are ") + \
                                         ::ntplite_test::repr(ntplite_test_lhs));                \
    }                                                                                            \
    NTP_TEST_DIAGNOSTICS_POP                                                                     \
  } while (false)

#define NTP_TEST_CHECK_NEAR(a, b, epsilon)                                                       \
  do {                                                                                           \
    NTP_TEST_DIAGNOSTICS_PUSH                                                                    \
    NTP_TEST_DIAGNOSTICS_SUPPRESS_CONSTANT_CONDITION                                             \
    ++::ntplite_test::total_checks();                                                            \
    const double ntplite_test_lhs = static_cast<double>(a);                                      \
    const double ntplite_test_rhs = static_cast<double>(b);                                      \
    if (!::ntplite_test::nearly_equal(ntplite_test_lhs, ntplite_test_rhs,                        \
                                      static_cast<double>(epsilon))) {                           \
      ::ntplite_test::report_failure(__FILE__, __LINE__,                                         \
                                     std::string("CHECK_NEAR(" #a ", " #b ") failed: ") +        \
                                         ::ntplite_test::repr(ntplite_test_lhs) + " vs " +       \
                                         ::ntplite_test::repr(ntplite_test_rhs) + " (epsilon " + \
                                         ::ntplite_test::repr(static_cast<double>(epsilon)) +    \
                                         ")");                                                   \
    }                                                                                            \
    NTP_TEST_DIAGNOSTICS_POP                                                                     \
  } while (false)

#define NTP_TEST_CHECK_STREQ(a, b)                                                                 \
  do {                                                                                             \
    NTP_TEST_DIAGNOSTICS_PUSH                                                                      \
    NTP_TEST_DIAGNOSTICS_SUPPRESS_CONSTANT_CONDITION                                               \
    ++::ntplite_test::total_checks();                                                              \
    const std::string ntplite_test_lhs = ::ntplite_test::copy_string(a);                           \
    const std::string ntplite_test_rhs = ::ntplite_test::copy_string(b);                           \
    if (!::ntplite_test::string_equal(ntplite_test_lhs.c_str(), ntplite_test_rhs.c_str())) {       \
      ::ntplite_test::report_failure(__FILE__, __LINE__,                                           \
                                     std::string("CHECK_STREQ(" #a ", " #b ") failed: ") +         \
                                         ::ntplite_test::repr(ntplite_test_lhs.c_str()) +          \
                                         " != " + ::ntplite_test::repr(ntplite_test_rhs.c_str())); \
    }                                                                                              \
    NTP_TEST_DIAGNOSTICS_POP                                                                       \
  } while (false)

/// Records an unconditional failure.  Handy as the `else` branch of a manual
/// condition that the CHECK macros cannot express.
#define NTP_TEST_FAIL(msg) ::ntplite_test::report_failure(__FILE__, __LINE__, (msg))

#endif  // NTP_LITE_TESTS_NTP_LITE_TEST_HPP
