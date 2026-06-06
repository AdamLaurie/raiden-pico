---
name: cli-errors
description: CLI input-validation rule for Raiden Pico — every command and sub-command MUST emit an explicit error on unrecognized, mistyped, or malformed input; it must never silently no-op or fall through to a wrong/default action. Invoke whenever you add, change, or review a CLI command path in src/command_parser.c (new verbs, sub-commands, argument parsing, dispatch branches) or the matchers in command_parser_match / match_and_replace.
---

# Every CLI command must error when entered wrong

Raiden Pico is driven entirely over the USB CDC serial CLI. A mistyped command
that **silently does nothing — or worse, does the wrong thing** — is dangerous:
this is a fault-injection tool that switches power rails and fires glitches. A
user who types `TARGET POWER EXT` (meaning `TARGET POWER MODE EXT`) and gets no
error will believe a mode switch happened when it did not, and the next command
operates on the wrong pins.

**Rule: every command, sub-command, and argument MUST produce an explicit
`ERROR: ...` line on unrecognized / mistyped / malformed input. Never silently
no-op, and never fall through to a default or unrelated branch.**

## The footgun that motivated this skill

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

## Checklist when adding or changing a CLI command

- [ ] Unknown verb/sub-command → `ERROR:` (use `match_and_replace()` for the
      enumerated set; don't hand-roll a `strcmp` chain with no `else` error).
- [ ] After an `if/else if` dispatch over sub-commands, the **final `else`
      emits an error** — there is no silent fall-through. (e.g. the
      `TARGET POWER ON/OFF/CYCLE/MODE` chain must error on anything else.)
- [ ] Bad/garbage arguments error (numeric parse failure via `parse_u32` etc.,
      out-of-range values, missing required args) — don't substitute a silent
      default for an argument the user clearly tried to supply.
- [ ] A sub-command that takes its own sub-tokens (e.g. `MODE INT|EXT`,
      `EXT AHIGH|ALOW`) validates each level — a wrong token at any depth errors.
- [ ] The error names the offending token and the context, e.g.
      `ERROR: Unknown POWER command 'EXT'`, so the user sees *what* was wrong.
- [ ] Don't confuse "ambiguous abbreviation" (multiple candidates match a prefix)
      with "unknown" (no candidate matches) — both must error, with distinct,
      accurate wording.

## What is NOT an error

- A bare query form is fine when it's the documented behavior (e.g.
  `TARGET POWER MODE` with no argument *prints* the current mode). That's an
  intentional zero-argument branch, not a fall-through — keep it explicit.
- Legitimate keyword-or-literal tokens (a name that may instead be a `0x…`
  address) may use `command_parser_match()` directly and treat the passthrough
  as "it's a literal, now validate it as one" — but then the literal path must
  itself error if the literal is invalid.

## When NOT to invoke

- Changes that don't touch CLI parsing/dispatch (PIO programs, pure hardware
  init, host-side Python, docs).
