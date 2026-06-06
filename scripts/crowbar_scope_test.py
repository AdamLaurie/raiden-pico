#!/usr/bin/env python3
"""
crowbar_scope_test.py — comprehensive glitch / crowbar-gate scenarios for scope validation.

Walks a series of named scenarios over the Raiden Pico CLI (USB CDC, /dev/ttyACM0).
For each scenario it:
  * configures power mode / crowbar polarity / PAUSE,WIDTH,GAP,COUNT,
  * fires via the manual ``GLITCH`` command (TRIGGER NONE),
  * VERIFIES on the firmware side (no scope needed): the glitch count increments
    by exactly the number of shots, and the system auto-disarms after each fire
    (``ARM`` -> ``DISARMED``),
  * prints what you should SEE on the scope (which pins, polarity, pulse count, width).

So you watch the scope while the script confirms the firmware is firing and
disarming correctly. A terminal BEL (\\a) sounds on every shot so you can count
shots against scope events.

SCOPE PROBES
  CH1 = GP2  (normal glitch output)
  CH2 = GP11 (crowbar gate, EXTERNAL mode only)   GND -> Pico GND
  Trigger: GP2 rising (AHIGH scenarios) or GP11 — normal mode, ~200 ns/div.

WIRING NOTE
  For the EXTERNAL scenarios, GP11 must be on its own line (NOT strapped to the
  GP10/12 ganged power harness) or driving GP11 fights GP10. Firmware-side checks
  (count / disarm) work regardless of wiring.

USAGE
  python3 crowbar_scope_test.py                 # all scenarios, 50 shots each, paced
  python3 crowbar_scope_test.py --shots 200     # more shots per scenario (scope persistence)
  python3 crowbar_scope_test.py --only 5        # run only scenario #5
  python3 crowbar_scope_test.py --list          # list scenarios and exit
  python3 crowbar_scope_test.py --port /dev/ttyACM1 --rate 30
"""
import argparse
import re
import sys
import time

import serial  # pyserial

CObj = "\033[0m"  # reset

def c(code, s):
    return f"\033[{code}m{s}{CObj}"


class Raiden:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=0.5)

    def cmd(self, command, wait=0.25):
        self.s.reset_input_buffer()
        self.s.write((command + "\r\n").encode())
        time.sleep(wait)
        return self.s.read(self.s.in_waiting or 1).decode(errors="replace")

    def status(self):
        return self.cmd("STATUS", wait=0.5)

    def glitch_count(self):
        m = re.search(r"Glitch Count:\s*(\d+)", self.status())
        return int(m.group(1)) if m else None

    def armed(self):
        r = self.cmd("ARM")
        return "ARMED" in r and "DISARMED" not in r

    def close(self):
        self.s.close()


# Each scenario: configure, then the scope expectation string.
# mode: "INT" | "EXT";  pol: None | "AHIGH" | "ALOW"
SCENARIOS = [
    dict(name="INTERNAL baseline (no crowbar)", mode="INT", pol=None,
         pause=0, width=75, gap=75, count=1,
         scope="GP2: one ~500ns HIGH pulse. GP7 (if probed): inverted. "
               "GP11 is a static power output here (no crowbar)."),
    dict(name="EXTERNAL AHIGH, single pulse", mode="EXT", pol="AHIGH",
         pause=0, width=75, gap=75, count=1,
         scope="GP11 idles LOW, one ~500ns HIGH pulse coincident with GP2."),
    dict(name="EXTERNAL ALOW, single pulse (inverted gate)", mode="EXT", pol="ALOW",
         pause=0, width=75, gap=75, count=1,
         scope="GP11 idles HIGH, one ~500ns LOW pulse (inverse of GP2). No trailing dip."),
    dict(name="EXTERNAL AHIGH, COUNT=3 burst", mode="EXT", pol="AHIGH",
         pause=0, width=75, gap=75, count=3,
         scope="GP11: 3 HIGH pulses (~500ns) with ~500ns gaps, matching GP2. All 3 present."),
    dict(name="EXTERNAL ALOW, COUNT=3 burst (inverted)", mode="EXT", pol="ALOW",
         pause=0, width=75, gap=75, count=3,
         scope="GP11: 3 LOW pulses with ~500ns gaps (inverse of GP2). All 3 present, no extra edges."),
    dict(name="EXTERNAL AHIGH, narrow pulse (WIDTH=15 ~100ns)", mode="EXT", pol="AHIGH",
         pause=0, width=15, gap=30, count=1,
         scope="GP11: one narrow ~100ns HIGH pulse == GP2 width. Zoom to ~50ns/div."),
    dict(name="EXTERNAL AHIGH, wide pulse (WIDTH=300 ~2us)", mode="EXT", pol="AHIGH",
         pause=0, width=300, gap=75, count=1,
         scope="GP11: one wide ~2us HIGH pulse == GP2, NOT truncated (soft-disarm lets the train finish)."),
    dict(name="EXTERNAL AHIGH, PAUSE delay (PAUSE=1500 ~10us)", mode="EXT", pol="AHIGH",
         pause=1500, width=75, gap=75, count=1,
         scope="~10us flat after the GLITCH trigger, THEN the GP11+GP2 pulse. Trigger on GP11 rising."),
    dict(name="EXTERNAL AHIGH, COUNT=5 long-ish train", mode="EXT", pol="AHIGH",
         pause=0, width=150, gap=150, count=5,
         scope="GP11: 5 full ~1us pulses with ~1us gaps (≈10us train). None truncated, no stuck pin."),
]


def configure(r, sc):
    if sc["mode"] == "EXT":
        r.cmd(f"TARGET POWER MODE EXT {sc['pol']}")
    else:
        r.cmd("TARGET POWER MODE INT")
    r.cmd("TRIGGER NONE")
    r.cmd(f"SET PAUSE {sc['pause']}")
    r.cmd(f"SET WIDTH {sc['width']}")
    r.cmd(f"SET GAP {sc['gap']}")
    r.cmd(f"SET COUNT {sc['count']}")


def verify_fire_once(r):
    """Fire one glitch and verify count++ and auto-disarm. Returns (ok, detail)."""
    before = r.glitch_count()
    arm = r.cmd("ARM ON")
    if "armed" not in arm.lower():
        return False, f"ARM ON did not arm (got: {arm.strip()!r})"
    g = r.cmd("GLITCH")
    if "Glitch executed" not in g:
        return False, f"GLITCH not accepted (got: {g.strip()!r})"
    after = r.glitch_count()
    if before is None or after is None:
        return False, "could not read Glitch Count from STATUS"
    if after != before + 1:
        return False, f"glitch count {before} -> {after} (expected +1)"
    if r.armed():
        return False, "still ARMED after GLITCH (auto-disarm failed)"
    return True, f"count {before}->{after}, auto-disarmed"


def run_scenario(r, idx, sc, shots, rate):
    print(c("1;36", f"\n=== Scenario {idx}: {sc['name']} ==="))
    print(f"  config: mode={sc['mode']} pol={sc['pol']} "
          f"PAUSE={sc['pause']} WIDTH={sc['width']} GAP={sc['gap']} COUNT={sc['count']}")
    print(c("33", f"  SCOPE: {sc['scope']}"))
    configure(r, sc)

    # 1) Firmware verification on the first shot (precise check).
    ok, detail = verify_fire_once(r)
    tag = c("32", "PASS") if ok else c("31", "FAIL")
    print(f"  verify: [{tag}] {detail}")

    # 2) Repeat for scope persistence; confirm the count moved by `shots-1` more.
    if shots > 1:
        base = r.glitch_count()
        delay = max(0.0, 1.0 / rate - 0.024)
        for i in range(shots - 1):
            r.cmd("ARM ON", wait=0.012)
            r.cmd("GLITCH", wait=0.012)
            sys.stdout.write("\a"); sys.stdout.flush()
            time.sleep(delay)
        end = r.glitch_count()
        moved = (end - base) if (base is not None and end is not None) else None
        exp = shots - 1
        ok2 = (moved == exp)
        tag2 = c("32", "PASS") if ok2 else c("31", "FAIL")
        print(f"  burst:  [{tag2}] fired {shots-1} more shots, count moved {moved} (expected {exp})")
        ok = ok and ok2

    # leave the part disarmed; INTERNAL is the safe resting mode
    r.cmd("TARGET POWER MODE INT")
    return ok


def main():
    ap = argparse.ArgumentParser(description="Crowbar/glitch scope-validation scenarios")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--shots", type=int, default=50, help="shots per scenario (>=1)")
    ap.add_argument("--rate", type=float, default=25.0, help="approx shots/sec in the burst")
    ap.add_argument("--only", type=int, default=None, help="run only this scenario number")
    ap.add_argument("--pause-between", type=float, default=2.0,
                    help="seconds to pause between scenarios (time to adjust the scope)")
    ap.add_argument("--list", action="store_true", help="list scenarios and exit")
    args = ap.parse_args()

    if args.list:
        for i, sc in enumerate(SCENARIOS, 1):
            print(f"{i:2d}. {sc['name']}")
        return 0

    r = Raiden(args.port)
    print(c("1", f"Connected to {args.port}. {r.cmd('VERSION').strip().splitlines()[-1] if False else ''}"))
    results = []
    try:
        for i, sc in enumerate(SCENARIOS, 1):
            if args.only and i != args.only:
                continue
            ok = run_scenario(r, i, sc, max(1, args.shots), args.rate)
            results.append((i, sc["name"], ok))
            if not args.only:
                time.sleep(args.pause_between)
    finally:
        r.cmd("TARGET POWER MODE INT")
        r.cmd("ARM OFF")
        r.close()

    print(c("1", "\n===== SUMMARY ====="))
    passed = sum(1 for _, _, ok in results if ok)
    for i, name, ok in results:
        tag = c("32", "PASS") if ok else c("31", "FAIL")
        print(f"  [{tag}] {i}. {name}")
    print(c("1", f"\n{passed}/{len(results)} scenarios verified on the firmware side."))
    print("  (Scope-side waveform shape is yours to confirm against the SCOPE: lines above.)")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
