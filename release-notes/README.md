# Release notes

One file per release, named after the git tag:

```
release-notes/
├── README.md          this file
└── v0.1.0.md          the notes published for tag v0.1.0
```

## Convention

* File name **must** match the tag exactly: tag `v1.2.3` → `release-notes/v1.2.3.md`.
  `.github/workflows/release.yml` uses the file as the body of the GitHub
  release and fails the job if it is missing.
* Write for a human deciding whether to upgrade, not for a diff reader. Lead
  with what changed *for them*, then list details.
* Keep the same section order as the template below.

## Template

```markdown
# ntplite vX.Y.Z

One or two sentences: what this release is about.

## Highlights

- The one or two things a user should care about.

## Added

- ...

## Changed

- ...

## Fixed

- ...

## Breaking changes

- ... (omit the section entirely when there are none)

## Upgrading

- ... (omit the section entirely when nothing needs doing)

## Compatibility

| | |
| --- | --- |
| C ABI version | `NTP_LITE_C_API_VERSION` = N |
| Minimum C++ | C++11 |
| Verified toolchains | MSVC 19.4x, GCC 11, Clang 14+ |

## Acknowledgements

- ... (omit when empty)
```

## Checklist before tagging

1. `python scripts/check_version_sync.py` passes.
2. The version is bumped in `CMakeLists.txt` **and**
   `include/ntplite/version.hpp`.
3. `CHANGELOG.md` has a dated section for the new version.
4. The notes file exists and the tag it names is the tag you are about to push.
5. `ctest` is green on Windows (MSVC) and Linux (GCC/Clang).
6. If `NTP_LITE_C_API_VERSION` changed, that is called out under
   *Breaking changes*.
