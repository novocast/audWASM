# Contributing

## Branch naming

`<milestone>/<short-description>`, e.g. `m04-waveform/peak-rms-extraction`. For cross-cutting work
not tied to a single milestone, `chore/<short-description>` or `fix/<short-description>`.

## Before opening a PR

- Read `documentation/tasks/M00-foundations-and-conventions.md`. It is normative: if code disagrees
  with it, the code is wrong or M00 needs a PR of its own first.
- `cmake --build --preset native-debug && ctest --preset native-debug` passes locally.
- `clang-format` and Prettier are clean (`git diff --check`-style — CI's `lint` job is authoritative).
- New engine code lives under the right `engine/<subsystem>/` per M00 §4's layout, and does not
  `#include <emscripten.h>` anywhere (CI greps for this).
- New analysers conform to the `Analyzer` shape in M00 §6, even before M20's registry exists.
- Errors from anything a malformed input file can trigger are `aud::Error`, never `AUD_ASSERT`.

## Review checklist

- [ ] Does this change anything M00 declares as a **Decision**? If so, the PR must edit M00 (and
      `00-INDEX.md` if the decision is summarised there) in the same diff — decisions don't drift
      silently into code.
- [ ] Are new allocations on hot/input-driven paths going through `aud::tryAllocate` rather than
      `new`/`malloc` directly?
- [ ] Do new `Result<T>`-returning functions actually use `AUD_TRY`/`AUD_TRY_ASSIGN` for
      early-return rather than manual `if (!has_value())` chains?
- [ ] Are third-party dependencies vendored per `third_party/VERSIONS.md`'s procedure, not adhoc?
- [ ] Does the PR update the task file's checklist (`- [ ]` → `- [x]`) for whatever it completes?
- [ ] Are Embind additions minimal and bulk-oriented (no per-sample calls, no returning large
      arrays by value) per M01's boundary rules?

## Commit messages

Focus on *why*, not *what* — the diff already shows what changed.
