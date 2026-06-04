# PIC18 ICSP code-protect (CP) bypass — tooling

Host-side tools for reading a **PIC18** over **ICSP** and bypassing flash **code-protect (CP)**
with a VDD crowbar glitch on the Raiden Pico. Deep dive (protocol, attack theory, findings):
[`../PIC18_ICSP.md`](../PIC18_ICSP.md). Engagement analysis + attack chains: `WORK/pic18_report.md`,
`WORK/pic18_findings.md` (Chain 1 = the ICSP read-glitch these tools drive).

**The attack (Chain 1):** on a CP'd part an ICSP **Table Read** still *executes*, but CP gates the
byte onto PGD so it reads back **0x00**. A **VDD crowbar** (GP2) fired into the Table-Read clock-out
faults that gate, so the protected byte returns its **true value** instead of 0x00 (WORK FIND-102/105/300).
DEVID / CONFIG / ID words read in the clear **even when protected** (FIND-102/402) — so identification
is free and non-destructive. **LVP entry — no 9–13 V VPP driver needed, GPIO only** (FIND-207).

> ⚠️ Status: the firmware ICSP + glitch (`src/pic18_target.c`) and these host scripts are built but
> **UNTESTED on real hardware** yet. Run `pic_info.py` first to confirm the link before any glitch.

All tools talk to the Pico over USB-CDC (`/dev/ttyACM*`; `--port` or `PIC_PORT` env to override).
**Outputs go to `scripts/pic18/`** (created on demand; keeps `scripts/` clean) — override with `-o`.

## Wiring (firmware-fixed pins)
The crowbar is on the **VDD rail** here (not a core rail like the nRF). See [`../PIC18_ICSP.md`](../PIC18_ICSP.md) §2.

| Pico2 GPIO | Signal | PIC18 pin | Direction |
|---|---|---|---|
| **GP17** | ICSP **PGC** clock | PGC / RB6 | Pico → target |
| **GP18** | ICSP **PGD** data | PGD / RB7 | bidirectional |
| **GP15** | **MCLR/VPP** (LVP entry) | MCLR/VPP | Pico → target |
| **GP2** | **crowbar** glitch | target **VDD** rail | Pico → target |
| GP10/11/12 | target **power** | VDD switch (`TARGET POWER`) | Pico → target |
| GP22 | GLITCH_FIRED marker | (scope trigger only) | Pico → scope |
| GND | common ground | VSS | — |

Tie target VSS, the crowbar return, and the scope ground to **one** node.

## The 3-step flow
1. **Identify** — `pic_info.py` (safe, non-destructive; works through CP)
   ```
   ./pic_info.py                 # DEVID + silicon rev + which blocks are CP'd
   ```
   Always run first: confirms the ICSP link (DEVID ≠ 0x0000/0xFFFF), pins down the exact part so you
   pick the right block map (FIND-108: the F4321 family differs from the FX320 family), and shows which
   blocks are actually protected so you only attack CP'd ones.

2. **Glitch** — `pic_glitch.py` (the Chain-1 CP-bypass sweep)
   ```
   ./pic_glitch.py sweep --probe 0x001800 --dump-on-hit hit.bin   # delay x width search
   ./pic_glitch.py shot  --delay 1000 --width 80                  # single pulse for scope bring-up
   ```
   The sweep runs on the Pico (`TARGET PIC GLITCH`); this streams progress and detects the
   `*** CP BYPASS` hit line. `--probe` = the protected address to test; on a hit it can `--dump-on-hit`.
   Defaults: delay 0–50 µs step 1, width 30–150 cyc step 10. Park a scope on VDD + GP22 for `shot`.

3. **Dump** — `pic_dump.py` (read program memory to a `.bin`)
   ```
   ./pic_dump.py --addr 0x0 --size 0x2000 -o pic_dump.bin
   ```
   On a CP'd part protected blocks read `0x00` (the baseline a glitch deviates from); on an OPEN part
   (or after a CP bypass) this reads real code. A post-dump report counts non-zero bytes so you can
   tell real code from an all-zero protected read. Default region = the 8 KB FX320 array
   (0x000000–0x001FFF); use `--addr/--size` for other parts (the F4321 family has a different map).

## What we read / dump (PIC18 ICSP address space)
| Region | Address | Tool | Notes |
|---|---|---|---|
| **Program / code flash** | `0x000000`… | `pic_dump.py` | **CP-gated** — `0x00` when protected; true bytes after a glitch |
| User ID | `0x200000`–`0x200007` | `pic_info.py` | readable in the clear |
| Config words (CONFIG1–7) | `0x300000`–`0x30000D` | `pic_info.py` | CP state lives in **CONFIG5L**; readable in the clear |
| Device ID (DEVID1/2) | `0x3FFFFE`–`0x3FFFFF` | `pic_info.py` | part + silicon revision; readable in the clear |
| Data EEPROM | `0xF00000`… | (EECON path) | separate access; may have its own CP |

## Files
- `pic_icsp.py` — shared host library: `PicLink` (drives `TARGET PIC …`), `add_common_args`, `part_name`,
  and the per-target output helpers `project_dir`/`project_path` (default folder `scripts/pic18/`).
- `pic_info.py` — identify DEVID / revision / CP state (safe first step).
- `pic_glitch.py` — the CP-bypass crowbar sweep (`sweep` / `shot`).
- `pic_dump.py` — read program memory to a `.bin` with a non-zero-byte report.

Firmware side: `src/pic18_target.c` (bit-banged ICSP + `pic18_glitch_cp`), command surface
`TARGET PIC18 | PIC INFO | STATUS | DUMP [addr] [bytes] | GLITCH … | SHOT … | POWER`.
