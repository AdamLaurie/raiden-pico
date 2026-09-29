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

## RDP0 Phase A status

- ✅ `stm32_payloads/f1/rdp_cleanwake.S` (`TARGET GLITCH CLEANWAKE`) — PASS
  2026-09-27 (marker 0xCAFEBABE returned; DATA…DONE, no FAULT).
- ✅ `stm32_payloads/f1/rdp_bypass_diag.S` (`TARGET GLITCH HALT`) — PASS 2026-09-27
  (marker 0xCAFEBABE dumped; "Dump complete", no FAULT).
- ✅ `stm32_payloads/f1/rdp_bypass.S` (BYPASS) — proven by success at RDP1 (dumps
  real flash / `DEADBEEF`); no separate Phase A needed.
- ⬜ Older diag payloads (`rdp_literal`, `rdp_regdump`, `rdp_resettest`) — run
  Phase A before citing their results if ever reused.
- ✅ `stm32_payloads/f4/rdp_bypass.S` (F4 BYPASS) — **Phase A PASS 2026-09-28** on
  F401RE @ RDP0: `TARGET GLITCH BYPASS 40 256` dumped `RDP1` header, CPUID
  0x410FC241 (Cortex-M4), and flash `0x08000000 = DEADBEEF` (+ 0xFF erased). Full
  two-stage path proven: POR-glitch→SRAM-boot stage1→FPB config→nRST warm-reset→
  stage2 via reset-vector remap→FPB reader-trick read→UART dump. The initial
  "RDP1 header not received" was NOT a payload bug — **nRST (GP15) was wired to
  CN7-5 (VDD) instead of the NRST pin**; moving it to CN8 NRST fixed it. RM0368
  §23.11 confirms the design: system reset (nRST/SYSRESETREQ) resets the core but
  NOT the debug/FPB (only PORRESETn does), so the FPB remap survives step-5's nRST.
  Now RDP1-ready — Phase B is re-lock to RDP1 + re-run BYPASS. See
  [[project_todo_f4_bypass_bench_test]].
- ⬜ F2/F3 payloads, when built — start at Phase A.
