# nRF52840 APPROTECT glitch — tooling

**Ultimate goal: two BLIND glitch attacks** — `nrf_attack.py`/`nrf_autopwn.py` (power-cycle) and
`nrf_reset_attack.py` (nRST reset) — that need only power/reset + the crowbar + an SWD probe, run
the calibrated width/delay parameters, and detect success by a real AHB read. Everything else here
is **bench calibration on a sacrificial part to find those parameters** (a real target can't be
erased or flashed). See "Practice-bench vs real target" below.

Host-side tools for the nRF52840 APPROTECT voltage-glitch bypass (crowbar on DEC1).
Hardware wiring, probe placement, and resistor values: see [`../NRF52840_GLITCH_SETUP.md`](../NRF52840_GLITCH_SETUP.md).
Firmware commands and attack model: see [`../NRF52840_APPROTECT.md`](../NRF52840_APPROTECT.md).

All tools talk to the Raiden Pico over USB-CDC (`/dev/ttyACM*`, auto-detected) and, where
noted, the Rigol scope over LAN (`lxi`). Deps: `pyserial`, `lxi-tools`, `Pillow` (for PNGs).

**Outputs go to a per-target folder** (keeps `scripts/` clean): nRF logs/dumps/screenshots →
`scripts/nrf52840/`, PIC dumps → `scripts/pic18/` (created on demand). Override with `--out`/`--log`/
`--csv`. Helper: `project_dir(name)` / `project_path(file, name)` in `nrf_recovery.py` (nRF) and
`pic_icsp.py` (PIC).

## The 3-step pipeline

1. **Tune the glitch WIDTH** — `nrf_feedback_harness.py`
   Loads a Thumb mock of the APPROTECT check into RAM on the (ERASEALL-transiently-unlocked)
   chip, glitches it, and reads ground-truth result counters over SWD → per-shot
   **SKIP / CRASH / NOEFFECT**. `sweep` finds the clean-skip width band (no scope needed).
   ```
   ./nrf_feedback_harness.py sweep --w-start 150 --w-end 185 --w-step 5 --reps 30
   ```

2. **Measure the boot DELAY** — `nrf_timing_marker.py`
   Flashes a tiny app whose first instruction drives a marker GPIO; scopes that edge vs a
   reference to get the boot ROM → app-start delay (≈ just after the APPROTECT read).
   - **Power-cycle boot** (CH1=VDD or GP2 fiducial):
     ```
     ./nrf_timing_marker.py --pin 13 flash      # program the marker app (re-locks after)
     ./nrf_timing_marker.py --pin 13 --tb 0.0004 --shot-delay 3000 marktime   # CH1=GP2, CH2=marker
     ```
   - **Reset (nRST) boot** — for the reset-glitch. A static marker is invisible on a warm
     reset (a floating GPIO retains its level), so use a **BLINK** marker and trigger on the
     nRST-release edge (CH1=GP15, CH2=marker):
     ```
     ./nrf_timing_marker.py --blink --pin 13 flash
     ./nrf_timing_marker.py --blink --pin 13 --tb 0.000005 resetmeasure   # nRST-release -> app-start
     ```
   `relock` re-protects the part after any flash (write UICR.APPROTECT=0xFFFFFF00 + power-cycle).

3. **Run the attack** — pick the boot mode:
   - **Power-cycle**: `nrf_autopwn.py` (loops/rotates windows forever) or `nrf_attack.py` (single window).
   - **Reset (nRST, PSU-free de-jitter)**: `nrf_reset_attack.py` — VDD stays on, reboots via nRST;
     the read window is EARLY (~0–8 µs after release), so sweep small delays.

   All verify the part is genuinely locked first (firmware pre-flight aborts if it's already open),
   detect unlock by an **actual AHB read** (not the unreliable APPROTECTSTATUS bit), and on a real
   unlock INFO + auto-dump the flash.
   ```
   ./nrf_attack.py --d0 1100 --d1 1320 --dstep 2 --w0 170 --w1 174 --tries 40000   # power-cycle
   ./nrf_autopwn.py --pico-port /dev/ttyACM0                                        # continuous
   ./nrf_reset_attack.py --d0 0 --d1 8 --dstep 1 --w0 165 --w1 175 --tries 200000   # reset
   ```

## Helpers
- `rigol_scope_live.py` — live scope view during the attack (LAN-only; runs in parallel). Writes
  `nrf52840/scope_live.png` every few seconds (CH1=GP2 glitch, CH2=marker).
- `rigol_screenshot.py` — grab a Rigol display hardcopy and save a true PNG in one flow (the
  scope's hardcopy is BMP regardless of extension). `--dir` picks the destination folder
  (default `scripts/nrf52840/`). Importable `grab_png(ip, path)`; used by the capture scripts.
- `rigol_view.py` — auto-refreshing window to WATCH the latest capture PNG (default
  `nrf52840/scope_live.png`), or `--poll` to mirror the scope live. `--dir`/`--file` to point elsewhere.
- `nrf_recovery.py` — post-unlock APPROTECT-persistence helper, branched on build code (legacy
  ERASEUICR vs hardened print-only). Default dry-run.

## ⚠️ Practice-bench vs real target — what actually transfers
The real target (e.g. a locked product PCB) gives us only: power/reset control, the crowbar
glitch, and an SWD probe. We **cannot** erase it or flash anything to it — doing so would destroy
the very firmware we want to dump. So several tools here are **bench-calibration only**, used on a
sacrificial practice part to *learn the glitch strategy*; on the real target we apply the learned
numbers blind.

| Step / tool | Practice bench | Real target |
|---|---|---|
| `nrf_feedback_harness.py` (width) | ✅ needs ERASEALL transient-unlock + RAM mock | ❌ destructive (ERASEALL) — **bench only** |
| `nrf_timing_marker.py` (delay) / `relock` | ✅ flashes a marker app, re-locks | ❌ would erase the target — **bench only** |
| Learned **WIDTH band** + **DELAY window** | calibrated here | ✅ **applied blind** (same chip class) |
| **Cold power-cycle** attack (`nrf_attack.py`/`nrf_autopwn.py`) — THE working one | ✅ | ✅ same glitch + probe |
| Warm `nrf_reset_attack.py` (nRST) — coupling-limited, dead end here | ✅ (no unlock) | ❌ don't bother |
| **Unlock detection = real AHB read** | ✅ | ✅ the *only* honest signal on a target (no marker) |
| On unlock: dump full flash + RAM | ✅ | ✅ (RAM = runtime secrets) |

Takeaway: the marker/harness/relock steps exist to *develop and validate the strategy*; the
deliverable for the real target is the **tuned width + delay + the glitch/probe loop with honest
AHB-read detection**, ported as-is.

## ✅ SOLVED operating point (PCA10059 rev 2, Rsrc = 10 Ω, caps removed) — 2026-06-02
The **cold power-cycle attack cracks the part reproducibly** (3–7× in this session). Wiring per attack:
[`../NRF52840_WIRING_POWERCYCLE.md`](../NRF52840_WIRING_POWERCYCLE.md) (works) and
[`../NRF52840_WIRING_RESET.md`](../NRF52840_WIRING_RESET.md) (warm reset — coupling-limited, does not work).
- **Width**: **225–265 cyc** (~1.5–1.77 µs), hits cluster 255–265. The one-shot LOCKED boot needs a
  STRONGER pulse than the RAM-harness clean-skip band (165–180 was a red herring for the boot).
- **Delay (power-on boot)**: **1065–1170 µs** after power-on (broad ~110 µs window); app-start ≈ 1271 µs
  (slow rail ramp), APPROTECT read precedes it. Blind sweep 1050–1350 µs lands it.
- **Off-time**: **18 ms** (rail fully discharges → clean cold boot each attempt).
- **Detection**: unlock = a successful **AHB read** (`FICR.INFO.PART == 0x52840`). The CTRL-AP
  `APPROTECTSTATUS` bit is unreliable (can read PROTECTED while the AHB-AP is open) — never gate on it
  alone. **Genuine = UICR.APPROTECT stays 0xFFFFFF00 (locked) while debug is open** = clean skip.
- **On unlock**: dumps full **flash (1 MiB) + RAM (256 KiB)**; bounded APB/AHB via `nrf_dump_test.py`.
- **Warm nRST reset**: app-start ≈ 8 µs after release, read ~1–2 µs — but the crowbar can't fault the
  warm boot (DEC1 held by the LDO); ~2 M attempts, 0 unlocks. Cold power-cycle is THE attack.

## What memory we dump — full nRF52840 memory map
Per the official nRF52840 memory map (see the Nordic product specification).
Once the glitch opens the AHB-AP, any AHB-readable region is dumpable over `TARGET NRF DUMP`. On unlock
the attacks pull the two bulk regions; `nrf_dump_test.py` adds bounded peripheral samples.

| Region | Base | Size | Output file | Dumped by | Notes |
|---|---|---|---|---|---|
| **Code flash** | `0x00000000` | **1 MiB** (`0x100000`) | `<out>.bin` | every attack, on unlock | the internal firmware image |
| Code-RAM (mirror) | `0x00800000` | 256 KiB | — | (same as 0x20000000) | SRAM via the code bus — same content as Data RAM |
| FICR (factory info) | `0x10000000` | — | (not bulk-dumped) | `TARGET NRF INFO` | PART/VARIANT/PACKAGE/DEVICEID |
| UICR (user config) | `0x10001000` | — | (not bulk-dumped) | INFO / recovery | holds `UICR.APPROTECT` @0x10001208 |
| **XIP (ext QSPI flash)** | `0x12000000` | up to `0x08000000` (to 0x19FFFFFF) | — | **add on a real target!** | external flash if fitted — firmware/data may live here, NOT in internal flash |
| **Data RAM (SRAM)** | `0x20000000` | **256 KiB** (`0x40000`) | `<out>.ram.bin` | every attack, on unlock | runtime data / secrets on a live target |
| **APB peripherals** | `0x40000000` | **192 KiB** (all 48 instances) | `nrf_apb.bin` | `nrf_dump_test.py` | incl. ECB/CCM/AAR (AES), NVMC, **APPROTECT**, QSPI |
| **AHB GPIO** | `0x50000000` | 4 KiB | `nrf_ahb_gpio.bin` | `nrf_dump_test.py` | GPIO P0/P1 config |
| ★ **CRYPTOCELL 310** | `0x5002A000` | 8 KiB | `nrf_cryptocell.bin` | `nrf_dump_test.py` | crypto/key subsystem (AES/ChaCha/HASH/PKA/RNG) |
| PPB (Cortex-M sys) | `0xE0000000` | — | (not dumped) | — | NVIC/SCB/debug regs |

**Peripheral base addresses:** see [`../../WORK/nRF52840_instantiation_table.md`](../../WORK/nRF52840_instantiation_table.md)
(full instance/base/description list). Security-relevant on a target: **CRYPTOCELL 310** @0x5002A000 +
CC_* engines @0x5002B000, **AES** ECB @0x4000E000 / CCM+AAR @0x4000F000, **NVMC/ACL** @0x4001E000,
**APPROTECT** @0x40000000, **QSPI** (ext-flash iface) @0x40029000.

Constants in `nrf_recovery.py`: `NRF52840_FLASH_SIZE`, `NRF52840_RAM_BASE/SIZE`, `NRF52840_APB_BASE`,
`NRF52840_AHB_BASE`. ⚠️ **Peripheral reads can have side effects** (read-to-clear) and unmapped
addresses bus-fault, so APB/AHB are **bounded samples**, not a blind dump. `dump_region(p, base, size,
out)` dumps any region; `dump_flash_and_ram(p, out)` does the two bulk regions.
- ★ **REAL-TARGET NOTE:** on any nRF52840 with external **QSPI flash** also dump
  the **XIP region @0x12000000** — the real firmware/keys may be in external flash, not the internal
  1 MiB. (The practice dongle has no QSPI, so XIP reads empty there.)
- **Fidelity verified**: wrote `1C E1 CE BA B4` to RAM @0x20002000 → found at the exact offset in the dump.
