# TODO

## Finish PSU control on the Pico (protocol verified; Pico link pending)

**Protocol is bench-verified** against a real TENMA 72-2540 V5.9 (host ↔ PSU via
USB-RS232): 9600 8N1, no terminator, `VSET1:`/`ISET1:` write formats and
`*IDN?` / `VSET1?` / `VOUT1?` / `STATUS?` reads all match `psu.c`. STATUS bits
decoded (bit0 CV/CC, bit4 beep, bit5 lock, bit6 output). No firmware protocol
changes needed.

**Remaining (needs the Pico + correct RS232↔TTL converter at home):**
- Wire `PSU DB9 → RS232-TTL converter → Pico GP10/11`. DB9 is **straight-through**
  (PSU is DCE — confirmed with the USB-RS232 cable). TTL side: GP10 → converter
  RXD, GP11 → converter TXD, converter Vcc = **3.3 V**, GND common.
- Flash the PSU firmware and run `PSU ID` over the CLI — expect
  `TENMA 72-2540 V5.9 SN:...`. Then `PSU VOLT/CURR/ON/OFF/STATUS`.
- The Pico's GP10 TX is already confirmed emitting clean `*IDN?` at 9600; the
  only unproven leg is the converter + the GP11 return path.
- **Not yet flash-tested on the Pico** (Pico was disconnected during protocol
  bring-up) — flash + retest before merge, per the version-bump skill.
