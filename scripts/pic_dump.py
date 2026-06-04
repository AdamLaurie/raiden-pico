#!/usr/bin/env python3
"""
pic_dump.py - Read PIC18 program memory over ICSP to a .bin file.

On a code-protected part, protected blocks read back as 0x00 (WORK FIND-102):
that is expected and is itself useful - it is the baseline a successful glitch
deviates from. On an OPEN part (or after a CP bypass) this reads the real code.

Default region is the 8 KB FX320 code array (0x000000..0x001FFF). Adjust with
--addr/--size for other parts (read pic_info.py first; the F4321 family has a
different map - FIND-108).

A small post-dump report shows how many bytes are non-zero, so you can tell at a
glance whether you captured real code or an all-zero protected read.

Examples:
  ./pic_dump.py -o dump.bin                       # 0x0..0x1FFF (8 KB)
  ./pic_dump.py --addr 0x1800 --size 0x800 -o blk3.bin
  ./pic_dump.py --addr 0x300000 --size 14 -o config.bin   # CONFIG words (always readable)
"""

import argparse
from colors import ColorHelpFormatter
import sys
import time

from pic_icsp import PicLink, add_common_args, project_path


def _progress(done, total):
    pct = done * 100 // total if total else 100
    print(f"\r  {done}/{total} bytes ({pct}%)", end="", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=ColorHelpFormatter)
    add_common_args(ap)
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=0x000000,
                    help="start byte address (default 0x000000)")
    ap.add_argument("--size", type=lambda x: int(x, 0), default=0x2000,
                    help="number of bytes (default 0x2000 = 8 KB)")
    ap.add_argument("--chunk", type=lambda x: int(x, 0), default=256,
                    help="bytes per ICSP DUMP call (default 256)")
    ap.add_argument("-o", "--output",
                    default=project_path(f"pic_dump_{time.strftime('%Y%m%d_%H%M%S')}.bin"),
                    help="output file (default: a UNIQUE timestamped file in scripts/pic18/, "
                         "so dumps never overwrite prior findings)")
    args = ap.parse_args()

    link = PicLink(port=args.port, verbose=args.verbose, lvp=args.lvp)
    try:
        devid, protected = link.status()
        if devid is None:
            print("PIC18: NOT CONNECTED - run pic_info.py to debug wiring/power/LVP.")
            return 1
        print(f"DEVID 0x{devid:04X}  ({'PROTECTED' if protected else 'OPEN'})")
        print(f"Dumping {args.size} bytes from 0x{args.addr:06X} -> {args.output}")
        data = link.dump(args.addr, args.size, chunk=args.chunk, progress=_progress)
        print()
    finally:
        link.close()

    with open(args.output, "wb") as f:
        f.write(data)

    nonzero = sum(1 for b in data if b)
    print(f"Saved {len(data)} bytes to {args.output}")
    print(f"  non-zero: {nonzero}/{len(data)} "
          f"({nonzero * 100 // len(data) if data else 0}%)")
    if protected and nonzero == 0:
        print("  -> all zero, as expected for a protected block. Use pic_glitch.py.")
    elif protected and nonzero:
        print("  -> NON-ZERO on a protected part: possible leak/partial bypass - inspect!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
