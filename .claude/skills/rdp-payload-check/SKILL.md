---
name: rdp-payload-check
description: Verification discipline for RDP flash-dump payloads (STM32 bypass / halt / cleanwake / future F2-F4). Any new or modified payload that reads flash MUST first be proven to read real flash at RDP0 (unprotected) before ANY conclusion is drawn from its behavior at RDP1. A fault at RDP1 is meaningless until the payload is RDP0-proven — you cannot tell "bypass failed" from "payload broken." Invoke whenever writing/editing an RDP payload (stm32_payloads/**), wiring a new *_bypass/halt/cleanwake path, or about to report an RDP1 result.
---

# RDP payloads are guilty until proven at RDP0

An RDP-bypass payload has two independent ways to fail:

1. **The bypass didn't work** — RDP is still enforcing, the read is blocked.
2. **The payload itself is broken** — bad assembly, wrong load base, wrong entry
   offset, USART misconfigured, faults before it ever reads flash, reads the wrong
   address, etc.

At **RDP1 both look identical**: you get `FAULT` / garbage / silence either way. So
an RDP1 result — success *or* failure — tells you nothing until you have first
eliminated cause (2). This session's mistake was exactly this: `CLEANWAKE` and the
`HALT` diag payload were only ever run at RDP1, faulted, and the fault was
attributed to RDP enforcement. That conclusion happened to hold (BYPASS is a
byte-identical control that dumps `DEADBEEF`), but it was not *proven* — a broken
payload would have produced the same fault.

## The rule

**Every payload that reads flash for RDP work MUST be proven at RDP0 (control)
before any RDP1 behaviour is trusted or reported.**

- **Phase A — RDP0 control (do this FIRST).** Run the payload on an *unprotected*
  target. It MUST return **real flash content** (a known, non-`0xFF` pattern — see
  below). If it faults, hangs, returns `0xFF`/garbage, or reads the wrong data →
  the payload is broken. Fix it and repeat Phase A. Do NOT proceed.
- **Phase B — RDP1 experiment.** Only once Phase A passes, run on the locked
  target. NOW the result is meaningful: real flash = bypass worked; fault = RDP
  enforced (bypass did not work) — *and you can say so, because the payload is
  known-good*.

A payload that has passed at RDP1 by returning real, correct flash (e.g. BYPASS →
`DEADBEEF`) is *already* proven — success at RDP1 cannot come from a broken read,
so it needs no separate Phase A. It is the payloads that have only ever **faulted**
that are unverified.

## What counts as an RDP0 pass

The RDP0 target must hold a **known, distinctive, non-erased pattern** so a real
read is unambiguous:

- A blank/mass-erased chip reads `0xFF` everywhere — that is INDISTINGUISHABLE from
  some fault modes. `0xFF` is NOT a pass.
- Flash a known marker (e.g. a `0xDEADBEEF` fill, or real firmware with a known
  reset vector `0x08000000..3` = SP, `..4..7` = reset handler) and confirm the
  payload returns exactly those bytes.

## Preparing an RDP0 target

- Prefer a **dedicated unlocked unit** kept for payload validation, pre-loaded with
  a known marker pattern.
- Or unlock the locked unit: `SWD RDP SET 0 WIPE` / `TARGET BL RU WIPE` — this
  **mass-erases all flash** (destructive; loses any `DEADBEEF` test content). It is
  a destructive op: requires the `WIPE` safe word AND explicit human confirmation
  (see the destructive-tests rule), and you must re-flash a known marker afterward
  before Phase A means anything.

## Record it

When a payload passes Phase A, note it next to the payload (source header comment
and/or the relevant campaign doc / memory): date, target, address read, and the
bytes returned. An RDP1 conclusion in a commit/doc should be able to point at the
RDP0 pass that licenses it.

## Current backlog (payloads NOT yet RDP0-proven)

- `stm32_payloads/f1/rdp_cleanwake.S` (`TARGET GLITCH CLEANWAKE`) — only faulted at
  RDP1.
- `stm32_payloads/f1/rdp_bypass_diag.S` (`TARGET GLITCH HALT`) — only faulted at
  RDP1 (both the old FPB-reader-trick and the current direct-read version).
- Check any other flash-reading diag payloads (`rdp_literal`, `rdp_regdump`,
  `rdp_resettest`) the same way before citing their results.
- `stm32_payloads/f1/rdp_bypass.S` (BYPASS) is already proven (dumps `DEADBEEF` at
  RDP1). F2/F3/F4 payloads, when built, start at Phase A.
