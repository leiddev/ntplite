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
  ntplite_result_t result;
  ntplite_result_init(&result);

  /* NULL options means the defaults; pass ntplite_options_init() to change them. */
  const ntplite_status_t status = ntplite_query("pool.ntp.org", NULL, &result);

  if (status != NTP_LITE_OK) {
    fprintf(stderr, "query failed: %s\n", ntplite_status_string(status));
    return 1;
  }

  char when[NTPLITE_TIME_TEXT_SIZE];
  char offset[NTPLITE_SECONDS_TEXT_SIZE];
  ntplite_format_utc(&result.server_time, when, sizeof(when));
  ntplite_format_seconds(&result.offset, offset, sizeof(offset));

  printf("the time is %s\n", when);
  printf("this machine's clock is %s s from it\n", offset);
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

## The command line tool

`ntplite` is the same client as a program. It reports what a server says the
time is and how far this machine's clock is from it, and it never touches the
local clock.

```
$ ntplite
server    pool.ntp.org -> 162.159.200.1:123
time      2026-10-09T12:34:56.789012345Z
offset    -2.550000000 s (the local clock is ahead)
delay     +0.175768440 s (the server itself took +0.153325080 s)
stratum   3, reference 10.140.8.4
protocol  NTPv4, 1 request
```

| Option | Meaning |
| --- | --- |
| `-4`, `-6` | resolve IPv4 / IPv6 addresses only |
| `-p`, `--port N` | server port (default 123) |
| `--ntp-version N` | speak NTPv3 or NTPv4 (default 4) |
| `-t`, `--timeout MS` | budget for one server (default 1000) |
| `-T`, `--total-timeout MS` | budget for the whole query (default 5000) |
| `--retry-interval MS` | wait before resending to a silent server (default 400) |
| `-q`, `--quiet` | print only the server's time, ISO 8601 UTC |
| `-o`, `--offset` | print only the offset, in seconds |
| `--json` | print the whole result as one JSON object |

Exit codes are `0` success, `1` the query failed, `2` the command line was wrong,
so a script can tell a bad invocation from a server that did not answer.

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
| `NTP_LITE_ONLINE_TESTS` | `OFF` | Register the verification against real NTP servers |
| `NTP_LITE_WERROR` | `OFF` | Treat compiler warnings as errors |
| `NTP_LITE_SANITIZE` | `OFF` | Sanitizers to build with: `ON` for ASan + UBSan, or a list such as `thread` (GCC/Clang only) |
| `NTP_LITE_INSTALL` | `ON` | Generate install / packaging rules |

### Tooling scripts

```sh
python scripts/format.py --check         # clang-format gate (same script CI runs)
python scripts/format.py                 # reformat in place
python scripts/check_version_sync.py     # version consistency across the tree
python scripts/cross_validate_ntp.py     # the tool, against a second NTP implementation
python scripts/verify_real_servers.py    # the tool, against real servers on the internet
```

`cross_validate_ntp.py` builds an NTP server out of `struct` and arithmetic of
its own, then drives the compiled `ntplite` tool against it and checks what comes
back. The C++ tests use a mock server that shares the library's packet codec, so
a shared misreading of RFC 5905 would be invisible there; this script is the
check that it is not. It needs the tool to be built, and is registered with CTest
as `ntplite.cross_validation`.

`verify_real_servers.py` is the same idea pointed outwards, at the servers
everybody else uses. It asks a real server with the tool, asks the same machine
again with an NTP client written longhand in the script, and requires the two to
agree within what the two round trips allow; it also checks the reported instant
against the clock the host keeps for itself, and watches the wall clock and the
monotonic clock move together across the query, which is how "the local clock was
never touched" is actually verified. It needs a network that lets UDP port 123
out, so it is opt-in: configure with `-DNTP_LITE_ONLINE_TESTS=ON` and CTest
registers it as `ntplite.real_servers`.

```sh
cmake -S . -B build -DNTP_LITE_ONLINE_TESTS=ON -DNTP_LITE_BUILD_TOOLS=ON
cmake --build build
ctest --test-dir build -R ntplite.real_servers --output-on-failure

python scripts/verify_real_servers.py --servers pool.ntp.org,time.cloudflare.com --repeat 2
```

Both ways are safe to run on a machine whose clock matters: neither the library
nor the script sets the system time, and the script would fail if it had been.

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
  Behaviour that needs a network is tested over real loopback sockets against
  `tests/mock_ntp_server.hpp`, a threaded NTP server the test itself drives, so
  CI never depends on the public internet. The mock shares the library's packet
  codec, which is a real weakness: `scripts/cross_validate_ntp.py` therefore
  speaks the same protocol with an implementation of its own and checks the
  compiled tool against that, so the two cannot be wrong in the same way. A
  query that is meant to fail is aimed at an address RFC 5737 reserves for
  documentation, which can never be a real host. Tests that do reach out to real
  servers are opt-in via `NTP_LITE_ONLINE_TESTS`, and those are the ones that
  check the assumption everything else rests on: that a real stratum 1 or 2
  machine, over a real network, with a clock kept right by somebody else, is
  something this client reads correctly. `scripts/verify_real_servers.py` is that
  check, and it needs no NTP daemon on the host to judge the answer.

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
.clang-format          formatting, gated on every push (clang-format 19.1.7)
.clang-tidy            the static analysis check list, gated on every push
```

---

## Continuous integration

Every push to `main` and every pull request runs `.github/workflows/ci.yml`,
with the same deterministic tests described under [Design
notes](#design-notes). Nothing reaches out to the internet.

| Job | What it covers |
| --- | --- |
| `linux` | GCC and Clang, Debug and Release, on Ubuntu 22.04 |
| `linux-32-bit` | the same suite where pointers, `size_t` and `long` are half the width |
| `macos` | AppleClang, Debug and Release |
| `windows` | MSVC (Visual Studio 2022), Debug and Release |
| `sanitizers` | AddressSanitizer + UndefinedBehaviorSanitizer, and ThreadSanitizer |
| `clang-tidy` | the static analyser and the bug-prone idioms, over the library |
| `cppcheck` | a second opinion from a different analyser, over the library |
| `format` | clang-format 19.1.7, pinned so a runner upgrade cannot fail the build |
| `hygiene` | the version is declared identically everywhere |
| `coverage` | a gcovr report, uploaded as an artifact (reported, never enforced) |
| `package` | `cmake --install` into a scratch prefix, consumed by `find_package()` |

The two analysers run over the library only: `src/ntplite_c.cpp` plus the
generated "header hygiene" translation unit for each public header, one header at
a time. That is exactly the code a consumer compiles, in the smallest units that
compile it, which keeps the run quick and keeps every finding where it can be
acted on. The tests, tools and examples are covered by the compiler, the
sanitizers and the test suite itself.

The matrix earns its keep. The `macos` row is the only one that runs a
BSD-derived socket stack, and the first time it ran it caught `send_to()` handing
a destination address to a socket that already had a peer: Linux and Winsock
accept that, macOS fails it with `EISCONN`, and every query had been dying as a
network error a line before its first datagram.

---

## Roadmap

- [x] **0** — repository scaffold, build system, CI, test framework
- [x] **1** — NTP ↔ Unix ↔ `std::chrono` time conversion, NTP era handling
- [x] **2** — NTPv4 packet encoding / decoding, Kiss-o'-Death handling
- [x] **3** — socket abstraction (Winsock2 / POSIX), timeouts, DNS
- [x] **4** — client core: offset and delay estimation, retries
- [x] **5** — public C API, CLI tool, installable CMake package
- [x] **6** — in-process mock NTP server, end-to-end tests, Python cross-validation
- [x] **7** — full CI matrix, sanitizers, static analysis
- [x] **8** — verified against real servers, driven from a real Linux host over SSH

Every milestone is in. The library does not set the system clock, does not need
one, and does not have one to set: it reads a server and hands the answer back.

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
