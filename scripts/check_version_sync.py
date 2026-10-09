#!/usr/bin/env python3
"""Verify that the ntplite version is declared identically everywhere.

The version lives in three places that are easy to forget:

    CMakeLists.txt                  project(ntplite VERSION x.y.z ...)
    include/ntplite/version.hpp     NTP_LITE_VERSION_MAJOR/_MINOR/_PATCH
    CHANGELOG.md                    a "## [x.y.z]" section must exist

Optionally, `--tag` additionally verifies a git tag such as "v0.1.0".

Exit code 0 when everything agrees, 1 otherwise.  Run by CI and by
.github/workflows/release.yml before anything is published.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Optional, Sequence, Tuple

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

CMAKE_LISTS = REPOSITORY_ROOT / "CMakeLists.txt"
VERSION_HEADER = REPOSITORY_ROOT / "include" / "ntplite" / "version.hpp"
CHANGELOG = REPOSITORY_ROOT / "CHANGELOG.md"

PROJECT_VERSION_RE = re.compile(
    r"project\s*\([^)]*?\bVERSION\s+(\d+)\.(\d+)\.(\d+)", re.DOTALL
)
HEADER_COMPONENT_RE = {
    "major": re.compile(r"^#define\s+NTP_LITE_VERSION_MAJOR\s+(\d+)\s*$", re.MULTILINE),
    "minor": re.compile(r"^#define\s+NTP_LITE_VERSION_MINOR\s+(\d+)\s*$", re.MULTILINE),
    "patch": re.compile(r"^#define\s+NTP_LITE_VERSION_PATCH\s+(\d+)\s*$", re.MULTILINE),
}
HEADER_STRING_RE = re.compile(
    r'^#define\s+NTP_LITE_VERSION_STRING\s+"([0-9]+\.[0-9]+\.[0-9]+)"\s*$', re.MULTILINE
)

Version = Tuple[int, int, int]


def read(path: Path) -> str:
    if not path.is_file():
        raise SystemExit(f"error: {path} does not exist")
    return path.read_text(encoding="utf-8")


def version_text(version: Version) -> str:
    return "%d.%d.%d" % version


def parse_cmake_version() -> Version:
    match = PROJECT_VERSION_RE.search(read(CMAKE_LISTS))
    if not match:
        raise SystemExit(
            f"error: could not find `project(<name> VERSION x.y.z ...)` in {CMAKE_LISTS}"
        )
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def parse_header_version() -> Version:
    text = read(VERSION_HEADER)
    components = []
    for key in ("major", "minor", "patch"):
        match = HEADER_COMPONENT_RE[key].search(text)
        if not match:
            raise SystemExit(
                f"error: could not find #define NTP_LITE_VERSION_{key.upper()} in {VERSION_HEADER}"
            )
        components.append(int(match.group(1)))

    declared = tuple(components)  # type: ignore[assignment]

    string_match = HEADER_STRING_RE.search(text)
    if not string_match:
        raise SystemExit(
            f"error: could not find #define NTP_LITE_VERSION_STRING in {VERSION_HEADER}"
        )

    if string_match.group(1) != version_text(declared):  # type: ignore[arg-type]
        raise SystemExit(
            "error: %s declares NTP_LITE_VERSION_STRING \"%s\" but the numeric "
            "components say \"%s\"" % (VERSION_HEADER, string_match.group(1), version_text(declared))
        )

    return declared  # type: ignore[return-value]


def check_changelog(version: Version) -> Optional[str]:
    """Returns an error message, or None when the changelog mentions the version."""
    if not CHANGELOG.is_file():
        return None  # nothing to check yet

    text = CHANGELOG.read_text(encoding="utf-8")
    if version_text(version) not in text:
        return f"error: {CHANGELOG} has no section for version {version_text(version)}"
    return None


def normalize_tag(tag: str) -> Version:
    stripped = tag[1:] if tag.startswith("v") else tag
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", stripped)
    if not match:
        raise SystemExit(f"error: tag '{tag}' is not of the form v<major>.<minor>.<patch>")
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)))


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", help="also verify that this git tag matches, e.g. v0.1.0")
    parser.add_argument("-q", "--quiet", action="store_true")
    arguments = parser.parse_args(argv)

    cmake_version = parse_cmake_version()
    header_version = parse_header_version()

    problems = []

    if cmake_version != header_version:
        problems.append(
            "error: version mismatch: %s says %s but %s says %s"
            % (CMAKE_LISTS, version_text(cmake_version), VERSION_HEADER, version_text(header_version))
        )

    if arguments.tag:
        tag_version = normalize_tag(arguments.tag)
        if tag_version != cmake_version:
            problems.append(
                "error: tag version %s does not match the project version %s"
                % (version_text(tag_version), version_text(cmake_version))
            )

    changelog_problem = check_changelog(cmake_version)
    if changelog_problem:
        problems.append(changelog_problem)

    if problems:
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1

    if not arguments.quiet:
        print("version: %s (consistent)" % version_text(cmake_version))
        print("  %s" % CMAKE_LISTS.relative_to(REPOSITORY_ROOT))
        print("  %s" % VERSION_HEADER.relative_to(REPOSITORY_ROOT))
        if CHANGELOG.is_file():
            print("  %s" % CHANGELOG.relative_to(REPOSITORY_ROOT))
        if arguments.tag:
            print("  tag %s" % arguments.tag)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
