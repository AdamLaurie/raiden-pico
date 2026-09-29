# Changelog

All notable changes to the Raiden Pico firmware. The version is the string the
`VERSION` CLI command prints (defined in `src/command_parser.c`); bump it in the
same change (see the `version-bump` skill) and add an entry here.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/). This file
was started at v0.7, so pre-0.6 entries are summarized from git history.

## [0.12-swdscan] — 2026-09-29 — SWD SCAN (DAP / CoreSight enumeration)

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
