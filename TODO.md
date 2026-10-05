# TODO

## Review PR: F4 BYPASS payload + per-family selection + Pico 2 W LED

Branch `feat/f4-bypass-and-pico2w-led` is up as a PR and needs review before
merge. Focus points for the reviewer:
- The F4 payload assumptions (load base 0x20000000, FPB reader trick, USART1/PA9
  @115200) are bench-unverified — see the bench-test item below.
- Per-family dispatch in `target_power_bypass` leaves F1 unchanged; confirm.
- Board-aware LED: `BOARD=pico2_w` no-ops the LED, no wireless pulled in.

## Bench-test the STM32F4 RDP1 BYPASS flash dump (needs target board)

**When:** next bench session, once STM32F4 target boards are available.

**Context:** BYPASS is the working RDP1 dump method (SWD upload → BOOT0/1=1 →
POR glitch SRAM-boots stage 1 → FPB → BOOT0=0 + nRST → stage 2 dumps). The gap
on F2/F3/F4 was never the method — it was the missing family-specific payload
(the F1 payload pokes F1 peripheral addresses and can't read flash on F4). This
round adds the F4 payload; RDP2→RDP1 downgrade is a separate later effort.

**What shipped (v0.8, UNTESTED on F4 silicon):**
- `stm32_payloads/f4/rdp_bypass.S` → `rdp_bypass_f4_hex.h`. F4 peripheral map,
  loads at 0x20000000, plain protocol (`RDP1` + CPUID + flash). Adds the FPB
  reader trick (2nd comparator remaps a flash-range fetch) because F4 blocks
  flash reads from SRAM-executing code under RDP1 — the one real F4 difference
  from the F1 payload.
- `get_rdp_bypass_payload()` per-family selector in `src/target_uart.c`, wired
  into `target_power_bypass`. F1 unchanged; F4 added; F2/F3 error explicitly.

**Test steps (needs F4 target board + power wiring — power-int gate):**
1. Wire an STM32F4 (F407/F427) at **RDP1**: SWD GP17/18, nRST GP15, BOOT0/BOOT1
   pins, target power group, target UART **USART1 TX = PA9 (AF7)** → Pico target
   UART RX (GP5). ADC26 for the sweep.
2. `make flash`.
3. `TARGET STM32F4`, then `TARGET GLITCH SWEEP` (calibrate), then
   `TARGET GLITCH BYPASS`.
4. Expect: `RDP1 bypass: STM32F4 ...`, POR triggered, then `RDP1` + CPUID +
   flash hex dump.
5. Run the gated suite: `pytest tests/ -v --config=power-int` with the F4 wired.

**Config-none check (no target, run once Pico is connected):**
- `pytest tests/test_config_none.py -v` — `TestBypassPayloadFamily` asserts the
  unsupported-family error. Not yet run (no Pico was connected during the change).

**F4 items likely to need bench tuning (all in one place: the descriptor +
`rdp_bypass.S`):**
- **Load base / SRAM boot.** Payload is at 0x20000000 (SRAM-boot aliases SRAM to
  0x00000000, so the vector table must be there — same as F1). If F4 SRAM boot
  reserves low RAM or the existing 0x20004000 bootloader-Go path is preferred,
  change `load_base` in `get_rdp_bypass_payload()` and the absolute addresses in
  `rdp_bypass.S` together. (The old `payload.c` + `scripts/load_sram_payload.py`
  used 0x20004000 via bootloader Go — a different launch.)
- **BOOT1 pin.** Confirm the firmware's BOOT1_PIN maps to the F4 board's BOOT1
  (PB2 / option bit varies by part/package).
- **UART / baud.** USART1 PA9 @ 115200 assumes the 16 MHz HSI (`BRR=0x8B`).
  Adjust if the board's reset clock differs, or retarget the USART if PA9 is
  unavailable.
- **Reader trick.** Confirm the F4 flash controller permits the read with PC
  remapped into flash range (FP_COMP1 → 0x08000100).

## Revisit SLEEP/WAKE (STOP-mode debug kill) on the F1 board

Earlier we concluded "we never got STOP to work" and shelved the HALT path. But
those SLEEP/WAKE attempts may have failed on **F4 because the payload was broken**
(wrong peripheral map, no FPB reader trick), not because the STOP-mode
debug-domain power-down technique itself is invalid.

Re-test on the **F1 board, where BYPASS is known-good** so we have a working
control: with a correct F1 payload, does entering STOP (SLEEP) and waking clear
C_DEBUGEN / drop the debug connection enough to read flash? If it works on F1,
the technique is sound and the earlier F4 failure was the payload — worth
re-porting the STOP kill to F4 (see the F4 bench-test debug-kill note above).
If it fails on F1 too, STOP is genuinely a dead end and BYPASS (POR) stays the
only method.

## Try EMFI on the I2C-boot RDP gate (Joe Grand two-fault mapping)

**Context:** the I2CGATE / I2CPULSE rail-dip campaigns are closed negative
(~5500 RDP1 shots, zero false-ACKs) — see memory `project_f401_rdp1_fpb_trilemma`
and the Joe Grand reference (`reference_joe_grand_stm32_fault_injection`).
Grand/Wouters (REcon 2026, repo `joegrand/stm32-fault-injection`) bypass the same
class of gate — the RDP check inside the Read Memory (0x11) bootloader command —
with **EMFI, not voltage glitching**: ChipSHOUTER + per-device tuned probe
POSITION over the die (CNC-swept), re-glitched on every 256-byte read call,
triggered from the READ_MEMORY command byte's TX edge then refined with
power-analysis (SAD) triggering to kill UART jitter. Our I2C ACK-slot anchor
already solves their jitter problem with ns determinism; what we lack is the
**spatial axis** (a global rail dip hits every transistor → corruption-or-BOR,
never a surgical compare flip) and their shaped-dip mechanism.

**Attack to try:** EMFI probe over the F401 mule's die, fired at our I2C
cmd-byte ACK-slot anchor (reuse `i2c_bl_send_cmd_ackhook` / the I2CGATE
classification harness: ACK-read vs NACK vs BOR vs corruption), sweep probe XY
via the GRBL stage + ChipSHOUTER 300–350 V + width/offset like notebook 3a.
Calibration on an RDP0 mule first (flags A0..AF, per the 2026-10-04 redo), then
RDP1. Success = false-ACK on a gated 0x11 at RDP1.

**Open caveat:** our I2CPROBE halt-recon shows the F401 ROM core HardFaults while
the I2C slave serves commands autonomously — unknown whether the verdict path on
this part is CPU-executed (their F205/F427 per-command glitch working suggests
theirs is) and hence whether a spatial fault can flip it. That question is itself
worth answering: a zero-result across a swept spatial grid would be evidence the
F401's slave-hardware verdict isn't faultable this way.

**Also from the repo, unexplored here:** their glitch #1 (RDP2→RDP1 downgrade)
corrupts option-byte loading DURING POWER-UP (nRST-triggered, verified via JTAG
OPTCR 0x40023C14 SPRMOD bit) — a different attack point than anything we've tried
on this mule, and the relevant one if we ever meet an RDP2 part.

## Follow-ups

- **F2 / F3 BYPASS payloads.** Same pattern. F3 is Cortex-M4 (like F4, needs the
  reader trick). F2 is Cortex-M3 (like F1) but has **no target type** in
  `stm32_target.c` yet — add `TARGET_STM32F2` + its info + `SET TARGET STM32F2`.
- **RDP2 → RDP1 downgrade** (separate, harder): needs a power-signature trigger
  and a fast crowbar we don't have yet. (Note: the Joe Grand repo's glitch #1
  above is a proven recipe for exactly this on F2/F4 — power-up option-byte
  corruption, OPTCR SPRMOD as the success oracle.)
