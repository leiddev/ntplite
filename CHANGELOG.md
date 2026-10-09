# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - unreleased

Initial development. The library is not usable as an NTP client yet — see the
roadmap in the README for what is left.

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
- GitHub Actions: compiler/build-type matrix on Linux (GCC + Clang) and Windows
  (MSVC), an ASan/UBSan job, a pinned `clang-format` gate, a version-hygiene
  check, an install + `find_package` packaging job, and CodeQL analysis.
- Developer tooling: `scripts/format.py`, `scripts/check_version_sync.py` and
  `scripts/remote_build.ps1`.
- Repository files: `.clang-format`, `.editorconfig`, `.gitattributes`,
  `.gitignore`, `LICENSE` (MIT), `README.md`, `release-notes/`.

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

### Fixed

- `ntplite_status_t` now pins its underlying type to `int` when compiled as
  C++. Without it, C++ narrows the enumeration's value range to its
  enumerators and any out-of-range value arriving from C or an FFI binding is
  undefined behaviour on load. Enumerator values are now explicit and frozen.

[Unreleased]: https://github.com/leiddev/ntplite/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/leiddev/ntplite/releases/tag/v0.1.0
