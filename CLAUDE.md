# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Operating constraints (read first)

- **Target hardware is a Pico 2 (RP2350), NOT a Pico (RP2040).** All firmware must target the RP2350.
- **Always flash with `make flash`** (from `build/`). Never copy UF2s by hand.
- **Use `/dev/ttyACM0`** for your own debugging/testing. **Close the tty when finished**, and never open it except for flashing or testing.
- `PICO_SDK_PATH=/home/software/unpacked/pico-sdk`
- You have full control of the Pico 2 and can flash + test it yourself. Verify everything works before notifying the user. Only ask for confirmation when external instruments (e.g. oscilloscope) are needed to validate something.

## Build, flash, test

```bash
# Configure + build (default board = pico2)
./build.sh                         # wraps: mkdir build; cd build; cmake ..; make -j
cmake -S . -B build -DBOARD=xxl    # alternate board: Olimex RP2350-XXL

# Flash (reboots target via 'REBOOT BL' over ttyACM0, then copies UF2 to /media/$USER/RP2350)
cd build && make flash

# If the firmware hangs and ignores 'REBOOT BL', reset via FTDI DTR→EN:
python3 scripts/reset_pico.py --wait

# Host-side integration tests (pytest drives the CLI over /dev/ttyACM0)
pip install -r requirements-dev.txt
pytest tests/ -v                   # config_none tests only (USB, no extra wiring)
pytest tests/ -v --config=swd      # requires SWD target wired
pytest tests/ -v --config=sweep    # requires target power + ADC26 wiring
pytest tests/ -v --config=trace    # requires shunt resistor + ADC1 wiring
pytest tests/ -v --config=swd-rdp --destructive   # DESTRUCTIVE: mass-erases target flash
pytest tests/ -v --config=power-int   # drives GP10/11/12 in INTERNAL mode — confirm ganged wiring / no target
pytest tests/ -v --config=power-ext   # EXTERNAL mode + FIRES the crowbar gate — confirm crowbar wiring / no target
pytest tests/test_config_swd.py::test_name -v      # single test
```

Tests are gated by `--config` wiring markers (see `tests/conftest.py` and `pyproject.toml`); without flags only hardware-free tests run. **The power-group tests (`TARGET POWER` modes + crowbar fire) are gated behind `power-int`/`power-ext` and never run by default** — on the wrong wiring they can damage a connected target or short GP10 against GP11/12. `RAIDEN_PORT` env var overrides the serial port.

## Architecture

Raiden Pico is RP2350 firmware that turns a Pico 2 into a fault-injection / glitching controller. It is driven entirely through a **USB CDC serial CLI** (stdio is USB-only; `pico_enable_stdio_uart` is off). There is no host GUI in firmware — host-side Python scripts (root + `scripts/`) talk to the CLI over the serial port.

**Main loop** (`src/main.c`) is a single cooperative poll: read CLI line → `command_parser_parse` → `command_parser_execute`, then service ChipShouter UART, target UART, and glitch flags. All subsystems are non-blocking and cooperate from this loop.

### Subsystem map (`src/` + matching `include/` headers)

- **`command_parser.c`** — the hub (~3500 lines). A flat `if/strcmp` dispatch over the first token (`SET`, `GET`, `TRIGGER`, `ARM`, `GLITCH`, `TARGET`, `SWD`, `JTAG`, `GRBL`, `CS`, `TRACE`, `ADC`, …). Supports non-ambiguous command abbreviation via `command_parser_match`. **New CLI commands go here**, dispatched out to the relevant subsystem.
- **`glitch.c` + `glitch.pio`** — core glitch engine. Uses PIO state machines for trigger detection and precise pulse generation. Config lives in `glitch_config_t` (`config.h`): `PAUSE/WIDTH/GAP/COUNT` in 150 MHz cycles (6.67 ns each), plus `VMIN`.
- **`target_uart.c`** — large subsystem (~4900 lines) for talking to targets over UART1: bootloader entry, LPC ISP + STM32 USART bootloader (AN3155) commands, power control, and the power-glitch / ADC-gated bypass routines.
- **`chipshot_uart.c`** — ChipSHOUTER control over UART0 (GP0/GP1).
- **`swd.c`, `jtag.c`** — bit-banged SWD (GP17/GP18, nRST GP15) and JTAG for ARM debug, flash dumping, RDP/CRP inspection.
- **`stm32_target.c`, `stm32_breakpoints.c`, `lpc_target.c`** — per-family target logic (STM32F1/F3/F4/L4, LPC ARM7 + LPC Cortex-M). STM32 flash register maps are table-driven via `stm32_target_info_t` / `stm32_get_target_info`.
- **`grbl.c`** — drives a GRBL CNC XY(Z) platform over UART1 (for EMFI probe positioning / heatmapping).

### Two cross-cutting hardware facts

1. **UART1 is shared between Target (GP4/GP5) and GRBL (GP8/GP9)** via pin alternate-functions — only one can be live at a time. Commands auto-switch, but switching breaks the other connection. **After any GRBL command you must re-run `TARGET SYNC` before `TARGET SEND` works again.** (Full workflow in the section below.)

2. **VMIN = ADC-gated glitching.** `SET VMIN <mV>` (0 = disabled) switches the glitch from a fixed-time PIO pulse to a CPU-side primitive that drops the rail and polls **ADC0 on GP26** until the probed voltage reaches the threshold, then holds for `WIDTH` cycles of minimum dwell. **Requires the target rail wired to GP26.** Currently routes through `TARGET GLITCH LPCBYPASS` and the STM32 sweep/test paths; PIO triggers (UART/GPIO) still use WIDTH-only timed pulses. See `VMIN.md`.

### Pin assignments

Authoritative source is `include/config.h` (and `swd.h` for debug pins); the live `PINS` CLI command prints current wiring. Key fixed pins: glitch out GP2 (inverted GP7), clock GP6, ChipShouter UART0 GP0/GP1, ARMED GP16, GLITCH_FIRED GP22, SWD SWCLK/SWDIO GP17/GP18, target nRST GP15. ADC trace uses a shunt on GP27/ADC1.

> Note: `PIO_ARCHITECTURE.md` describes the intended SM allocation but predates some pin moves — trust `config.h` and `PINS` for actual pins.

## Reference docs

Extensive Markdown docs at the repo root document specific workflows and findings: `README.md` (full CLI command reference), `VMIN.md`, `TARGET_UART.md`, `CHIPSHOT_UART.md`, `GLITCHING_GUIDE.md`, `PIO_ARCHITECTURE.md`, `SHORTCUTS.md`, and the various `*_RESULTS.md` / `*_FINDINGS.md` campaign notes.

## UART Switching (Target vs Grbl)

The system uses UART1 for both Target (GP4/GP5) and Grbl XY platform (GP8/GP9 via alternate function).
Only one can be active at a time - switching between them requires reconfiguration.

**Important Limitation:**
- Switching from Target to Grbl (or vice versa) breaks the active UART connection
- After using GRBL commands, TARGET SYNC must be run again before TARGET SEND commands will work
- This is a hardware limitation - UART1 can only be on one set of pins at a time

**Workflow:**
1. TARGET SYNC → TARGET SEND commands (works)
2. GRBL commands (auto-switches, breaks target connection)
3. TARGET SYNC → TARGET SEND commands (re-establishes connection, works)

**Auto-switching behavior:**
- GRBL commands automatically initialize Grbl UART (GP8/GP9) if not active
- TARGET commands automatically initialize Target UART (GP4/GP5) if not active
- Switching happens transparently but breaks the other connection
- TRIGGER UART functionality continues to work correctly after auto-switching
