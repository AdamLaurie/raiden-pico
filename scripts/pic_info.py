#!/usr/bin/env python3
"""
pic_info.py - Identify a PIC18 over ICSP: DEVID, silicon revision, code-protect state.

This is the safe, non-destructive first step. DEVID, CONFIG words and ID
locations read out in the clear EVEN ON A PROTECTED PART (WORK FIND-102/402), so
this works regardless of CP and never glitches or erases anything.

ALWAYS run this before any glitch/dump campaign:
  - confirms the ICSP link (DEVID != 0x0000 / 0xFFFF),
  - tells you the exact part + revision so you pick the right block map
    (FIND-108: the F4321 family differs from the FX320 family), and
  - shows which blocks are actually protected, so you only attack CP'd blocks.

Examples:
  ./pic_info.py                 # /dev/ttyACM0
  ./pic_info.py -p /dev/ttyACM1 -v
"""

import argparse
from colors import ColorHelpFormatter
import sys

from pic_icsp import PicLink, add_common_args, part_name


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=ColorHelpFormatter)
    add_common_args(ap)
    args = ap.parse_args()

    link = PicLink(port=args.port, verbose=args.verbose, lvp=args.lvp)
    try:
        info = link.info()
    finally:
        link.close()

    if not info.get("connected"):
        print("PIC18: NOT CONNECTED - no DEVID.")
        print("  Check: wiring (PGC=GP17 PGD=GP18 MCLR=GP15, common GND),")
        print("         target powered (TARGET POWER ON / GP10-12), and LVP=1 on the part.")
        return 1

    devid = info["devid"]
    rev = info["revision"]
    name = part_name(devid)
    print("=== PIC18 ICSP Info ===")
    print(f"DEVID      : 0x{devid:04X}" + (f"  ({name})" if name else "  (part unknown - check datasheet DEVID table)"))
    print(f"Revision   : 0x{rev:02X} (REV4:0 = DEVID1<4:0>)")
    if info["config5l"] is not None:
        print(f"CONFIG5L   : 0x{info['config5l']:02X}")
    cp = info["cp"]
    decoded = " ".join(
        f"CP{i}={'PROT' if cp[i] else ('open' if cp[i] is not None else '?')}"
        for i in range(4)
    )
    print(f"Code prot. : {decoded}")
    print(f"State      : {'PROTECTED' if info['protected'] else 'OPEN (readable)'}")
    if info["protected"]:
        print()
        print("Protected. Next: pic_glitch.py to bypass CP, or pic_dump.py to confirm")
        print("a protected block currently reads back 0x00.")
    else:
        print()
        print("Open part - pic_dump.py will read it directly (no glitch needed).")
        print("Good candidate to validate the ICSP read path before attacking a CP'd part.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
