# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-10-09

First milestone: the project skeleton. No NTP functionality yet.

### Added

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
- Developer tooling: `scripts/format.py` and `scripts/check_version_sync.py`.
- Repository files: `.clang-format`, `.editorconfig`, `.gitattributes`,
  `.gitignore`, `LICENSE` (MIT), `README.md`, `release-notes/`.

[Unreleased]: https://github.com/leiddev/ntplite/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/leiddev/ntplite/releases/tag/v0.1.0
