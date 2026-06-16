# PIC18 ICSP + Code-Protect Glitch — Wiring & Operation

How the Raiden-Pico (Pico2 / RP2350) talks to a classic PIC18 over ICSP and
faults its code-protection (CP) to read protected program memory. Pairs with the
host scripts `scripts/pic_info.py`, `scripts/pic_dump.py`, `scripts/pic_glitch.py`
(shared lib `scripts/pic_icsp.py`) and the firmware in `src/pic18_target.c`.

Background analysis and findings: `../WORK/pic18_report.md`,
`../WORK/pic18_findings.md` (referenced as FIND-NNN below).

---

## 1. Why ICSP (not SWD/UART)

A PIC18 has **no SWD and no UART ROM bootloader**. Program memory is accessed
over **ICSP**: a 2-wire synchronous serial link (PGC clock, PGD data) plus the
MCLR/VPP entry pin. When CONFIG5L code-protect bits are set, an ICSP Table Read
of a protected block returns `0x00` — the data is still fetched into the table
latch, CP only gates it onto PGD (FIND-102). The attack faults that gate during
the read clock-out so the real byte appears instead of `0x00` (FIND-105/300).

Entry is **LVP (Low-Voltage Programming)**, and there are **two flavours** — pick
the one that matches the silicon (wrong flavour ⇒ the part reads `NOT CONNECTED`):

- **KEY** (FX220/X320, DS39592 — firmware default): clock the 32-bit key `"MCHP"`
  on PGD while raising MCLR to VDD. The PGM pin is unused.
- **PGM** (**PIC18F4321 family**, DS39687): hold **RB5/PGM high** (GP19) while
  raising MCLR to VDD — **no key is clocked**, and PGM stays high for the whole
  session. Select it with `TARGET PIC LVP PGM`, or `--lvp pgm` on the host scripts.

Both need only GPIO — **no 9–13 V VPP driver** (FIND-207) — and both require the
part's LVP config bit = 1 (the default after erase, FIND-406). If LVP is disabled
you need an HV VPP source on MCLR (out of scope of this rig).

---

## 2. Pin map (firmware-fixed)

Defined in `include/pic18_target.h` / `include/config.h`. The ICSP pins reuse the
SWD header.

| Pico2 GPIO | Signal            | PIC18 pin            | Direction        |
|-----------:|-------------------|----------------------|------------------|
| **GP17**   | ICSP **PGC** clock| PGC / RB6            | Pico → target    |
| **GP18**   | ICSP **PGD** data | PGD / RB7            | bidirectional    |
| **GP15**   | **MCLR/VPP**      | MCLR/VPP             | Pico → target    |
| **GP19**   | **PGM** (LVP en)  | RB5 / PGM            | Pico → target    |
| **GP2**    | **crowbar** glitch| target **VDD** rail  | Pico → target    |
| **GP22**   | GLITCH_FIRED      | (scope trigger only) | Pico → scope     |
| GP10/11/12 | target **power**  | VDD switch (`TARGET POWER`) | Pico → target |
| GND        | common ground     | VSS                  | —                |

All signals are 3.3 V logic. **Common ground is mandatory** — the Pico GND, the
target VSS, the crowbar return, and the scope ground must be the same node.

---

## 3. Wiring schematic

```
        Raiden-Pico (Pico2 / RP2350)                     PIC18 (e.g. F2320/F4320)
       ┌───────────────────────────┐                   ┌────────────────────────┐
       │                           │                   │                        │
       │  GP17 (PGC) ──────────────┼───────────────────┤ PGC / RB6              │
       │  GP18 (PGD) ──────────────┼───────────────────┤ PGD / RB7              │
       │  GP15 (MCLR)──────────────┼───────[ 1k ]──────┤ MCLR/VPP   (+10k p" up │
       │                           │                   │             to VDD)    │
       │  GP19 (PGM) ──────────────┼───────────────────┤ RB5/PGM  (F4321 LVP;   │
       │                           │                   │          unused on FX320)│
       │                           │                   │                        │
       │  GP10/11/12 ─[ load sw ]──┼─────┬─────────────┤ VDD  (2.0–5.5 V)       │
       │  (TARGET POWER)           │     │             │                        │
       │                           │     │             │                        │
       │  GP2 (crowbar) ──[ Q1 ]───┼─────┘  ◄ glitch   │                        │
       │                  gate     │   on VDD rail      │                        │
       │                           │                   │                        │
       │  GND ─────────────────────┼─────────┬─────────┤ VSS                    │
       │  GP22 (scope trig) ──┐    │         │         │                        │
       └──────────────────────┼────┘         │         └────────────────────────┘
                              │              │
                         ┌────▼────┐    common ground
                         │  SCOPE  │◄────────┘   CH1: target VDD   EXT-TRIG: GP22
                         └─────────┘
```

### Crowbar detail (GP2)

The crowbar is an N-channel MOSFET (Q1) that briefly shorts the target VDD rail
toward GND when GP2 pulses high — the same crowbar platform the nRF/STM32 work
uses (`platform_set_type(PLATFORM_CROWBAR)` in firmware). Keep it close to the
target VDD pin and minimise decoupling on the target so the droop is sharp
(FIND-106 notes the spec's 0.1–10 µF decoupling fights the glitch — reduce it).

```
   target VDD ──┬──────────────► PIC18 VDD pin
                │
              drain
   GP2 ─[ Rg ]─gate   Q1  (logic-level N-FET, e.g. IRLML2502 / AO3400)
              source
                │
               GND (common)
```

`Rg` ~33–100 Ω gate series; optional ~10 k gate pulldown. Width is set in
firmware cycles (6.67 ns @150 MHz), clamped to the safe band **15–450 cyc
(~0.1–3.0 µs)** so an over-wide pulse cannot tip the NVM controller into an erase.

### MCLR series resistor

A ~1 kΩ series resistor on GP15→MCLR protects the GPIO if the board also has a
hardware MCLR pull-up to VDD (typical 10 kΩ). LVP drives MCLR to VDD logic level,
not high voltage, so no level shifting is needed.

---

## 4. Bring-up order (do this once, on the bench)

1. **Wire** per the table; verify common ground with a meter.
2. **Power the target from the Pico** so the rig can power-cycle it for each
   glitch attempt: `TARGET POWER ON` (GP10/11/12). Confirm VDD on the scope.
3. **Identify the part — non-destructive, works even when protected:**
   `./pic_info.py` (FX320), or `./pic_info.py --lvp pgm` for a **PIC18F4321
   family** part (RB5/PGM on GP19). You should see a real `DEVID` (not
   `0x0000`/`0xFFFF`) and the decoded CP state. If `NOT CONNECTED`: re-check
   PGC/PGD/MCLR, GND, power, that the part's LVP bit is 1, and — most commonly on
   an F4321 — that you picked the right `--lvp` flavour and wired GP19→PGM.
4. **Validate the read path on a known-OPEN part first** (or the always-readable
   CONFIG/ID space): `./pic_dump.py --addr 0x300000 --size 14 -o config.bin`.
   Confirm you get sensible, non-zero config bytes. Only trust a *glitched* dump
   after the plain read path is proven (per the header warning in
   `pic18_target.h`).
5. **Park the scope** on VDD with EXT-TRIG = GP22 and fire single shots:
   `./pic_glitch.py shot --delay 1000 --width 150`. Tune the crowbar until you
   see a clean, repeatable droop at the marker.

---

## 5. Attack workflow (Chain 1 — CP read glitch)

```
   pic_info.py            -> DEVID, revision, which CPn blocks are protected
        │
   pic_dump.py (probe)    -> confirm the target block currently reads 0x00
        │
   pic_glitch.py sweep    -> delay x width search; firmware fires the crowbar
        │                    into the Table-Read clock-out, looking for a
        │                    non-0x00 read at the probe address
        │   (hit: "*** CP BYPASS on attempt N (delay=.. width=..): read 0xXX")
        ▼
   pic_dump.py / --dump-on-hit -> read out the now-leaking memory
```

Window-finding tactic (FIND-105): start **wide and coarse**, and if hits are
elusic, run ICSP slower / VDD lower to widen every read window ~10×, then tighten
around the delay/width that first leaks a byte. Public single-shot yield is low
(~0.24 %, FIND-306) but the gate is re-evaluated **per byte**, so one good
(delay,width) cell can be replayed address-by-address for a full dump.

Example first pass:

```
./pic_glitch.py sweep --probe 0x001800 \
    --d-start 0 --d-end 200 --d-step 5 \
    --w-start 30 --w-end 200 --w-step 10 --max 20000
```

`--probe` must be a byte address inside a block whose CPn = protected (read it
off `pic_info.py`). Default `0x001800` is the CP3 block on the FX320 map.

> **Part-map caveat (FIND-108):** the `PIC18F4321` family is *different silicon*
> from the `FX320` family — variable boot block (BBSIZ), only CP0/CP1. Read DEVID
> first and use that part's own block boundaries, or the probe address mis-targets.

---

## 6. Host command surface (what the scripts send)

| Script call             | Pico CLI command                                   |
|-------------------------|----------------------------------------------------|
| `PicLink(lvp=...)` ctor | `TARGET PIC18` then `TARGET PIC LVP KEY|PGM`         |
| `link.set_lvp("pgm")`   | `TARGET PIC LVP PGM`  (F4321 family, GP19/PGM)      |
| `link.info()`           | `TARGET PIC INFO`                                   |
| `link.status()`         | `TARGET PIC STATUS`                                 |
| `link.dump(a, n)`       | `TARGET PIC DUMP 0xADDR n` (chunked)               |
| `link.shot(d, w)`       | `TARGET PIC SHOT d w`                               |
| `link.glitch_cp(...)`   | `TARGET PIC GLITCH 0xPROBE d0 d1 ds w0 w1 ws max`   |

Default port `/dev/ttyACM0` (override `--port` or `$PIC_PORT`), 115200 baud.

---

## 7. Safety / gotchas

- **Code-protect glitching is the goal, NOT erase.** Width is clamped (15–450 cyc)
  precisely so a stray wide pulse cannot push the NVM controller into a bulk
  erase that would destroy the firmware you want (FIND-200/212). Do **not** raise
  the firmware clamp without a scope and a sacrificial part.
- **Never issue a bulk/block erase against the only target.** The CP-clear path
  also erases the array; the erase-asymmetry idea (Chain 2, FIND-201/405) is
  destructive-on-failure and must be proven on sacrificial samples first.
- A glitched read is only trustworthy once the **plain** read path is validated
  against known data (open part / CONFIG space).
- Keep target decoupling minimal and the crowbar close to the VDD pin.
- These scripts only act on explicit calls; importing the library powers nothing.

---

## 8. References

- `../WORK/pic18_report.md` — engagement report, four attack chains.
- `../WORK/pic18_findings.md` — 44-finding index + IOC/parameter master list.
- DS39592E (PIC18FX220/X320 Flash Programming Spec) and DS39599C / DS39689E
  datasheets — in `../WORK/`.
- bunnie, "Hacking the PIC 18F1320" — the optical/UV analogue of this break.
