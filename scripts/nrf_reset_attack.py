#!/usr/bin/env python3
"""
nrf_reset_attack.py -- nRF52840 APPROTECT glitch via nRST RESET (warm boot).

⚠️ COUPLING-LIMITED — DOES NOT WORK ON THIS RIG. Use the cold power-cycle attack
(nrf_attack.py / nrf_autopwn.py) instead. On a warm reset DEC1 is already charged and
the LDO holds it in steady-state high-drive, so the crowbar can't collapse it: ~2M
attempts across width 165-295 x delay 0-60us gave 0 unlocks, while the SAME widths crack
the cold boot 3-7x. Kept for completeness / in case a hardware change (deeper coupling)
later makes the warm boot glitchable. Wiring: ../NRF52840_WIRING_RESET.md.

Mechanism: drives the firmware's `TARGET GLITCH APPROTECTRST` -- VDD stays ON and each
attempt reboots via nRST (GP15 -> P0.18). The warm boot is fast (DEC1 up, no rail ramp),
so the APPROTECT read is EARLY: app-start ~7-8us after the release edge, read precedes it
(~1-2us / 50-150 cyc per LayerOne). Sweep delay ~0-8us, NOT hundreds.

The firmware pre-flight aborts if the part is already open (honest AHB-read check). On a
(hypothetical) unlock it dumps the full flash + RAM, same as the power-cycle attack.

--ctrlap: reboot over SWD via CTRL-AP RESET instead of the GP15 nRST pin (firmware
`TARGET GLITCH APPROTECTCAP`). Needs NO physical reset pad -- useful when the board's
nRST is unfindable. Still a warm reset (same coupling limit), and the release edge is
SWD-jittered, so timing is looser than a hardware nRST edge.

Examples:
  ./nrf_reset_attack.py                                    # early window 0-8us x width 225-265
  ./nrf_reset_attack.py --d0 0 --d1 8 --w0 225 --w1 265 --tries 200000
  ./nrf_reset_attack.py --ctrlap                           # CTRL-AP reset (no nRST pad)
"""
import argparse
from colors import ColorHelpFormatter
import os
import sys
import time

from nrf_attack import Pico, find_port
from nrf_recovery import (parse_hexdump, run_recovery, dump_flash_and_ram,
                          project_path, pretty_info)

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=ColorHelpFormatter)
    ap.add_argument("--pico-port", default=None, help="Pico CDC port (default: auto-detect)")
    ap.add_argument("--d0", type=int, default=0, help="delay sweep start, us from nRST release")
    ap.add_argument("--d1", type=int, default=8, help="delay sweep end, us (read is ~1-8us after release)")
    ap.add_argument("--dstep", type=int, default=1, help="delay step, us")
    ap.add_argument("--w0", type=int, default=225, help="width start, cyc (validated cold-boot band; 6.67ns/cyc)")
    ap.add_argument("--w1", type=int, default=265, help="width end, cyc")
    ap.add_argument("--wstep", type=int, default=2, help="width step, cyc")
    ap.add_argument("--ctrlap", action="store_true",
                    help="reboot via CTRL-AP RESET over SWD (no nRST pad needed)")
    ap.add_argument("--reset-hold", dest="hold", type=int, default=200, help="reset low hold, us")
    ap.add_argument("--settle", type=int, default=4, help="wait after glitch before SWD probe, ms")
    ap.add_argument("--tries", type=int, default=200000, help="max attempts before giving up")
    ap.add_argument("--dump-bytes", dest="dump", type=int, default=0,
                    help="bytes to dump on unlock (0 = full 1MB flash)")
    ap.add_argument("--out", default=project_path("nrf_flash_dump.bin"),
                    help="flash dump file on unlock (default: scripts/nrf52840/)")
    ap.add_argument("--log", default=project_path("nrf_reset_attack.log"),
                    help="progress log, flushed (default: scripts/nrf52840/)")
    a = ap.parse_args()

    try:
        lf = open(a.log, "a", buffering=1)
        lf.write(f"\n===== reset-attack {time.strftime('%Y-%m-%d %H:%M:%S')} =====\n")
    except Exception:
        lf = None

    def out(s):
        print(s, flush=True)
        if lf:
            lf.write(s + "\n")

    port = find_port(a.pico_port)
    out(f"[pico] {port}")
    p = Pico(port)
    try:
        p.cmd("TARGET NRF52840", 0.4)
        cmd = "APPROTECTCAP" if a.ctrlap else "APPROTECTRST"
        reboot = "CTRL-AP RESET over SWD (no pad)" if a.ctrlap else "nRST(GP15)"
        params = (f"{a.d0} {a.d1} {a.dstep} {a.w0} {a.w1} {a.wstep} "
                  f"{a.hold} {a.settle} {a.tries}")
        out(f">>> TARGET GLITCH {cmd} {params}   [reboot: {reboot}]")
        out(f"    delay {a.d0}-{a.d1}/{a.dstep}us (EARLY: read ~1-8us after release), "
            f"width {a.w0}-{a.w1}/{a.wstep}cyc")
        p.send(f"TARGET GLITCH {cmd} {params}")

        buf = b""; unlocked = False; aborted = False; start = time.time()
        n_disrupt = 0
        while True:
            n = p.s.in_waiting
            if n:
                buf += p.s.read(n)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    t = line.decode("utf-8", "replace").rstrip()
                    if not t or t.startswith("[SWD] Connected"):
                        continue
                    if "ALREADY OPEN" in t:
                        out("   " + t); aborted = True
                    if "DISRUPT" in t:
                        n_disrupt += 1; out("   " + t)
                    if "attempt " in t and "/" in t:
                        out("   " + t)
                    if "UNLOCKED on attempt" in t:
                        out("   " + t); unlocked = True
                    if "did not unlock" in t or "No unlock" in t:
                        out(f"--- sweep ended: no unlock (disrupts {n_disrupt}) ---")
                        return
                    if aborted and "Re-lock" in t:
                        out("ABORTED: part is already open. Re-lock first "
                            "(nrf_timing_marker.py relock), then retry.")
                        return
            else:
                if unlocked:
                    break
                time.sleep(0.03)

        # genuine unlock -> INFO + dump (full flash + RAM)
        out("\n*** GENUINE UNLOCK -- reading flash + RAM ***")
        out(pretty_info(p.cmd("TARGET NRF INFO", 1.2)))
        fn, rn, ram_out = dump_flash_and_ram(p, a.out, log=out)
        out(f"saved flash {fn} bytes -> {a.out}")
        out(f"saved RAM {rn} bytes -> {ram_out}" if rn else "WARN: RAM dump 0 bytes")
        try:
            run_recovery(p, apply=False)   # dry-run only; never auto-mutate on an unwatched unlock
        except Exception as e:
            out(f"[recovery] skipped: {e}")
        out(f"SUCCESS after {int(time.time()-start)}s")
    except KeyboardInterrupt:
        out("[interrupted]")
    finally:
        p.close()


if __name__ == "__main__":
    main()
