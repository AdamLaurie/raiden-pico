# VMIN — ADC-gated voltage glitching

`VMIN` adds a depth-based glitch primitive to Raiden Pico alongside the
existing time-based PIO pulse. Where the legacy path drops the glitch
output for a fixed number of cycles defined by `WIDTH`, the VMIN path
drops the target VDD rail and lets the CPU poll the ADC until the
probed voltage hits a configured threshold, then releases. `WIDTH` is
reinterpreted as a *minimum dwell time past the threshold*.

## Quick reference

| Command | Effect |
| --- | --- |
| `SET VMIN 0` | Disable VMIN; use legacy WIDTH-only PIO pulse |
| `SET VMIN 500` | Enable VMIN; release when ADC reads ≤ 500 mV (probe-space) |
| `SET WIDTH 0` | **Immediate restore** — release the rail the instant ADC hits VMIN, no dwell. Safest for the Pico GPIO pads and the most common starting point for new targets. |
| `SET WIDTH 15000` | 100 µs minimum dwell past threshold (15000 cycles ÷ 150) — adds a forced hold past threshold for chips whose core caps need extra discharge time after the I/O rail bottoms out. |
| `GET VMIN` | Show current threshold |
| `GET WIDTH` | Show current pulse / dwell time |
| `GET` (no args) | Show all glitch parameters |

VMIN is in millivolts so integer-only parsing keeps the CLI simple.
The probe pin is **GP26 (ADC channel 0)**, the same pin used by
`TARGET POWER SWEEP` and the `ADC` CLI command.

## Wiring (mandatory)

The target's reference rail must be physically connected to **GP26
(ADC1)** before any VMIN glitch will work. The CPU loop polls this
pin to decide when to release the rail — with nothing connected, it
reads noise / floor and the threshold never trips (or trips
immediately, depending on which side of `VMIN` the floor sits on).

Typical wiring choices:

- **JTAG `VTref`** — the JTAG header's target-reference-voltage pin
  is usually a direct tap on the chip's VDD (or VDD via a small
  series R for protection). Convenient if your target exposes a JTAG
  header. **Caveat:** some JTAG adapters bridge nRST or other signals
  onto the VTref net; characterise it with the four-state procedure
  below before trusting it. The Raiden firmware floats its reset pin
  when inactive specifically so the reset wire doesn't leak onto
  VTref.
- **Direct probe on a decoupling cap** at the chip's VDD pin — most
  accurate, but needs you to identify the rail you actually care
  about. Watch out for chips with separate I/O and core supplies;
  glitching the wrong rail will pass calibration but never affect
  the protection logic.
- **Divider** — if the rail you want to monitor exceeds GP26's ADC
  range (3.3 V max), put a divider between the rail and GP26.
  Everything VMIN reports will then be in probe-space; scale
  threshold values accordingly.

Sanity check after wiring: `ADC 1` should show the target's idle
voltage (or a divided version). If it reads 0 V the wire is loose;
if it reads close to 3.3 V you have a leak from another net (often
the reset line — see below).

## When does VMIN fire?

The CPU-side ADC primitive (`power_glitch_once`) is called by:

- `TARGET GLITCH LPCBYPASS [count]` — uses `VMIN` and `WIDTH` directly.
  Errors if `VMIN=0`.
- `TARGET GLITCH SWEEP` — already uses ADC gating with its own
  internal thresholds.
- `TARGET POWER GLITCH <voltage> <count>` — STM32 path, uses its own
  per-call voltage arg (will migrate to VMIN config in a future pass).

PIO-driven triggers (`TRIGGER UART`, `TRIGGER GPIO`) **do not** route
through the CPU-side ADC primitive yet. They still fire the WIDTH-only
PIO pulse on `PIN_GLITCH_OUT` (GP2). Wiring those into VMIN is a
separate piece of work tracked in the project memory as
`project_todo_glitch_vmin_set`.

## The pulse shape

```
                          ┌──────────────── idle VDD
   target VDD             │
   ───────────┐           │
              │           │
              │  ramp     │  restore
              │  down     │
              ▼           │
       ┌──────┴──── VMIN ─┴───────
       │  ADC-gated release
       │
       │  dwell (= WIDTH cycles ÷ 150 µs)
       │
       ▼
```

The CPU drives GP10/11/12 LOW and polls ADC0 in a tight loop. When the
ADC reading hits the threshold, it (optionally) holds for `WIDTH` µs
more, then restores the rail.

`dur_us` in the per-shot log is the **total** time the rail spent
LOW — from the moment GP10/11/12 went LOW until they went back HIGH.
That includes both the ramp-down and the dwell.

## RC time constant — why depth ≠ duration

The rail-fall time is dominated by RC, not by the configured dwell.
The Pico's GP10/11/12 sink together at roughly 36 mA peak. The target
board's bulk decoupling caps determine how long it takes to drop the
rail to the requested threshold:

```
dt ≈ C × ΔV / I_sink
```

| Total cap on rail | Time to drop 3 V at 36 mA |
| --- | --- |
| 10 µF | 830 µs |
| 1 µF | 83 µs |
| 100 nF | 8 µs *(fault-injection feasible)* |
| 10 nF | 0.8 µs *(real glitch territory)* |

So a stock target board with several µF of decoupling will give
millisecond-scale rail descents — the chip's POR fires somewhere
during the descent, the chip resets cleanly, and ISP recovers when
power restores. This is **soft power-cycling**, not fault injection.

To actually disturb chip logic mid-instruction you need the rail to
crash on a sub-µs timescale. Two ways to get there:

1. **Strip decoupling caps off the target board.** Identify the bulk
   (electrolytic) and ceramic 100 nF caps on the rail, pull them one
   at a time, watch `dur_us` shrink. Existing firmware works
   unchanged. Stop when ISP idle becomes unreliable — you've gone
   too far.
2. **External MOSFET crowbar** between target Vcore and ground,
   driven by `PIN_GLITCH_OUT` (GP2). The PIO pulse-gen already drives
   GP2 and the existing trigger machinery (manual, UART, GPIO)
   carries over directly. Costs hardware but doesn't modify the
   target.

## Probe placement

VMIN is only meaningful if GP26 is connected to the rail that actually
matters for the protection logic. The bench LPC2468 setup has taught
us a few things worth recording:

- **Different rails decouple in time and amplitude.** A board can have
  separate I/O VDD (3.3 V) and core VDD (1.8 V regulator output).
  Glitching the I/O rail to 0.05 V via GPIO sink while the core rail
  stays at 1.8 V via a local regulator means the protection logic
  never even notices.
- **JTAG TREF is often a clean tap** on the target VDD — but only if
  the JTAG adapter doesn't bridge nRST or other signals into it. The
  Raiden-Pico firmware floats the reset pin when inactive (GPIO_IN,
  no pulls) to avoid leaking 3.3 V from the Pico's drive onto TREF.
  See the four-state characterization procedure in
  `reference_jtag_tref_probe_check` memory before trusting a TREF
  probe.
- **Probe on the wrong rail will pass calibration but never glitch.**
  If the chip survives every voltage from 1.30 V down to 0.05 V with
  ms-scale `dur`, the probe is almost certainly on a different rail
  from the one being glitched.

## Per-shot power cycle

`TARGET GLITCH LPCBYPASS` performs a 100 ms power-OFF / 50 ms power-ON
cycle between shots so each attempt fires against a freshly-booted
chip. This eliminates state carryover between shots, which on a
slow-brownout setup can fabricate apparent "death floors" that
actually only reflect the cumulative damage from earlier shots.

If you want to test cumulative effects deliberately (e.g. for
race-conditions in early boot code), remove the inter-shot cycle
from `target_power_lpc_glitch` or expose it as a parameter — there's
no CLI knob for it yet.

## Calibration workflow

The bundled host script
`scripts/lpc_voltage_glitch_sweep.py` exercises the LPCBYPASS path
across a configurable voltage range and writes a CSV log + serves a
live web UI on port 8080. Two modes:

- **Calibrate** (`--calibrate`): 1 shot per voltage, descending from
  `--v-max` to `--v-min`. Stops on N consecutive sync_fails
  (`--calibrate-fails`, default 3). Output suggests a real-sweep
  range based on the highest survival voltage.
- **Real sweep** (no `--calibrate` flag): multiple shots per voltage
  (`--shots`), captures bypass / normal / effect / sync_fail
  classifications per shot.

Typical run:

```
python3 -u scripts/lpc_voltage_glitch_sweep.py --calibrate \
    --v-min 0.05 --v-max 1.30 --v-step 0.05
```

The script automatically `SET VMIN <mV>` before each voltage step,
so you only need to `SET WIDTH` once before running if you want a
non-zero dwell.
