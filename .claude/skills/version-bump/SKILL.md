---
name: version-bump
description: Firmware-change discipline for Raiden Pico — ANY change to the firmware (src/*.c, src/*.pio, include/*.h that alters the built raiden_pico.uf2's behavior or output) MUST bump the firmware version string in the SAME change. The version is what the VERSION command prints, in src/command_parser.c. Also: if any source file is touched between testing and committing/pushing, a full flash + retest is required before the push. Invoke whenever you edit firmware source, are about to build + flash a firmware change, or are about to commit/push firmware.
---

# Every firmware change bumps the version

The firmware version is the one string a user (and host tooling) reads to know
*what is actually on the chip*. If the binary's behavior changes but the version
doesn't, a freshly-flashed device is indistinguishable from the old build — you
can't tell whether a fix or feature is really on the part. This bites hardest on
this project because flashing is a manual drag-drop dance; the `VERSION` readout
is the only confirmation the right firmware landed.

**Rule: any change that alters the firmware's behavior or output MUST bump the
version in the SAME change (same commit as the code).**

## Where the version lives

A single string printed by the `VERSION` command:

```c
// src/command_parser.c — the VERSION handler
uart_cli_send("Raiden Pico Glitcher vX.Y\r\n");
```

Find it: `grep -n 'Glitcher v' src/command_parser.c`. **Keep the `Raiden Pico
Glitcher v` prefix** — host tooling and `tests/conftest.py` gate on the word
"Raiden", and STATUS/scripts surface this string.

## How to bump

- **Minor** (`vX.Y` → `vX.(Y+1)`) for a new feature/command or a behavior change
  (new CLI command, new mode, a refactor that changes timing/behavior/output).
- **Patch** (add a `.Z` field if needed) for a small bug fix.
- One bump per logical change/PR — don't double-bump for the same change.
  **Bump from the version that is LIVE on `main` (committed/pushed), not your local
  working-tree value** — if nothing's been pushed since the last release, the whole
  uncommitted batch is one bump (don't re-increment for each edit/flash cycle).

## Also update the CHANGELOG

Every version bump MUST add a matching entry to **`CHANGELOG.md`** in the same
change — that's the only place changes are tracked outside commit messages. Add a
`## [X.Y] — <date>` section with `Added` / `Changed` / `Fixed` / `Removed`
subsections as appropriate. Keep it user-facing (what the device now does), not a
code diff.

## Re-flash + retest if source changed after the last test

The proof that firmware works is only valid for the **exact binary that was on
the chip when you tested it**. Any edit to firmware source (`src/*.c`,
`src/*.pio`, `include/*.h`) after your last flash+test produces a *different*
binary — the earlier "tests passed" no longer applies to what you're about to
ship, however small the edit looks.

**Rule: if ANY source file is touched between testing and committing/pushing, you
MUST do a full flash of the freshly-built firmware and re-run the tests before
the push.** No exceptions for "it's a one-liner" or "it obviously can't matter" —
the whole point of the on-device test is to catch the case where it did. (This
skill exists because a "trivial" post-test edit — e.g. a missed code path in one
of several near-identical functions — shipped untested more than once.)

Applies to the commit/push, not just the edit: the sequence must end
`edit → build → flash → retest green → commit → push`, with no source edit after
the retest. If you amend a commit or add "just one more fix," the clock resets —
re-flash and retest again. Host-only edits (`tests/`, `scripts/`, `*.md`) don't
change the binary, so they need a test re-run but not a re-flash.

## Checklist

- [ ] Did this change alter firmware behavior or output (`src/*.c`, `src/*.pio`,
      `include/*.h`)? → bump the version.
- [ ] **No firmware source edited since the last flash+test?** If any was, re-flash
      the freshly-built firmware and re-run the tests green BEFORE committing/pushing.
- [ ] Version string in `src/command_parser.c` (VERSION handler) updated, bumped
      from the LIVE-on-`main` version (one bump per uncommitted batch).
- [ ] **`CHANGELOG.md` entry added** for the new version (Added/Changed/Fixed/Removed).
- [ ] The bump + changelog ride in the **same commit** as the firmware change.
- [ ] Flashed + verified `VERSION` on the device matches the bumped source
      (so you know the new firmware actually landed).

## When NOT to bump

- Docs-only changes (`README.md`, `*.md`), host-side Python (`scripts/`, `tests/`),
  or pure comment/whitespace edits that don't change behavior or output — these
  don't alter the firmware a user runs, so the version stays.
- If unsure: does the change alter what the device *does* or *prints*? If yes, bump.
