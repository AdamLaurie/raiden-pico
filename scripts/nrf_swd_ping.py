#!/usr/bin/env python3
"""nrf_swd_ping.py -- simple SWD liveness ping for the nRF52840 over the Raiden Pico.

Connects once and reads TARGET NRF STATUS, printing the DPIDR and APPROTECT state.
A healthy locked part answers with DPIDR=0x2BA01477 and APPROTECT=PROTECTED. Loop it
(-n) to shake out a flaky SWD joint: a solid link gives the same line every time,
while a wiggling/intermittent joint flip-flops between connected and failed.

Examples:
  ./nrf_swd_ping.py                  # single ping (default)
  ./nrf_swd_ping.py -n 20            # 20 pings -- good while wiggle-testing joints
  ./nrf_swd_ping.py -n 0             # ping forever until Ctrl-C
  ./nrf_swd_ping.py --pico-port /dev/ttyACM0
"""
import argparse
import os
import re
import sys
import time

# Work no matter the current directory (./script, run from repo root, etc.): put this
# script's own dir on sys.path before importing siblings.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nrf_attack import Pico, find_port
from colors import hdr, ok, warn, err, info, dim, green, val, ColorHelpFormatter

_DPIDR_RE = re.compile(r"DPIDR=(0x[0-9A-Fa-f]+)")


def parse_status(text):
    """Pull (connected, dpidr, approtect) out of a TARGET NRF STATUS reply."""
    up = text.upper()
    m = _DPIDR_RE.search(text)
    dpidr = m.group(1) if m else None
    connected = "CONNECTED, DPIDR" in up or "NRF: DPIDR" in up
    approtect = None
    if "APPROTECT=PROTECTED" in up:
        approtect = "PROTECTED"
    elif "APPROTECT=UNPROTECTED" in up:
        approtect = "UNPROTECTED"
    return connected, dpidr, approtect


def fmt_line(i, connected, dpidr, approtect):
    tag = dim(f"[{i}]")
    if not connected or not dpidr:
        return f"{tag} {err('FAIL')}  no DPIDR  (SWD did not connect)"
    state = approtect or "?"
    # UNPROTECTED on a locked target = the unlock we are hunting -> shout it green.
    sc = green(state) if state == "UNPROTECTED" else (ok(state) if state == "PROTECTED" else warn(state))
    return f"{tag} {ok('OK')}  DPIDR={val(dpidr)}  APPROTECT={sc}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=ColorHelpFormatter)
    ap.add_argument("--pico-port", default=None, help="Pico CDC port (default: auto-detect)")
    ap.add_argument("-n", "--count", type=int, default=1,
                    help="number of pings; 0 = loop forever until Ctrl-C (default: 1)")
    ap.add_argument("-i", "--interval", type=float, default=0.0,
                    help="delay between pings, seconds (default: 0)")
    a = ap.parse_args()

    port = find_port(a.pico_port)
    print(info(f"[pico] {port}"))
    p = Pico(port)
    seen_dpidr = set()
    n_ok = n_total = 0
    try:
        p.cmd("ARM OFF", 0.1)
        p.cmd("TARGET POWER ON", 0.4); time.sleep(0.4)
        p.cmd("TARGET NRF52840", 0.4)

        i = 0
        while a.count == 0 or i < a.count:
            i += 1; n_total += 1
            connected, dpidr, approtect = parse_status(p.cmd("TARGET NRF STATUS", 1.0))
            if connected and dpidr:
                n_ok += 1; seen_dpidr.add(dpidr)
            print(fmt_line(i, connected, dpidr, approtect))
            if a.interval and (a.count == 0 or i < a.count):
                time.sleep(a.interval)
    except KeyboardInterrupt:
        print(warn("\n[ping] interrupted"))
    finally:
        p.close()

    # summary (skip the noise for a single ping)
    if n_total > 1:
        line = f"{n_ok}/{n_total} connected"
        if n_ok == n_total and len(seen_dpidr) == 1:
            print(ok(f"\n>> {line} -- stable {seen_dpidr.pop()}"))
        elif n_ok == 0:
            print(err(f"\n>> {line} -- no SWD link"))
        else:
            print(warn(f"\n>> {line} -- INTERMITTENT (check joint/strain relief)"))


if __name__ == "__main__":
    main()
