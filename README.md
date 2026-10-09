# ntplite

A tiny, dependency-free **NTP client library** for C++11, with a stable C ABI.

[![CI](https://github.com/leiddev/ntplite/actions/workflows/ci.yml/badge.svg)](https://github.com/leiddev/ntplite/actions/workflows/ci.yml)
[![CodeQL](https://github.com/leiddev/ntplite/actions/workflows/codeql.yml/badge.svg)](https://github.com/leiddev/ntplite/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

- **Header-only.** `#include <ntplite/ntplite.hpp>` and you are done. No extra
  build step, no library to link (besides Winsock on Windows).
- **C and C++ friendly.** A flat `extern "C"` API means plain C, Rust, Go and
  Python `ctypes` can all talk to it.
- **Portable.** Windows (MSVC) and Linux (GCC / Clang) are both first-class and
  both are exercised on every push.
- **Never touches your clock.** `ntplite` only *measures* offsets and reports
  them; applying a correction is left to you. That also means it never needs
  administrative privileges.
- **Zero dependencies.** Not even a test framework.

> Status: early development. The C++ client below works today; the C API is the
> target interface and is being filled in milestone by milestone — see
> [Roadmap](#roadmap).

---

## Quick start (C++)

```cpp
#include <ntplite/ntplite.hpp>

#include <cstdio>

int main() {
  ntplite::query_result result;
  const ntplite::error_code status = ntplite::query("pool.ntp.org", result);

  if (status != ntplite::error_code::ok) {
    std::printf("query failed: %s\n", ntplite::error_code_name(status));
    return 1;
  }

  // The offset to add to the local clock, in milliseconds.  Nothing is applied;
  // ntplite only measures.
  std::printf("offset: %+.3f ms, round trip: %.3f ms\n",
              result.offset_seconds() * 1000.0,
              result.round_trip_delay_seconds() * 1000.0);
  return 0;
}
```

## Quick start (C)

```c
#include <ntplite/ntplite.h>
#include <stdio.h>

int main(void) {
  ntplite_time_t now;
  ntplite_status_t status = ntplite_query("pool.ntp.org", 3000, &now);

  if (status != NTP_LITE_OK) {
    fprintf(stderr, "query failed: %s\n", ntplite_status_string(status));
    return 1;
  }

  printf("unix time: %lld s\n", (long long)now.unix_seconds);
  return 0;
}
```

---

## Integrating

### With CMake

```cmake
find_package(ntplite CONFIG REQUIRED)

# Header-only, C++ consumers.
target_link_libraries(my_app PRIVATE ntplite::ntplite)

# Prebuilt stable C ABI, C / FFI consumers.
target_link_libraries(my_c_app PRIVATE ntplite::c)
```

### Without CMake (drop-in)

Copy the `include/` directory into your project and either:

**A. C++** — include `<ntplite/ntplite.hpp>`; nothing else is needed.

**B. C** — link the one compiled translation unit. In exactly one C++ file:

```cpp
#define NTP_LITE_IMPLEMENTATION
#include <ntplite/ntplite.h>
```

and keep including `<ntplite/ntplite.h>` from your C sources.

On Windows, link `ws2_32`.

> Do not combine A/B with linking `ntplite::c` — you would define every symbol
> twice. Pick one route.

---

## Building from source

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake presets are provided for the two supported toolchains:

```sh
cmake --preset windows-vs2022     # Visual Studio 2022, x64
cmake --preset unix-debug         # Ninja + GCC/Clang, Debug
cmake --preset unix-sanitize      # Ninja + ASan/UBSan
```

### Useful options

| Option | Default | Meaning |
| --- | --- | --- |
| `NTP_LITE_BUILD_TOOLS` | `ON` | Build the `ntplite` command line tool |
| `NTP_LITE_BUILD_EXAMPLES` | `ON` | Build the example programs |
| `NTP_LITE_BUILD_TESTS` | `ON` | Build the test suite |
| `NTP_LITE_ONLINE_TESTS` | `OFF` | Register tests that need real internet access |
| `NTP_LITE_WERROR` | `OFF` | Treat compiler warnings as errors |
| `NTP_LITE_SANITIZE` | `OFF` | Build with ASan + UBSan (GCC/Clang only) |
| `NTP_LITE_INSTALL` | `ON` | Generate install / packaging rules |

### Tooling scripts

```sh
python scripts/format.py --check         # clang-format gate (same script CI runs)
python scripts/format.py                 # reformat in place
python scripts/check_version_sync.py     # version consistency across the tree
```

---

## Design notes

* **Header-only, but honest about C.**
  The implementation is C++11, so a C compiler cannot build it. `ntplite`
  therefore ships two ways to consume it: a fully inline C++ header and a
  single compiled translation unit with a stable `extern "C"` ABI.

* **Nothing global, nothing mutable.**
  C++11 has no `inline` variables, so every piece of shared state lives in a
  function-local `static` reached through an `inline` accessor. A dedicated
  ODR test links two translation units that both include the headers, so a
  mistake here fails the build rather than a user's link.

* **Deterministic tests.**
  Network-dependent behaviour is tested against an in-process mock NTP server
  bound to `127.0.0.1`, so CI never depends on the public internet. Tests that
  do reach out are opt-in via `NTP_LITE_ONLINE_TESTS`.

---

## Repository layout

```
include/ntplite/       public headers (ntplite.hpp for C++, ntplite.h for C)
src/                   the single compiled translation unit (C ABI)
tools/                 the `ntplite` command line tool
examples/              minimal C and C++ integration examples
tests/                 dependency-free test framework, mock server, consumers
scripts/               developer / CI helper scripts (Python)
cmake/                 package configuration templates
release-notes/         human written notes, one file per release
```

---

## Roadmap

- [x] **0** — repository scaffold, build system, CI, test framework
- [x] **1** — NTP ↔ Unix ↔ `std::chrono` time conversion, NTP era handling
- [x] **2** — NTPv4 packet encoding / decoding, Kiss-o'-Death handling
- [x] **3** — socket abstraction (Winsock2 / POSIX), timeouts, DNS
- [x] **4** — client core: offset and delay estimation, retries
- [ ] **5** — public C API, CLI tool, installable CMake package
- [ ] **6** — in-process mock NTP server, end-to-end tests
- [ ] **7** — full CI matrix, sanitizers, static analysis
- [ ] **8** — verified on a real Linux host over SSH

See [CHANGELOG.md](CHANGELOG.md) for what has actually landed.

---

## Accuracy

`ntplite` implements the classic four-timestamp exchange, which yields a
clock offset and a round-trip delay per sample:

```
        client                        server
          |  ---- request  (T1) ------> |
          |                    (T2)     |
          |  <--- response (T3) ------- |
   (T4)   |

  offset = ((T2 - T1) + (T3 - T4)) / 2
  delay  = (T4 - T1) - (T3 - T2)
```

Expect millisecond accuracy over the public internet and sub-millisecond on a
LAN. For nanosecond accuracy you want PTP (IEEE 1588), not NTP.

---

## License

[MIT](LICENSE).
