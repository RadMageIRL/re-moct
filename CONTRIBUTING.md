# Contributing to RE-MOCT

Thanks for your interest. A few conventions keep the project consistent.

## Naming

Write the project as **RE-MOCT** (all caps, hyphen) in prose, docs, commit messages, and
UI text. Use lowercase `re-moct` **only** where it is a filesystem path or binary name
(e.g. `~/.config/re-moct/`, `remoct.exe`). Never write "Remoct".

## Building

See **[BUILD.md](BUILD.md)** for Windows (MSYS2 UCRT64) and Linux (Debian Trixie)
instructions. The CI matrix in `.github/workflows/ci.yml` is the source of truth for
dependencies and build steps.

## Discipline

RE-MOCT is developed **design-first**: for anything non-trivial - and always before
touching the audio-thread/concurrency-sensitive paths (crossfade, streaming, the ring
buffer) - write down the approach and the correctness argument before the code. New
parsing/protocol logic is validated **probe-first** with a standalone tool (see
[`tools/`](tools/)) before it is integrated.

Verify changes in **two layers**: the automated tests (`ctest`) *and* a real-world gate
(actually play/rip/stream the affected path). A green build is necessary, not sufficient.
Keep changes surgical and additive; explain *why* in the commit.

## Third-party code

If a change adds or updates a third-party dependency, update
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Be especially careful with copyleft or
non-FOSS components (FDK-AAC, LGPL libraries) - see that file's obligations section.
