# nRF52840 APPROTECT glitch — WIRING: **Reset (nRST) attack**

This is the wiring for the **warm-reset** attack (`nrf_reset_attack.py`): VDD stays **ON** and the
part is rebooted via **nRST** each attempt (no power-cycle). The idea was a PSU-free de-jitter — a
clean digital reset edge with DEC1 already up, so the APPROTECT read lands at a repeatable delay.

> ⚠️ **On this rig the reset attack does NOT work — it is coupling-limited, not a tuning problem.**
> With DEC1 already charged and the LDO in steady-state high-drive on a warm boot, the crowbar can't
> collapse it: ~2 M attempts across width 165–295 × delay 0–60 µs gave **0 unlocks / 0 disrupts**,
> while the **same widths crack the cold [power-cycle attack](NRF52840_WIRING_POWERCYCLE.md) 3–7×**.
> **Use the power-cycle attack.** This doc + script are kept for completeness and in case a hardware
> change (deeper coupling / lower Rsrc / stiffer drive) later makes the warm boot glitchable.

The only wiring difference from the power-cycle rig is: **VDD stays on** (not cycled) and you add the
**nRST wire (GP15 → nRF P0.18)**. Crowbar/MOSFET/resistors are identical — see
[`NRF52840_GLITCH_SETUP.md`](NRF52840_GLITCH_SETUP.md).

```
        Raiden Pico (RP2350)                         nRF52840 (PCA10059, rev 2)
        ┌────────────────┐                           ┌─────────────────────────┐
        │ GP17  SWDCLK ──┼──────────────────────────►│ SWDCLK                  │
        │ GP18  SWDIO ──┼◄─────────────────────────►│ SWDIO                   │
        │ GP15  nRST ───┼──────────────────────────►│ P0.18 / RESET (SW2 net) │ ◄ NEW: reboots via nRST
        │ GP10 ┐         │                           │                         │
        │ GP11 ├ VDD ────┼──────────────────────────►│ VDD  (stays ON)         │ ◄ NOT cycled
        │ GP12 ┘         │                           │ DEC1 (1.3 V core rail)  │ ◄ CROWBAR injection
        │ GP2  GLITCH ──┼──[Rg]──► MOSFET gate       │ P0.13 pad (BLINK marker)│ ◄ scope only (bench cal)
        │ GND ───────────┼──────────────────────────►│ GND                     │
        └────────────────┘   (MOSFET drain→DEC1,     └─────────────────────────┘
                              source→Rsrc 10Ω→GND, gate←Rg←GP2, gate→Rpd 10k→GND)
```

## Connections (delta vs the power-cycle rig)
| From (Pico) | To (nRF52840) | Purpose |
|---|---|---|
| **GP15** | **P0.18 / RESET (SW2 net)** | **NEW** — assert/release nRST to reboot each attempt |
| GP10 + GP11 + GP12 | VDD | power the target — **left ON the whole time** (not cycled) |
| GP2 → Rg → MOSFET gate; drain → DEC1; source → Rsrc 10 Ω → GND | — | crowbar (identical to power-cycle) |
| GP17 / GP18 | SWDCLK / SWDIO | SWD |
| GND | GND | one common ground |

Board prep identical (DEC1/VDD caps removed; tap DEC1 on the live die pin).

## Operating point (intended, not validated — see warning above)
- **DELAY ≈ 0–8 µs after the nRST RELEASE edge** (warm boot is fast: DEC1 up, no rail ramp;
  app-start ≈ 7–8 µs after release, the APPROTECT read precedes it — LayerOne ~50–150 cyc).
- Width swept 165–295 cyc; reset_hold ≈ 200 µs. **No unlock at any of these on this rig** (coupling).

## Run it (will report no-unlock on this rig)
```
./scripts/nrf_reset_attack.py --d0 0 --d1 8 --dstep 1 --w0 225 --w1 265 --wstep 2 --reset-hold 200
```
Firmware pre-flight verifies the part is genuinely LOCKED first (honest AHB read) and aborts if it's
already open. On a (hypothetical) unlock it dumps the full flash + RAM, same as the power-cycle attack.
