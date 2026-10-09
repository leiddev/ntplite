# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - unreleased

Initial development. The C++ client core works and is tested against a real
socket, but the public C entry point is not written yet — see the roadmap in the
README for what is left.

### Added

#### Project skeleton

- Header-only project layout with a strict C++11 baseline and a stable
  `extern "C"` application binary interface.
- `ntplite::ntplite` (header-only, C++) and `ntplite::c` (prebuilt, C / FFI)
  CMake targets, exported through a `find_package(ntplite)` package and a
  `pkg-config` file, plus a `CMakePresets.json` for both supported toolchains.
- Version surface in `include/ntplite/version.hpp` and a `ntplite_status_t`
  enumeration shared by the C and C++ APIs.
- Dependency-free test framework (`tests/ntplite_test.hpp`) with automatic
  registration, filtering, per-case timing and a machine-readable summary.
- Test infrastructure that guards the architecture rather than the features:
  a standalone-include check for every public header (compiled both as C and
  as C++), an ODR check linking two translation units, and a C consumer linked
  against the prebuilt library.
- A separate CMake consumer project (`tests/consumer`) that builds against an
  *installed* ntplite through `find_package`.
- GitHub Actions: a compiler matrix (GCC, Clang, MSVC, AppleClang) across Debug
  and Release, a 32-bit build, AddressSanitizer + UndefinedBehaviorSanitizer and
  ThreadSanitizer jobs, `clang-tidy` and `cppcheck` over the library, a pinned
  `clang-format` gate, a version-hygiene check, an install + `find_package`
  packaging job, a coverage report, and CodeQL analysis.
- Developer tooling: `scripts/format.py`, `scripts/check_version_sync.py`,
  `scripts/cross_validate_ntp.py` and `scripts/remote_build.ps1`.
- Repository files: `.clang-format`, `.clang-tidy`, `.editorconfig`,
  `.gitattributes`, `.gitignore`, `LICENSE` (MIT), `README.md`, `release-notes/`.

#### Time conversion layer

- `ntplite::timestamp` and `ntplite::duration`, both normalised so the
  sub-second field is in [0, 1e9).
- `make_timestamp`, `make_duration`, `timestamp_seconds`, `duration_seconds`
  and their double-precision inverses, plus `difference`, `add_duration` and
  `compare`.
- `std::chrono::system_clock` interoperability via `to_chrono` / `from_chrono`.
- NTP 32.32 fixed-point and 16.16 short-format conversions, with the
  sub-nanosecond rounding behaviour documented and tested.
- NTP era resolution: a raw 32-bit second count is decoded into the era whose
  instant is closest to a supplied reference, so the 2036 rollover is handled
  rather than misread as 1900.

#### NTPv4 packet codec

- `encode_packet` / `decode_packet` for the fixed 48-byte header. Decoding is
  total — every bit pattern is representable — and rejects only buffers that
  are too short.
- `validate_reply`, which holds the protocol rules: server or broadcast mode,
  a supported version, a usable stratum, a synchronised server clock, and an
  origin timestamp that echoes our request.
- Kiss-o'-Death detection with names for all eleven reference identifiers
  defined by RFC 5905, and a printable renderer for reference identifiers.
- `make_client_request` for building the outbound query.

#### UDP transport

- `ntplite::detail::error_code`, the implementation's own error taxonomy, kept
  numerically identical to `ntplite_status_t` by `static_assert`s so the C ABI
  mapping can never drift.
- `ntplite::detail::endpoint`, a copyable IPv4/IPv6 address value that owns a
  `sockaddr_storage`, renders itself as text (bracketing IPv6 once a port
  follows) and is what the socket calls speak.
- `resolve`, which turns a name or an IP literal into the full list of UDP
  endpoints the resolver offers, so a client can fall through from a dead
  address to a live one.
- `deadline`, a `steady_clock`-based timeout so a wall-clock jump cannot turn a
  timeout into a hang, and `sleep_ms` for retry spacing.
- `wait_readable` / `wait_writable` over `select(2)`, retrying interrupted waits
  against the remaining budget instead of giving up early.
- `udp_socket`: a move-only, non-throwing socket wrapper with connect, bind,
  non-blocking mode, address reuse and broadcast, raw send/receive,
  `is_connected`, and `local_endpoint` for discovering the port the kernel
  chose.
- Winsock and POSIX are both supported from the same header. `WIN32_LEAN_AND_MEAN`
  and `NOMINMAX` are applied before the first Windows header is read, and a
  translation unit that has already included `<windows.h>` without them gets an
  explanation instead of a screenful of redefinition errors.

#### Clock discipline

- `ntplite::detail::clock_reading` pairs a reading of the wall clock with a
  reading of the monotonic clock, so the two uses of time cannot be confused:
  the wall clock gives the offset, the monotonic clock measures how long
  something took.
- `estimate_clock` turns the four timestamps of an exchange into a `clock_sample`
  — the offset, the round trip delay, the time the server itself spent, and
  whether the delay is plausible for the round trip it was measured over.
- A clock step cannot corrupt a measurement. The round trip is measured on the
  monotonic clock and supplied by the caller, never computed as T4 - T1, so a
  wall-clock correction during an exchange cannot masquerade as network delay.
- An implausible delay (the server claims it held the request longer than the
  whole exchange took) is reported through `delay_is_plausible` rather than being
  hidden or clamped.

#### Client

- `ntplite::query`, the whole client in one call, with `query_options` for the
  port, IP version, NTP version and timeouts, and a `query_result` carrying the
  four timestamps, the offset, the delay, the server's stratum and reference
  identifier, and its root delay and dispersion.
- Retries: a silent endpoint is asked up to three times, the last attempt
  inheriting whatever budget is left so a quick retry cannot shorten the total
  wait. A reply that is ours but unusable ends the exchange immediately, because
  asking again earns the same answer.
- Server fallback: a name that resolves to several addresses is tried in order
  until one answers, all within one total timeout.
- Anti-spoofing: a datagram is accepted only if it comes from the endpoint that
  was asked *and* its origin timestamp echoes our request. Anything else is
  discarded and the wait continues.
- A Kiss-o'-Death is reported as `NTP_LITE_ERR_KOD` with its code, instead of
  being folded into a generic protocol error, so the caller can tell "this server
  refused" from "this server is broken".
- The result object is cleared before every query, so a caller that reuses one
  can never read a field left over from a previous answer.

#### Public C API

- `ntplite_query()`, the whole client behind one flat `extern "C"` call, with
  `ntplite_options_t` for the port, IP version, protocol version and timeouts and
  `ntplite_result_t` for the four timestamps, the offset, the delay, the server's
  stratum and reference identifier, and its own root delay and dispersion.
- `ntplite_options_init()` and `ntplite_result_init()`. A zero field means "use
  the default", so a zero-initialised options struct is already valid and the init
  function is a convenience rather than a requirement.
- Every result struct carries `struct_size`, the forward compatibility seam. It
  lets a later release add fields without breaking a caller compiled against this
  header: the library writes only the prefix the caller says it has, and leaves
  the caller's own `struct_size` alone.
- `ntplite_format_utc()` and `ntplite_format_seconds()`, so a C caller can print
  a time without borrowing the calendar arithmetic.
- A refused argument is rejected before any IO, and a failed call hands back a
  cleared result, so a caller that reuses one result object can never read a
  previous answer out of it.

#### Printing a time

- `format_utc()` and `format_signed_seconds()` render an instant as ISO 8601 UTC
  and a duration as seconds with a sign and nine decimals.
- The calendar arithmetic is done in the library rather than through `gmtime()`,
  which is not thread safe, is deprecated by MSVC, and - through a 32 bit
  `time_t` - cannot describe the far side of the era boundary NTP is heading
  for. A timestamp in 2100 formats correctly here and would not there.
- A negative duration is printed by borrowing back the second its floored whole
  part holds: `-0.5 s` is stored as `{-1, 500000000}`, and printing the two
  fields as they stand would produce a different half second.

#### Command line tool

- `ntplite` queries a server and reports its time, this machine's offset from it,
  the delay, and the server's stratum and reference identifier. It is written
  against the C API on purpose, so building it is itself a check that the C ABI
  is complete enough to write a program with.
- `--json` prints the whole result as one object on one line, with the status
  first, for a script to parse. `-q` and `-o` print just the time or just the
  offset. Exit codes distinguish a bad command line (2) from a query that failed
  (1).
- The offset is printed with its sign *and* in words, because a signed number is
  easy to read the wrong way round and knowing which clock is ahead is the whole
  point.

#### Tests that talk to a real socket

- `tests/mock_ntp_server.hpp`, a threaded NTP server on loopback, so the client
  can be driven end to end instead of through its own building blocks. It can
  answer, stay silent, truncate, send noise, duplicate its reply, refuse with a
  Kiss-o'-Death, shift its clock, or claim to have held the request far longer
  than the exchange took; and it counts what it was asked.
- End-to-end cases covering the whole path, from resolving a name to reading the
  report: a normal answer, an offset in both directions, a retry answered on the
  second attempt, a refusal that is not retried, an unusable answer that ends the
  exchange, a truncated datagram and noise that are both discarded and retried,
  a duplicate that is accepted exactly once, a silent server that times out with
  the expected attempt count, and a server whose invented processing time makes
  the delay impossible.
- `scripts/cross_validate_ntp.py`, the one check that shares no code with the
  library. It builds an NTP server out of `struct` and arithmetic of its own,
  drives the compiled tool against it, and checks both the datagrams the client
  sent and the numbers it reported - including that a reply from the wrong port,
  and a reply that does not echo the request, are both ignored. Registered with
  CTest as `ntplite.cross_validation`, and skipped when the tool is not built or
  no Python 3 interpreter is available.
- Tests that count the requests a silent peer received wait for the wire to go
  quiet first. A datagram that has been handed to the kernel is not on the peer's
  queue yet, so counting the instant the client stops waiting races the delivery
  of its last request - a race that Linux and Windows hide by delivering loopback
  packets inside the send call, and that macOS lost on a loaded runner.

### Changed

- `NTP_LITE_SANITIZE` accepts a list of sanitizers as well as the on/off
  spelling. `ON` still means AddressSanitizer + UndefinedBehaviorSanitizer;
  `-DNTP_LITE_SANITIZE=thread` now builds with ThreadSanitizer instead, and the
  combination of `thread` with `address` is refused at configure time rather
  than producing a binary that dies before `main`.
- Reference identifiers are no longer padded. `reference_id_text` used to render
  all four octets exactly, so the common stratum 1 identifier `"GPS\0"` came out
  as `"GPS."` and an empty identifier as `"...."`. Trailing NUL and space padding
  is now dropped, giving `"GPS"` and `""`.
- `is_kiss_of_death` now requires a reference identifier as well as stratum 0.
  Stratum 0 with four NULs is an empty packet, not a refusal, and reporting it as
  one told the caller a server had said "no" when it had said nothing.

### Fixed

- `ntplite_status_t` now pins its underlying type to `int` when compiled as
  C++. Without it, C++ narrows the enumeration's value range to its
  enumerators and any out-of-range value arriving from C or an FFI binding is
  undefined behaviour on load. Enumerator values are now explicit and frozen.
- The test framework's `CHECK_STREQ` now holds its operands in `std::string`
  values instead of raw pointers. A call such as
  `CHECK_STREQ(make_text().c_str(), "x")` used to capture a pointer into a
  temporary that was destroyed before the comparison ran, so the assertion
  could pass or fail depending on what the stack happened to hold.
- The resolver no longer offers the same endpoint twice. A hosts file that names
  a host on several lines, or a resolver that answers from more than one source,
  can put the same address in the results more than once; the client then asked
  it again, spending a whole extra timeout and a second datagram on a question it
  had already had answered. Repeats are now dropped, keeping the resolver's
  order.
- Sending no longer hands a destination to a socket that already has a peer.
  `send_to()` called `sendto()` unconditionally, which Linux and Winsock accept
  and the BSD-derived stacks do not: macOS fails the call with `EISCONN`, so
  every query died as a network error before its first datagram left the
  machine, and no test noticed because the whole suite had only ever run on
  Windows and Linux. The address is now passed only while the socket is
  unconnected, where it is the only thing that can mean anything, and the
  `send_to_on_a_connected_socket_still_sends` case covers it.

[Unreleased]: https://github.com/leiddev/ntplite/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/leiddev/ntplite/releases/tag/v0.1.0
