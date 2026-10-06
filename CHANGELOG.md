## [0.18] — 2026-10-06 — Bootloader sync fixes + SHADOWTRACE POR characterisation

### Fixed — bootloader sync
- **`TARGET BL` auto-sync now runs the exact same sequence as explicit `TARGET SYNC`.**
  The two had diverged: the inline auto-sync copy **omitted `target_power_ensure_on()`**,
  so `TARGET BL GV/GET/GID` would NACK ("No response — run TARGET SYNC first") where an
  explicit `TARGET SYNC` beforehand succeeded. Both paths now call one shared
  `target_bl_sync_sequence()` helper, so they cannot drift apart again.
- **Stale `bootloader_synced` latch.** The flag was only cleared on a target-type
  change — never on a reset or power-cycle. So after any reset/power-cycle (including
  the ones SHADOWTRACE/SHADOWBYPASS perform) the Pico still believed it was synced,
  **skipped auto-sync**, and `TARGET BL` failed with "No response" without even trying.
  Now cleared in both `target_reset_execute()` and `target_power_cycle()`.
- **`TARGET I2C SYNC` desync.** The bus scan's bare address probe (address + STOP, no
  command frame) desynced the freshly-reset STM32 I2C bootloader, so SYNC's confirming
  Get then NACKed (while `TARGET I2C GET`, which skips the scan, worked). SYNC now
  re-enters the bootloader (clean reset) after the scan, before the Get.

### Added — SHADOWTRACE POR characterisation (`TARGET GLITCH SHADOWTRACE [samples] [reps] [hold_nrst]`)
- **`hold_nrst=1`** drives nRST low throughout the capture (core held in reset), to
  classify a trace feature as chip reset-exit activity vs a rig/rail/analog artifact.
  (Used to identify the dominant ~1.4ms POR dip as the reset-INDEPENDENT internal
  regulator/VCAP inrush — NOT the option-byte load.)
- **nRST-referenced output**: detects the GP15 nRST rising edge during capture and
  reports every row relative to it (`n=` column + `SHADOWTRACE_NRST_US:` line).
- **Un-clipped visualisation**: ASCII bars auto-zoom to the settled band (the rail-
  charge ramp no longer squashes plateau detail), plus a full-sample `SHADOWTRACE_CSV:`
  line for external plotting at arbitrary scale.

## [0.17] — 2026-10-05 — Fix RDP-campaign test apparatus + trustworthy I2CPROBE catch-state

### Added
- **I2CPROBE now reports catch-state** so its PC histogram is interpretable
  (audit gap): it reads **DHCSR before halting** (S_HALT/S_SLEEP → core was
  already stopped/waiting, PC is NOT a freshly-caught running addr) and **DFSR
  after** (HALTED bit → our halt caught it). Summary tallies clean-caught vs
  pre-HALTED vs pre-SLEEP vs EXC/faulted. Only clean-caught PCs locate running
  code; a histogram dominated by pre-HALT/SLEEP/EXC means the halt cannot catch
  the core there (and the earlier "core already in HardFault during I2C service"
  reading was a halt-intrusion artifact, not a target property).

### Fixed (test-validity bugs found in an audit of the v0.15 campaigns)
- **I2CPULSE PIO one-shot ignored BOTH sweep parameters.** `i2c_pulse_oneshot`
  (`glitch.pio`) read PAUSE via `out x,32` with autopull DISABLED, so `out`
  shifted stale OSR (= WIDTH) instead of the FIFO PAUSE, and the post-fire
  `mov x,osr` read an emptied OSR (= 0). Net: the LOW dip was a fixed ~1–2 cycles
  (~13 ns) at an offset tied to the WIDTH value, and the swept PAUSE/WIDTH did
  nothing. The ~4500-shot RDP1 result ("open-loop PIO sink cannot reach the
  corrupting band") therefore characterised a degenerate 13 ns pulse, not the
  intended window — **that negative is void.** Fixed: PAUSE is now pulled from the
  TX FIFO each shot (`pull block; out x,32`) and WIDTH is taken from Y (loaded once
  at start via `mov x,y`), so both parameters are live.
- **I2CGATE / I2CPULSE did not re-fire the warm-up frame after an in-loop
  re-entry.** The pre-loop ungated `0x02` warm-up (the first frame after a fresh
  entry always NACKs on a cold slave) was not repeated after the in-loop
  `i2c_bl_enter()` re-boots that follow a BOR/bus-dead. In an aggressive dip
  campaign (frequent BORs) the attempt right after each re-entry ate a guaranteed
  cold-slave NACK, which — with the rail dipped — was tallied as a "corrupted
  check HIT". Since RDP0 timing is calibrated by requiring corruption-rate > 0,
  this could validate the WRONG window and send the RDP1 search off-target. Fixed:
  a warm-up `0x02` frame is now fired after every re-entry in both campaigns.

### Changed — honest read classification (rdp-payload-check discipline)
- I2CGATE/I2CPULSE no longer count an **all-0xFF read as a "clean read / data path
  proven"** success. A blank/erased chip, a blocked read, and a poison return are
  indistinguishable at 0xFF, so all-FF is now reported as **INCONCLUSIVE**; only a
  **non-FF** read is meaningful (at RDP0 it must recover the known marker to prove
  the path; at RDP1 it is a dump). Summaries now split `non-FF/real` vs
  `all-FF/inconclusive` vs `read-fail`, warn when zero non-FF reads were seen
  (data path unproven), and no longer imply an RDP1 gated-NACK is a bypass.

## [0.16] — 2026-10-05 — SWD auto-attach-under-reset for sleeping targets

### Fixed
- **SWD now attaches to a running target that sleeps, without a manual `CONNECTRST`.**
  On a target whose firmware enters a low-power loop (`WFI`/`WFE`/STOP), the AHB
  bus clock is gated, so every AHB-AP *memory* access WAITs forever (ACK=0x2)
  even though the DP link and AP *register* access (IDR/CSW/TAR) stay alive. A
  plain `SWD CONNECT` therefore reported success while `IDCODE`/`READ`/`FLASH`/
  `HALT` all failed with "Could not read CPUID/debug registers". `swd_ensure_connected()`
  (the auto-connect used by every memory-touching SWD command) now probes real
  memory access with a DHCSR read after connecting; if it WAITs out, it escalates
  to connect-under-reset — vector-catching the core at the reset vector before
  firmware can re-enter the sleep loop — and re-checks. Verified on an
  STM32F103RB (Nucleo MB1136) whose firmware sleeps: plain `SWD IDCODE` now
  returns CPUID `0x411FC231` / STM32F1 Medium-density where it previously needed
  a hand-typed `SWD CONNECTRST`. The escalation is generic (any sleeping Cortex-M),
  only triggers when memory is otherwise unreachable, and prints a one-line notice
  when it halts the core under reset.
- **`SWD HALT` no longer resumes an already-halted core.** Its first step cleared
  C_HALT (enable-debug-without-halt) before requesting the halt; on a target that
  is only reachable while halted (sleeping/blank core caught under reset), that
  resume made it inaccessible and the re-halt timed out. `swd_halt()` now returns
  success immediately when the core is already halted (S_HALT + C_DEBUGEN set),
  which is also the correct answer for halt-when-already-halted. No change for a
  normally running target (it still falls through to the robust halt loop).

## [0.15] — 2026-09-30 — ROMGADGET: SRAM-boot ROM-gadget experiment (gate-2 mechanism)

### Added
- `TARGET GLITCH ROMGADGET [variant | 0xADDR]` — SRAM-boot stage2 -> boot-ROM gadget -> SWD recovery [F401]. Variants: 0=control (ROM read loop, SRAM source), 1=flash-plain, 2=flash+KEYR-unlock, 3=FPB reader-trick (stage2 programs FPB itself), 4=fetch-probe (blx a host-supplied ROM/flash address; `0xADDR` form), 5=data-read (stage2 loads from flash+ROM directly). Fault forensics in recovery: stacked-exception PC/LR, CFSR/HFSR/BFAR decode.
- `TARGET GLITCH ROMFPB` / `ROMFPBCTL` — FPB-remap of the boot-ROM RDP checker + gated I2C command exercise, and its no-patch control (concluded: FPB regs wiped by BOR; halt kills bootloader I2C).

### Changed
- `TARGET I2C GET` now enumerates each supported command with its AN4221 name (`00 GET | 01 GETVER | ... | 93 RU`) instead of a bare hex list; unknown codes print `?`.
- I2C bootloader timing property documented and bench-confirmed: the hardware-slave byte ACKs remove the USART path's milliseconds of sync/command/verdict jitter — frame boundaries are deterministic, which is what the I2CGATE/I2CPULSE campaigns use (STOP edge = t=0).
- ROMGADGET argument validation: non-numeric/garbage args and unknown variants emit explicit `ERROR:` lines (cli-errors rule).
- **`TARGET GLITCH I2CGATE [attempts] [mv]`** — brownout-dip the boot-ROM RDP check during the I2C command stretch, three rotating windows (cmd-byte / pre-STOP / post-STOP), verdict poll + full 16-byte read on ACK. Calibration mode at RDP0: NACK-on-dip = corrupted check, ACK + FF read = clean data path.
- **`TARGET GLITCH I2CPROBE [samples] [delay_us]`** — SWD-halt mid-stretch timing recon; dumps PC/xPSR + stacked fault frame + CFSR/BFAR. `delay_us 999999` = control (no 0x11 sent). Recon-only: a halt inside the read path gates the read.
- **`TARGET GLITCH I2CPULSE [attempts] [pause_lo] [pause_hi] [pause_step] [width]`** — PIO one-shot rail pulse (new `i2c_pulse_oneshot` program on the crowbar SM slot, INTERNAL mode): GP10 idles HIGH, dips LOW for `width` 6.67ns ticks after `pause` ticks; ns-resolution sweep. Re-anchored mid-campaign from the STOP edge to the cmd byte's ACK-slot SCL rise (new `i2c_bl_send_cmd_ackhook` primitive) — the verdict latches ~0.6us after that edge; post-frame hooks are ~400us too late.

### Findings (bench)
- I2CGATE calibration at RDP0 (200-shot run, 2.10V dips): post-STOP window corrupts the check ~47% (28 NACK / 31 ACK+clean-read); ≤2.3V never corrupts, ≤2.0V BORs (re-entry recovers). Data path proven: every ACK shot completed a clean read.
- I2CGATE at RDP1: 450 shots across 1.95–2.25V, zero false-ACKs — a µs rail dip flips pass→fail easily but cannot forge the precise `cmp == 0xAA00` false-pass. Rail-dip primitive ruled out for RDP1 bypass.
- I2CPROBE recon (RDP1 mule): the ROM core is ALREADY in HardFault (BFSR.IBUSERR at 0x1FFF03E2, the I2C wait-poll loop; timeout seed 0xAAAA via the 0x1FFF0C1C accept-helper) at every sampled delay 0µs–5ms — and even with no command sent — while the I2C slave hardware serves GET/GID/PROBE/verdicts autonomously. The gated-command verdict is NOT CPU-generated during the stretch; the "glitch the running rdp_locked() check" model was wrong for the I2C boot path.
- I2CPROBE operational notes: C_DEBUGEN survives nRST, so each probe sample must POR (power-cycle) before re-entering the bootloader; the first command frame after entry+GET always NACKs (warm-up frame fixes it); `delay_us` is capped at 1e6 (a huge delay busy-waits the whole main loop).
- I2CPULSE at RDP1 (~4500 shots, ACK-slot anchor, all geometries): zero ACKs, near-zero disturbance. Rail-depth map (50us ADC min-probe): divider geometry (1 sink vs 1 source) floors at ~2.71V width-independent — above the 2.1-2.28V corrupting band; zero-source geometry collapses the rail below BOR at ANY width (33ns suffices — no bulk holdup on this board). The open-loop PIO sink cannot reach the corrupting band; ADC feedback (the I2CGATE hook) is what makes a dip depth-controllable. A raw 0x02 liveness probe between shots wedges the slave's write path (alternating bus-dead) unless the full GETID response is drained — removed in favor of per-shot bus-dead logging.
- I2CPULSE operational notes: the gang pins must be re-released AFTER the entry sequence (power_drive() re-drives all three as SIO outputs), and re-ganged BEFORE i2c_bl_enter (its power-up uses gpio_set_mask, a no-op on input-released pins — released pins during re-entry left the target unpowered, a 100/100 bus-dead cascade).

# Changelog

All notable changes to the Raiden Pico firmware. The version is the string the
`VERSION` CLI command prints (defined in `src/command_parser.c`); bump it in the
same change (see the `version-bump` skill) and add an entry here.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/). This file
was started at v0.7, so pre-0.6 entries are summarized from git history.

## [0.14.1] — 2026-10-01 — Fix SWD IDCODE zero-masking + unreachable SWD DISCONNECT

### Fixed
- **`swd_detect()` no longer masks failed CPUID/DBG_IDCODE reads as success.** It
  discarded the return value of `mem_read32()` for both reads, so a transient
  AHB-AP read failure right after a plain (un-halted) `SWD CONNECT` — the AP can
  race the target's own bus activity immediately after debug-power-domain
  power-up — still returned `true` with the caller's zero-initialized values.
  `SWD IDCODE` then printed `CPUID: 0x00000000` / `Chip: Unknown` as if that were
  real (but blank) silicon, which reads as "target not enumerating" even though
  the DP/AP link is fine. Now propagates the read failure so the CLI reports
  `ERROR: Could not read CPUID/debug registers` instead. `SWD CONNECTRST` was
  never affected (it halts the core first, avoiding the race).
- **`SWD DISCONNECT` was unreachable.** Its handler existed
  (`command_parser.c`), but the `swd_subcmds[]` allow-list used for
  sub-command matching didn't include `"DISCONNECT"`, so every call was
  rejected as `ERROR: Unknown SWD sub-command 'DISCONNECT'` before reaching
  the handler — including the cleanup calls used throughout the SWD test
  suite, which never asserted on the response and so never caught it.

## [0.14] — 2026-09-29 — STM32 bootloader over I2C (bit-banged)

### Added
- **`SWD STEP [n]`** — single-step the target core (ARMv7-M mask-interrupts-then-step
  DHCSR sequence). **`SWD ROMREAD <addr> <len>`** — F401 boot-ROM gadget flash-dump
  probe (single-steps the ROM's own `ldrb` read gadget past its software RDP check).
- **`TARGET I2C <SCAN|SYNC|GET|GV|GID|READ|WRITE|GO|PROBE|ERASE|RP|RU>`** — talks to
  the STM32 system bootloader over a bit-banged I2C master (AN4221), to test the I2C
  boot interface the F401 boot ROM initialises but AN2606 doesn't document. Reuses
  the target UART1 pins (**GP4=SCL, GP5=SDA**) since the bootloader locks to one
  interface, so UART-boot and I2C-boot can never co-exist; on the target these are
  the ROM's **I2C1 = PB6/PB7** (not PB8/PB9). Default 7-bit slave address **0x39**
  (decoded from the ROM: `OAR1=0x4072`), overridable per command. Handles clock-
  stretching (80 ms budget; measured worst case ~34 ms on write-commit) and the
  AN4221 command/ACK framing. Full command set: `SCAN` (probe 0x08..0x77), `SYNC`
  (enter+scan+Get), `GET`/`GV`/`GID`, `READ <addr> <len>`, `WRITE <addr> <hex>`,
  `GO <addr>`, `PROBE <cmd_hex>` (gate mapping), and destructive `ERASE ALL WIPE` /
  `RP CONFIRM` / `RU WIPE` (confirm tokens). New `src/i2c_bootloader.c`.
- Config_none tests cover the argument-validation paths.

### Verified (bench, F401 DEV_ID 0x433)
- I2C boot interface **is reachable** on the F401 at slave 0x39 (undocumented in
  AN2606). At RDP0 the full command set works (Write DEADBEEF → READ-back verified).
- **RDP1 command gating mapped** (via `PROBE`): only **Get (0x00), GV (0x01),
  GID (0x02)** and the RDP-management pair **RP/RU** are accepted; **Read (0x11),
  Go (0x21), Write (0x31), Erase (0x44), Write-Protect (0x63) are all NACK'd**.
  Corrects an earlier hypothesis — there is **no command-level Write+Go bypass** at
  RDP1; the only remaining flash-read route stays the VCAP glitch of the ROM's
  Read-Memory RDP check.
- **Debugger-jump-to-ROM-gadget bypass tested and blocked.** At RDP1 the boot ROM
  (0x1FFF0000) is walled off the SWD debug port exactly like flash: AHB-AP reads
  fault (ACK=0x4) and the CPU won't execute ROM when PC is redirected there by the
  debugger (single-step retires nothing; SRAM steps fine as control). The 30 KB
  bootrom dump was only possible at RDP0, confirming ROM debug-access is RDP-gated.
  So the ROM's trusted flash read is reachable only via genuine boot flow — the
  glitch stays necessary.

## [0.13] — 2026-09-29 — SWD LEAKPROBE + flash-leak experiment (negative)

### Added
- **`SWD LEAKPROBE <addr>`** — atomic flash-read-leak probe: does a MEM-AP read of
  a (possibly RDP-blocked) address and captures the raw DRW data phase, RDBUFF, and
  sticky-error state with **no intervening error-clear**, so the per-command
  auto-clear can't wipe transient residue. Baselines with a known SRAM read to
  distinguish stale pipeline data from a real leak.

### Findings
- **Flash-read-leak experiment: NO LEAK (clean block).** Programmed an
  address-encoding pattern to flash at RDP0, re-locked to RDP1, and probed the CPU
  `ldr` path (HardFault, no data) and the debug MEM-AP path (`LEAKPROBE`: stale
  baseline + STICKYERR, no flash data). Flash data never leaves the flash-controller
  boundary. Recorded in `RDP1_DEBUG_MATRIX.md`.

## [0.12] — 2026-09-29 — SWD SCAN (DAP / CoreSight enumeration)

Feature branch (version tag distinguishes it from the parallel I2C branch; the
final number is assigned at merge).

### Added
- **`SWD SCAN`** — enumerates the ADIv5 DAP over SWD: Access Ports (reads each
  `AP_IDR`) and, for each MEM-AP, walks the CoreSight ROM table (`BASE` → entries
  → per-component `PIDR`/`CIDR`), naming components by their ARMv7-M debug base
  (SCS, DWT, FPB/BPU, ITM, TPIU, ETM). SWD-only — no JTAG needed. Powers up the
  debug/system domains first so AP reads work before any mem access.
  Bench-verified on STM32F401 at **both RDP0 and RDP1**: the debug topology
  enumerates identically when locked (PPB/ROM-table stays readable) even though
  application flash/SRAM MEM reads fault (ACK=0x4). Registered in the SWD matcher.

## [0.11] — 2026-09-28 — Explicit glitch-voltage arg + robust auto-detect

### Added
- **`[voltage_mv]` argument on `TARGET GLITCH BYPASS` and `SHADOWBYPASS`.** Establish
  the glitch depth once with `TARGET GLITCH SWEEP`, then pass it (millivolts, 0–3300)
  to skip re-sweeping every run. On BYPASS it sets the calibrated threshold directly
  and skips the sweep; on SHADOWBYPASS it ADC-gates the recovery dip to that depth
  instead of the legacy uncontrolled fixed-time low pull. Invalid/out-of-range values
  error at parse time (config_none regression tests added).

### Changed
- **`ensure_target_type()` now auto-detects the robust way `SWD IDCODE` does** —
  `target_power_ensure_on()` + `swd_ensure_connected()` + `swd_clear_errors()` before
  reading DEV_ID. The old bare `swd_connect()` path faulted (ACK=0x7) when a preceding
  glitch left the target mid-boot, so `SWEEP`/`BYPASS`/`SHADOWBYPASS` could no longer
  self-set the target. Now they recover and detect on their own.

## [0.10] — 2026-09-28 — STM32F4 RDP1 BYPASS + F4 flash-controller support

### Added
- **STM32F4 RDP1 BYPASS payload.** `TARGET GLITCH BYPASS` is now per-family via a
  `get_rdp_bypass_payload()` selector: F1 unchanged, **F4 added** (F4 peripheral
  map — RCC 0x40023800, GPIOA AHB1, USART1 PA9/AF7 @115200 — plus the FPB reader
  trick F4 needs to read flash from SRAM-executing code under RDP1). F2/F3 return
  an explicit error. Payload `stm32_payloads/f4/rdp_bypass.S`.
- **`TARGET GLITCH SHADOWCHAR [n]`** — M1 of the deterministic POR option-byte
  shadow-load glitch plan. Power-cycles the target and timestamps `t_vdd` (VDD
  rising through ~2.0 V on ADC GP26) and `t_nrst` (nRST/GP15 release after POR)
  over N iterations, reporting the `[t_vdd, t_nrst]` window (where the option-byte
  shadow load happens) plus nRST jitter — i.e. how lockable a timed glitch delay
  can be. Non-destructive; runs at RDP0 or RDP1.
- `rdp-payload-check` skill (prove an RDP flash payload reads real flash at RDP0
  before trusting any RDP1 result) and `tty-contention-check` skill (`fuser` the
  serial port before use — empty CLI reads usually mean a second terminal, not a
  dead device).

### Fixed
- **F4 flash-controller support** — SWD flash erase/write/fill worked on F1 but
  failed on F4. Fixed in `swd.c`: `flash_wait` reads BSY at **bit 16** (F4/L4), not
  bit 0; F4 erase/program clear the sticky SR error/EOP flags first; `flash_unlock`
  is **idempotent** (on F4, re-writing KEYR while already unlocked re-locks it — the
  double-unlock that broke erase+write). `SWD FILL` flash chunk capped at the 2 KB
  buffer (F4's 16 KB sector `page_size` over-read it). All 16 SWD subcommands now
  verified on an F401 (SRAM full 96 KB, flash, system memory, peripherals, regs).

## [0.9.1] — 2026-09-27 — RDP1 deep-sleep control test (research/diagnostic)

### Added
- **`TARGET GLITCH CLEANWAKE`** — a glitch-free control test for the STM32F1 RDP1
  investigation. Uploads an SRAM payload, resumes with `C_DEBUGEN=1`, detaches
  SWD, and lets the payload run autonomously: it announces itself (`CLN0`), enters
  STOP, self-wakes via RTC (~2 ms, `WAKE`), reports its own `DHCSR` + `FLASH_OBR`,
  then attempts a direct flash read. Payload: `stm32_payloads/f1/rdp_cleanwake.S`.

### Changed
- `TARGET GLITCH HALT` diag payload (`stm32_payloads/f1/rdp_bypass_diag.S`) now
  reads flash with a direct `ldr` instead of the F2/F4-only FPB "reader" trick
  (unneeded and fault-prone on F1).

### Findings (bench, F1 at RDP1; BYPASS as proven control)
- Deep sleep genuinely disconnects debug on F1: resumed with `C_DEBUGEN=1`, after
  the autonomous STOP/wake the payload read back `C_DEBUGEN=0` (it never writes
  DHCSR). **But this does not bypass RDP1** — `FLASH_OBR RDPRT=1` and the read
  faults. RDP1 is the flash-controller POR latch, independent of debug state; only
  the voltage glitch corrupts it. On F1, SRAM code reads flash directly once RDP is
  down — the FPB reader trick is an F2/F4 requirement, not F1.

## [0.9] — 2026-09-27 — External PSU control (TENMA / Multicomp Pro 72-2540)

### Added
- **External programmable PSU control.** New `PSU` command family
  (`VOLT/CURR/ON/OFF/STATUS/ID/RELEASE`) driving a TENMA 72-2540 / Korad-protocol
  supply over UART1 routed to GP10/11 (9600 8N1) via the RP2350 alternate
  funcsel. `src/psu.c` + `include/psu.h`. Needs a MAX3232 on the PSU's RS-232 DB9.
- Mutually exclusive with the GP10/11/12 target power group: a PSU command
  releases the power group and claims the pins; `TARGET POWER` refuses while the
  PSU holds them (run `PSU RELEASE`). New `power_group_release()` in target_uart.c.
- **Verified end-to-end on real hardware** through the Pico: Pico GP10/11 →
  YL-97 (MAX3232) → RS-232 DB9 → 72-2540. `PSU ID` returns the unit's identity
  (tested on `Multicomp Pro 72-2540 V6.1` and `TENMA 72-2540 V5.9`), `PSU VOLT`/
  `CURR` set and read back, `PSU ON`/`OFF` drive the output with the correct
  STATUS decode (bit0 CV/CC, bit4 beep, bit5 lock, bit6 output), `PSU STATUS`
  reports live Vout/Iout, and the `TARGET POWER` mutual-exclusion guard fires.
  Protocol: 9600 8N1, no terminator; `VSET1:NN.NN`/`ISET1:N.NNN` write formats.
  Wiring note: the RS-232 DB9 needs pin-5 GND common to the converter, and the
  converter↔PSU link must be wired for the DB9 orientation (both are DCE).
  config_none tests cover the CLI error/parse paths.

(Version 0.10 is the separate STM32F4 BYPASS branch, still bench-pending; the two branches
reconcile at merge time — this PSU work lands first as 0.9.)

## [0.8] — 2026-09-19 — Pico 2 W support + Target/GRBL UART bleed fix

### Added
- **Pico 2 W support.** The status LED is now board-aware via the SDK's
  `PICO_DEFAULT_LED_PIN` (GP25 on Pico 2). On the Pico 2 W the LED is on the
  CYW43 chip and GP25 is its chip-select (WL_CS), so the firmware no-ops the LED
  instead of driving GP25 — no wireless stack pulled in. New `BOARD=pico2_w`
  build option; `PINS` output is board-aware.

### Fixed
- **Target↔GRBL UART1 "TTL bleed."** `target_initialized` latched true and was
  never cleared when GRBL took UART1 (GP8/9), so a `TARGET SEND` / bootloader /
  ISP command after any GRBL command wrote to UART1 while it was still on GP8/9,
  bleeding bootloader traffic onto the GRBL controller. Target TX now
  auto-reclaims UART1 to GP4/5 (via `target_uart_ensure_active()`, applied to
  the send and STM32/LPC ISP-entry paths) and prints
  `OK: UART1 reclaimed from GRBL for Target (GP4/5)`. The manual `TARGET SYNC`
  after GRBL is no longer required. Verified electrically with dual FTDI probes.

## [0.7] — 2026-06-08 — ChipSHOUTER command fixes + hardening

### Fixed
- `CS FAULTS` now sends `get fault` (was `get faults_current`, which the
  ChipSHOUTER console rejected with "Command Not Found").
- `CS HVOUT` now sends `get voltage` (was `help`, which dumped the command list);
  reports the capacitor-bank charge voltage — set value plus the measured HV when armed.
- `CS TRIGGER HW` now also sends `set hwtrig_term 0` (high-impedance). The 50 Ω
  default (restored by every `CS RESET`) held the trigger input below its 2 V
  threshold for the Pico's 3.3 V GP2 GPIO, so the CS armed/charged but never fired.
- `CS TRIGGER SW` sends `set hwtrig_term 1` (was the invalid `True`) and no longer
  sends the invalid `set emode True`.
- `glitch_heatmap.py`: on `cs_error`, retry cheaply and only `CS RESET` + cool down
  once faults **persist** (was resetting on every transient, which stalled scans /
  thrashed; combined with a CS that trips without the literal "fault" string).

### Host tooling (`glitch_heatmap.py`)
- New `--drop N` (default 1): quickmap drops the voltage only after **N consecutive
  non-normals**, instead of on the first hit — so a single noise hit no longer moves
  the voltage. A normal breaks the hit streak (and vice-versa).
- New `--pause N` (default 0): glitch PAUSE in 150 MHz cycles is now a CLI arg
  (was hardcoded to 5000). Note: the LPC1114 success in `SUCCESS_FOUND.md` was at
  PAUSE 0, but this target's CRP-check window opens later — effects appear at
  PAUSE ≈ 3000–5000, nothing at 0/1000.
- `--shots` is now a **per-voltage** cap in quickmap, not a per-cell cap: the voltage
  descent always continues to the floor (`CS_VOLTAGE_MIN`); the cap only stops a
  non-converging single voltage from looping forever.

### Added
- `CS VOLTAGE` / `CS PULSE` argument guardrails: range-validate (150–500 V /
  80–1000 ns) and reject garbage/out-of-range **before** sending to the CS.
- CS replies are scanned for "Command Not Found" and surfaced as an `ERROR`, so a
  bad firmware command string can no longer masquerade as success.
- `config_none` regression tests for the CS guardrail error paths (`TestCsCommands`).

## [0.6] — 2026-06-07 — Crowbar EXTERNAL power mode + cleanup (PR #8)

### Added
- EXTERNAL crowbar power mode: `TARGET POWER EXTERNAL` re-tasks the GP10/11/12
  group — GP10 = supply enable, GP11 = PIO-driven crowbar gate (AHIGH/ALOW idle
  polarity), GP12 = spare. The gate emits the same waveform as GP2 via a 2nd
  pulse_generator SM on PIO0 SM3, with a soft-disarm so multi-pulse trains finish.
- Auto-power-on at the `TARGET SYNC` / `SWD CONNECT` choke points, with a
  cold-target settle delay before the first transaction.
- Power-test safety gating (`--config=power-int` / `--config=power-ext`) and a
  reboot-based boot-power-default regression test.

### Changed
- `TARGET POWER MODE <X>` → `TARGET POWER <X>` (the `MODE` keyword was dropped).
- Boot power default flipped to OFF (de-energized) in both modes.
- ADC channels renamed to the official 0-based numbering: `ADC 0` = GP26,
  `ADC 1` = GP27 (the old `ADC 2` now errors).

### Removed
- The never-implemented PLATFORM command and its abstraction
  (`platform.c`/`.pio`/`.h`, `PLATFORM_GUIDE.md`).

## [0.5] and earlier

Predates this changelog. Highlights from git history: the EXTERNAL power mode with
the PIO crowbar gate on GP11, the `pins` GPIO-assignment skill, shared host-script
tooling (CLI colors + Rigol scope helpers), the ADC-gated `VMIN` glitch primitive,
and the LPC/STM32 bootloader + bit-banged SWD/JTAG support. Initial public commit
was v0.3. See `git log` for detail.
