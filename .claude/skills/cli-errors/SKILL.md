---
name: cli-errors
description: CLI discipline for Raiden Pico. (1) Every command/sub-command/argument MUST emit an explicit error on unrecognized, mistyped, or malformed input — never silently no-op or fall through to a wrong/default branch. (2) Every CLI add/change/rename MUST update the config_none pytest regression tests AND the docs/HELP in the same change. Invoke whenever you add, change, rename, or review a CLI command path in src/command_parser.c (verbs, sub-commands, argument parsing, dispatch branches) or the matchers in command_parser_match / match_and_replace.
---

# Two rules for every CLI change

Raiden Pico is driven entirely over the USB CDC serial CLI. Two things must hold
whenever you touch a command path.

## Rule 1 — every CLI command must error when entered wrong

A mistyped command that **silently does nothing — or worse, does the wrong
thing** — is dangerous: this is a fault-injection tool that switches power rails
and fires glitches. A user who fat-fingers a sub-command (say `TARGET POWER
EXTRENAL`) and gets no error will believe a mode switch happened when it did not,
and the next command operates on the wrong pins.

**Every command, sub-command, and argument MUST produce an explicit `ERROR: ...`
line on unrecognized / mistyped / malformed input. Never silently no-op, and
never fall through to a default or unrelated branch.**

### The footgun that motivated this skill

`command_parser_match()` (abbreviation matcher) returns the **original string**
on no match, not `NULL`:

```c
} else {
    return abbrev;  // No match, return original   <-- lenient passthrough
}
```

That passthrough is intentional for *keyword-or-literal* call sites (where the
token may be a name OR an address). But it means a bare keyword gate that only
checks `matched == NULL` will accept any garbage token. The gate
`match_and_replace()` therefore rejects the no-match passthrough explicitly:

```c
if (matched == NULL)   { /* ambiguous abbreviation */ return false; }
if (matched == *part)  { /* no candidate matched   */ return false; } // <-- required
```

**When you add an enumerated sub-command set, route it through
`match_and_replace()`** (not a bare `command_parser_match()` + `strcmp` chain) so
the unknown-token error is automatic.

### Checklist (Rule 1)

- [ ] Unknown verb/sub-command → `ERROR:` (use `match_and_replace()` for the
      enumerated set; don't hand-roll a `strcmp` chain with no `else` error).
- [ ] After an `if/else if` dispatch over sub-commands, the **final `else`
      emits an error** — no silent fall-through. (e.g. the
      `TARGET POWER ON/OFF/CYCLE/INTERNAL/EXTERNAL` chain must error on anything else.)
- [ ] Bad/garbage arguments error (numeric parse failure via `parse_u32` etc.,
      out-of-range values, missing required args) — don't substitute a silent
      default for an argument the user clearly tried to supply.
- [ ] A sub-command that takes its own sub-tokens (e.g. `EXT AHIGH|ALOW`)
      validates each level — a wrong token at any depth errors.
- [ ] The error names the offending token and the context, e.g.
      `ERROR: Unknown POWER command 'WIBBLE'`, so the user sees *what* was wrong.
- [ ] Don't confuse "ambiguous abbreviation" (multiple candidates match a prefix)
      with "unknown" (no candidate matches) — both must error, with distinct wording.

## Rule 2 — every CLI change updates the regression tests AND docs

A new, changed, or renamed command isn't done until its behavior is covered by the
USB-only `config_none` pytest suite and reflected in the docs. (This is how the
`TARGET POWER MODE X` → `TARGET POWER X` rename was kept honest.)

**In the SAME change you MUST:**
- Add/update a test (asserts on the CLI text response) for at least the **happy
  path** (set/query returns the expected substring) and the **error path** (a bad
  token/arg returns `ERROR`). New subsystem → its own file (e.g.
  `tests/test_power_mode.py`); a tweak to an existing command → the relevant class
  in `tests/test_config_none.py`.
- **Pick the right gate by SAFETY, not convenience.** A command that only echoes
  text / changes harmless state → `config_none` (default run). A command that
  **drives hardware which could damage or mis-read a connected target** — the
  `TARGET POWER` modes, the crowbar gate, power on/off/cycle — MUST be gated behind
  the matching `--config` marker (`config_power_int` / `config_power_ext`) and
  must NOT run under `config_none`. See the SAFETY note in `tests/conftest.py`.
- On a **rename/removal**, add a test that the **old syntax now errors** (so the
  change is observable and can't silently regress) — e.g. after dropping the `MODE`
  keyword, `TARGET POWER MODE EXT` must return `ERROR: Unknown ...`. (An error-path
  test doesn't drive hardware, so it can stay in the lighter gate.)
- Update the `HELP`/usage strings and docs (`README.md` CLI reference, and any
  relevant wiring docs) to the new syntax — and any host scripts that send
  the command.
- Re-run green against **freshly-flashed** firmware: `pytest tests/ -v` for
  `config_none`, plus the gated suite for what you touched (e.g.
  `--config=power-int` / `--config=power-ext`) with the bench wired accordingly.
  Needs the device on `/dev/ttyACM0`.

### Checklist (Rule 2)

- [ ] config_none test covers the new/changed command's happy path.
- [ ] config_none test covers its error path(s) (bad token, bad arg → `ERROR`).
- [ ] On rename/removal: a test asserts the OLD form now errors.
- [ ] HELP/usage + README (+ wiring docs + affected host scripts) updated.
- [ ] Suite re-run green against freshly-flashed firmware.

## What is NOT an error

- A bare query form is fine when it's the documented behavior (e.g.
  `TARGET POWER` with no argument *prints* the power state + mode). That's an
  intentional zero-argument branch, not a fall-through — keep it explicit.
- Legitimate keyword-or-literal tokens (a name that may instead be a `0x…`
  address) may use `command_parser_match()` directly and treat the passthrough
  as "it's a literal, now validate it as one" — but then the literal path must
  itself error if the literal is invalid.

## When NOT to invoke

- Changes that don't touch CLI parsing/dispatch (PIO programs, pure hardware
  init, host-side Python unrelated to commands, docs-only edits).
