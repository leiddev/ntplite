#!/usr/bin/env python3
"""Apply or verify clang-format across ntplite's C/C++ sources.

The same script is used by developers and by CI, so that "it passes locally"
always means "it passes in CI".

Examples
--------
    python scripts/format.py            # rewrite files in place
    python scripts/format.py --check    # verify only; exit 1 on any diff
    python scripts/format.py --list     # print the files that would be touched
    python scripts/format.py --check src/ntplite_c.cpp

Environment
-----------
    CLANG_FORMAT   absolute path to the clang-format binary to use
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Sequence

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

SOURCE_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc"}
)

# Directories that never contain hand-written sources.
SKIPPED_DIRECTORIES = frozenset(
    {
        ".git",
        ".github",
        ".vscode",
        "build",
        "build-",
        "_deps",
        "_install",
        "dist",
        "out",
        "install",
        "cmake-build-debug",
        "cmake-build-release",
    }
)

# clang-format renders the same file differently across major versions, so CI
# pins one and warns when a developer's binary disagrees.
PINNED_VERSION = "19.1.7"

FALLBACK_LOCATIONS = (
    r"C:\Program Files\LLVM\bin\clang-format.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe",
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\bin\clang-format.exe",
    "/usr/bin/clang-format",
    "/usr/local/bin/clang-format",
    "/opt/homebrew/bin/clang-format",
)


def find_clang_format() -> str:
    """Locate a usable clang-format binary, or explain how to get one."""
    override = os.environ.get("CLANG_FORMAT")
    if override:
        if Path(override).is_file() or shutil.which(override):
            return override
        raise SystemExit(f"CLANG_FORMAT is set to '{override}', which does not exist")

    for name in ("clang-format", "clang-format.exe"):
        located = shutil.which(name)
        if located:
            return located

    for candidate in FALLBACK_LOCATIONS:
        if Path(candidate).is_file():
            return candidate

    raise SystemExit(
        "clang-format was not found.\n"
        "  * install it (pip install clang-format==%s), or\n"
        "  * set the CLANG_FORMAT environment variable to its full path." % PINNED_VERSION
    )


def clang_format_version(executable: str) -> str:
    try:
        completed = subprocess.run(
            [executable, "--version"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            universal_newlines=True,
        )
    except OSError as error:  # pragma: no cover - environmental failure
        raise SystemExit(f"failed to run '{executable}': {error}")

    text = (completed.stdout or "").strip()
    for token in text.split():
        if token[:1].isdigit():
            return token
    return "unknown"


def is_skipped(path: Path) -> bool:
    relative_parts = path.relative_to(REPOSITORY_ROOT).parts
    return any(part in SKIPPED_DIRECTORIES or part.startswith("build") for part in relative_parts)


def collect_sources(explicit: Sequence[str]) -> List[Path]:
    """Return the files to format, sorted for reproducible output."""
    if explicit:
        candidates = [Path(item) for item in explicit]
        files = []
        for candidate in candidates:
            if not candidate.is_absolute():
                candidate = REPOSITORY_ROOT / candidate
            if candidate.is_dir():
                files.extend(
                    entry
                    for entry in candidate.rglob("*")
                    if entry.is_file()
                    and entry.suffix in SOURCE_SUFFIXES
                    and not is_skipped(entry)
                )
            elif candidate.is_file():
                files.append(candidate)
            else:
                raise SystemExit(f"no such file or directory: {candidate}")
        return sorted(set(files))

    return sorted(
        entry
        for entry in REPOSITORY_ROOT.rglob("*")
        if entry.is_file()
        and entry.suffix in SOURCE_SUFFIXES
        and not is_skipped(entry)
    )


def process(executable: str, path: Path, check: bool) -> bool:
    """Format (or verify) one file.  Returns True when the file is compliant."""
    command = [executable, "-style=file", "--fallback-style=none"]

    if check:
        command += ["--dry-run", "--Werror"]
    else:
        command += ["-i"]

    command.append(str(path))

    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
        universal_newlines=True,
    )

    output = (completed.stdout or "").strip()

    if completed.returncode == 0:
        return True

    relative = path.relative_to(REPOSITORY_ROOT)
    print(f"{'would reformat' if check else 'formatting failed'}: {relative}")
    if output:
        for line in output.splitlines():
            print(f"    {line}")
    return False


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Apply or verify clang-format for ntplite.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("paths", nargs="*", help="files or directories (default: whole repository)")
    parser.add_argument("-c", "--check", action="store_true", help="do not modify; exit 1 on diff")
    parser.add_argument("-l", "--list", action="store_true", help="list the files that would be processed")
    parser.add_argument("-q", "--quiet", action="store_true", help="only report problems")
    arguments = parser.parse_args(argv)

    executable = find_clang_format()
    version = clang_format_version(executable)

    if not arguments.quiet:
        print(f"clang-format: {executable} (version {version})")
        if version != PINNED_VERSION and version != "unknown":
            print(
                f"  warning: CI pins clang-format {PINNED_VERSION}; formatting produced by "
                f"{version} may differ."
            )

    sources = collect_sources(arguments.paths)

    if arguments.list:
        for path in sources:
            print(path.relative_to(REPOSITORY_ROOT).as_posix())
        print(f"\n{len(sources)} file(s)")
        return 0

    if not sources:
        print("no C/C++ sources found", file=sys.stderr)
        return 1

    failures = [path for path in sources if not process(executable, path, arguments.check)]

    if arguments.check:
        if failures:
            print(
                f"\n{len(failures)} file(s) are not clang-formatted.\n"
                f"Run 'python scripts/format.py' to fix them."
            )
            return 1
        if not arguments.quiet:
            print(f"\nall {len(sources)} file(s) are correctly formatted")
        return 0

    if failures:
        print(f"\n{len(failures)} file(s) could not be formatted")
        return 1

    if not arguments.quiet:
        print(f"\nformatted {len(sources)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
