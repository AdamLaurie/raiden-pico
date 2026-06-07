---
name: pins
description: GPIO pin-assignment rules for Raiden Pico (RP2350). Invoke whenever you add, change, move, or claim a GPIO pin — e.g. editing pin #defines in include/config.h or other headers, wiring a new target/feature to a GPIO, or reviewing/creating a PR that touches pin assignments. Enforces (1) never repurpose the ADC-rail pins GP26–GP29, and (2) keep the PINS CLI output and README/wiring docs in sync with any pin change.
allowed-tools: Bash, Read, Edit
---

Two hard rules govern GPIO assignment in this project. Apply them whenever a change adds, moves, or claims a pin — and check them when reviewing any PR that touches pin `#define`s, `gpio_*` calls, or wiring docs.

## Rule 1 — never repurpose the ADC-rail pins (GP26–GP29)

GP26–GP29 are the RP2350's analog inputs and are reserved for ADC use only. **Do not assign them to digital I/O, glitch outputs, crowbar gates, triggers, status lines, or any switching signal.** A fast digital edge on an ADC-capable pad couples noise into AVDD / the ADC reference and corrupts exactly the measurements the glitch loop depends on.

| Pin | ADC | Role — keep clear |
|-----|-----|-------------------|
| GP26 | ADC0 | target VDD monitor / `VMIN` gating — **in active use** |
| GP27 | ADC1 | shunt-current capture / `TRACE` — **in active use** |
| GP28 | ADC2 | reserved for analog; do not switch digital signals here |
| GP29 | ADC3 | VSYS sense (internal) — do not use |

Also avoid the board-internal pins **GP23** (SMPS power-save), **GP24** (VBUS sense), **GP25** (onboard LED) for new functions on the base Pico 2.

When you need a free GPIO, take it from the digital range and confirm it isn't already claimed (see the authoritative sources below). On the **Olimex RP2350-XXL** (`BOARD=xxl`) there are many more GPIOs (GP30+) — prefer a board-conditional assignment so the base Pico 2 isn't forced to double-book a pin.

## Rule 2 — pin changes must update CLI + docs in the same change

A GPIO assignment lives in **four** places that must stay consistent. When you add or change a pin, update all that apply in the *same* PR:

1. **The definition** — `include/config.h` (or the relevant header, e.g. `swd.h`, `jtag.h`, target headers). This is the source of truth.
2. **The `PINS` CLI command** — its output in `src/command_parser.c` must list the new/changed pin. (Also update any `HELP`/usage strings that name the pin.)
3. **`README.md`** — the pin/wiring reference section.
4. **Per-target wiring docs** — e.g. `NRF52840_WIRING_*.md`, `PIC18_ICSP.md`, `EFM32_GLITCH.md` — whichever describe the affected wiring.

A pin change that updates the `#define` but not the `PINS` output or the docs is incomplete — reviewers should request the doc/CLI updates before merge.

## Authoritative sources (read these before claiming a pin)

```bash
# Current pin #defines across headers
grep -rnE '#define[[:space:]]+[A-Z0-9_]*(PIN|GP)[A-Z0-9_]*[[:space:]]+[0-9]' include/
# Hardcoded GPIO usage in firmware (power group, GRBL alt-func, trigger pin, ADC)
grep -rnE 'POWER_PIN|POWER_MASK|GRBL_(TX|RX)_PIN|adc_gpio_init|gpio_(init|set_dir|set_function)' src/
```

The live `PINS` command on the device prints the current assignment — treat `include/config.h` + `PINS` as authoritative (`PIO_ARCHITECTURE.md` is partly stale).

## Checklist when adding/changing a pin
- [ ] Target pin is **not** GP26–GP29 (ADC) and not GP23–GP25 (board-internal).
- [ ] Chosen pin isn't already claimed by another subsystem (grep the sources above).
- [ ] If it could collide on base Pico 2, made it board-conditional (`pico2` vs `xxl`).
- [ ] Updated the `#define` in the correct header.
- [ ] Updated the `PINS` output (and any HELP/usage text) in `src/command_parser.c`.
- [ ] Updated `README.md` and the relevant wiring doc(s).

## When NOT to invoke
- Changes that don't touch any GPIO assignment (parsing tweaks, host scripts, pure docs unrelated to wiring).
