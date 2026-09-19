# Changelog

All notable changes to the Raiden Pico firmware. The version is the string the
`VERSION` CLI command prints (defined in `src/command_parser.c`); bump it in the
same change (see the `version-bump` skill) and add an entry here.

Format loosely follows [Keep a Changelog](https://keepachangelog.com/). This file
was started at v0.7, so pre-0.6 entries are summarized from git history.

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
