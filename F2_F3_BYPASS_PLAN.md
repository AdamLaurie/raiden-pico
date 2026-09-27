# STM32F2 / F3 RDP1 BYPASS — Plan

Extends the F4 RDP1 BYPASS work (PR #9, `feat/f4-bypass-and-pico2w-led`, v0.10)
to the STM32F2 and STM32F3 families, reusing the same per-family payload
architecture. **Step 0 first: validate the deep-sleep debug-kill on a
known-working F1** before committing to a method for F2/F3.

## Step 0 — RESOLVED (2026-09-27): deep-sleep FAILS, use POR-glitch BYPASS

Tested on the known-good F1 (POR-glitch BYPASS proven as control). **Result:
deep-sleep debug-disconnect is real but does NOT bypass RDP1.**

- **Deep sleep DOES disconnect debug (clean test, `TARGET GLITCH CLEANWAKE`).**
  The core was resumed with `C_DEBUGEN=1` and the payload never writes DHCSR; after
  its own autonomous STOP/wake it read back `DHCSR=0x01010000` → `C_DEBUGEN=0`. The
  only thing between the two is the sleep, so on F1 STOP genuinely clears
  C_DEBUGEN. (The *earlier* HALT test's `C_DEBUGEN=0` was a confound — that path
  clears C_DEBUGEN twice before the STOP: a DAP write in `target_power_halt` and
  stage 1's own clear at `rdp_bypass_diag.S:88-90`. CLEANWAKE removes both, so its
  result is real.)
- **But debug-disconnect is NOT the RDP gate.** With `C_DEBUGEN=0` and no debugger
  attached, the flash read still faults: `FLASH_OBR=0x03FFFFFE RDPRT=1`
  (`DATA`+`"FAULT"`). RDP1 is the flash-controller POR latch, independent of debug
  state — detaching/sleeping/waking never touches it. Only the POR voltage glitch
  corrupts it (control: BYPASS on the same unit dumps `DEADBEEF`).
- Ruled out the read *method*: switched the diag payload's stage 2 from the FPB
  "reader" trick to a plain `ldr r3,[r5]` — still faults. So the fault is RDP
  enforcement in the flash controller, not the reader trick.

**Why:** RDP1 on F1 is enforced by the flash controller, whose RDP state is
latched from the option bytes at POR. A *clean* reset (STOP/STANDBY wake) re-reads
the option bytes and re-applies RDP1 — it cannot corrupt that latch. Only the POR
*voltage glitch* corrupts the latch mid-latching, which is why BYPASS works and no
low-power mode can. Confirms the prior `project_todo_run_rdp_resettest` finding.

**Decision:** F2/F3 use the POR-glitch BYPASS with per-family payloads (below).
Deep-sleep is dropped. (`TARGET GLITCH HALT` remains a useful diagnostic — it
returns the register snapshot above — but is not an RDP bypass.)

## (historical) Step 0 rationale — validate the deep-sleep method on a known-good F1

An RDP1 flash dump needs the debug connection dropped so the flash controller
permits reads, while the SRAM payload survives. Two ways to sever debug:

- **(a) POR glitch** — the current `TARGET GLITCH BYPASS`: brief brownout resets
  the RDP latch, SRAM retained by decoupling; stage 1 SRAM-boots and configures
  FPB. Proven on F1 and (per protocol) F4.
- **(b) Deep-sleep / STOP-mode debug-domain power-down** — payload stage 1 enters
  STOP (or STANDBY) with the debug clock gated (`DBGMCU` STOP bit clear), the
  debug domain powers down and clears `C_DEBUGEN`, then wakes (RTC alarm) with
  SRAM intact and reads flash. **No glitch hardware needed** — deterministic.

**Why revisit:** earlier notes say STOP/IWDG/WWDG/STANDBY+wake all *failed* to
bypass RDP1 on F1 (`project_todo_run_rdp_resettest`) and "STOP never worked."
But those attempts may have been confounded (broken payload, wrong debug-clear
sequence, F4-only bugs). Retest cleanly on an **F1 where POR-glitch BYPASS is a
proven control**, so a pass/fail is trustworthy.

**Decision gate:**
- If deep-sleep dumps flash on F1 → adopt it for F2/F3 (no glitch, simpler rig)
  and keep POR-glitch BYPASS as the fallback.
- If it fails on F1 too → F2/F3 use the POR-glitch BYPASS with per-family
  payloads (below), and we stop chasing deep-sleep.

**Test rig:** F1 at RDP1 (known-good), payload stage 1 = enter STOP with
`SCB->SCR SLEEPDEEP`, `PWR_CR PDDS=0`, `DBGMCU_CR` STOP/STANDBY/SLEEP bits
cleared, RTC alarm wake (~2 ms); stage 2 reads `0x08000000` and streams it out.
Compare against the known-good POR-glitch dump on the same unit.

## Carry-over from PR #9 (the F4 pattern to reuse)

- **Command:** `TARGET GLITCH BYPASS` → SWD-upload payload → `BOOT0/1=1` → POR
  glitch SRAM-boots stage 1 (configures FPB) → `BOOT0=0` + nRST → stage 2 dumps
  flash over UART. Protocol on the wire: `RDP1` + CPUID + flash bytes @ 115200.
- **Per-family selector:** `get_rdp_bypass_payload()` in `src/target_uart.c`
  returns `{payload, size, load_base}` per `target_type_t`. F1 + F4 done; F2/F3
  currently return an explicit error. Add F2/F3 entries here.
- **FPB "reader" trick:** F4 needs a 2nd FPB comparator that remaps a flash-range
  instruction fetch (0x08000100) to an SRAM reader (`ldr r0,[r0]; bx lr`), so the
  PC sits in flash range and the flash controller permits the read. **F1 does NOT
  need this** (F1 SRAM code can read flash under RDP1). The F2/F4 families add the
  extra protection — so F2 and F3 almost certainly need the reader trick too
  (verify at the bench).
- **Payload build:** `stm32_payloads/<fam>/rdp_bypass.S` → `objcopy -O binary`
  → `xxd -i` into `rdp_bypass_<fam>_hex.h`, linked at the SRAM load base
  (`--section-start=.text=0x20000000`). Makefile rule mirrors `stm32_payloads/f4`.
- **Tests/docs:** extend `TestBypassPayloadFamily` (config_none), update HELP
  (`[STM32F1/F2/F3/F4]`), README, CHANGELOG; bump version.

## STM32F2 (Cortex-M3 — like F1, but F2/F4 RDP1 protection)

- **New target type:** F2 is not in `target_type_t` yet. Add `TARGET_STM32F2`
  (config.h) + a `stm32_target_info_t` entry (stm32_target.c) for F205/F207:
  flash `0x08000000`, SRAM `0x20000000`, `FLASH_OPTCR 0x40023C14`, flash size,
  RDP bytes. Add `SET TARGET STM32F2` to `command_parser.c` (match list + HELP +
  config_none test).
- **Peripheral map:** F2 ≈ F4 — GPIO on AHB1 `0x40020000` (MODER/AFR), RCC
  `0x40023800`, USART1 `0x40011000`. So the F2 payload is close to the F4 payload
  (USART1 PA9/AF7 @115200), just `.cpu cortex-m3`.
- **Reader trick:** yes (F2 blocks SRAM-code flash reads under RDP1) — verify.
- **Payload:** port `f4/rdp_bypass.S` → `f2/rdp_bypass.S` (M3 core, same
  peripheral map, reader trick, load base per BYPASS SRAM-boot).

## STM32F3 (Cortex-M4 — like F4, different peripheral map)

- **Target type:** `TARGET_STM32F3` already exists.
- **Peripheral map differs from F4** — verify against RM0316 (F303): GPIO is on
  **AHB2 at `0x48000000`** (not 0x40020000), RCC `0x40021000`, USART1 `0x40013800`
  (APB2). So the F3 payload needs its own addresses — do NOT copy F4's blindly.
  F3 flash is small (~64 KB per the target info).
- **Reader trick:** likely needed (Cortex-M4, F-series RDP1) — verify.
- **Payload:** `f3/rdp_bypass.S` with the F3 map + reader trick.

## Open questions to settle at the bench

- Does the FPB reader trick apply to F2 and F3? (F1 no, F4 yes.)
- F2/F3 SRAM load base for the BYPASS SRAM-boot (F4 uses 0x20000000).
- F2/F3 BOOT1 pin behaviour for SRAM-boot mode.
- **Deep-sleep viability (Step 0)** — decides POR-glitch vs deep-sleep for F2/F3.

## Sequence

1. **Step 0:** deep-sleep method on known-good F1 → decision gate.
2. F2 target type + payload (peripheral map ≈ F4) → bench-test on an F2 at RDP1.
3. F3 payload (F3-specific map) → bench-test on an F3 at RDP1.
4. Per-family selector + tests + docs + version bump for each.
