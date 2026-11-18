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
  CH1 = GP2   (normal glitch output)
  CH2 = GP22  (GLITCH_FIRED marker — scope trigger, GP22->GP2 = PAUSE)
  CH3 = target rail at the FET drain (VDD or VCAP) — the crowbar droop/recovery
  CH4 = GP12  (crowbar gate, EXTERNAL mode only)
  GND -> Pico GND (common with target GND). Trigger: CH2/GP22 rising, ~200 ns/div.
  Bring-up order: FET DRAIN DISCONNECTED first (confirm the GP12 gate waveform),
  then connect the rail and watch CH3 collapse/recover.

CROWBAR SAFETY (read before wiring a FET)
  * The crowbar runs in EXTERNAL mode ONLY. Never INTERNAL: INTERNAL makes GP12 a
    ganged power OUTPUT (driven HIGH), latching a non-inverting gate ON into a
    sustained short, and a GP2-driven crowbar in INTERNAL shorts the RP2350's own
    source pins. This script rests the bench de-energized in EXTERNAL (gate idles
    de-asserted); it will NOT run the INTERNAL baseline unless you pass --allow-int
    (use that only with NO crowbar FET attached).
  * GP12 must be on its own signal line to the gate driver, never strapped to the
    GP10/11 ganged power harness.

PSU CURRENT-LIMIT PROTECTION (--psu, Pico-driven PSU over UART)
  Set the supply's current limit BELOW the FET's safe sustained current and ABOVE
  the target's real draw: a stuck-on short then folds the PSU back to CC (harmless)
  instead of feeding a dead short. The PSU is too slow to source the ns crowbar peak
  (that comes from rail/output capacitance), so a modest limit doesn't weaken the
  glitch. Rule of thumb: ILIM ~= 1.5-3x idle draw, <= FET safe sustained.
    Bare F401 MCU VDD : --volts 3300 --ilimit-ma 100
    Whole Nucleo @3V3 : --volts 3300 --ilimit-ma 250
  Note the supplied node (VDD, 3.3 V) may differ from the crowbarred node (VCAP,
  internal ~1.2 V) — you still feed VDD at 3.3 V. Without --psu (USB-driven PSU),
  the script prints the recommended limit for you to dial in by hand.

USAGE
  python3 crowbar_scope_test.py                 # all EXT scenarios, 50 shots each, paced
  python3 crowbar_scope_test.py --psu --volts 3300 --ilimit-ma 100   # Pico sets PSU protection
  python3 crowbar_scope_test.py --shots 200     # more shots per scenario (scope persistence)
  python3 crowbar_scope_test.py --only 5        # run only scenario #5
  python3 crowbar_scope_test.py --allow-int     # also run the INTERNAL baseline (NO FET attached!)
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
               "GP12 is a static power output here (no crowbar)."),
    dict(name="EXTERNAL AHIGH, single pulse", mode="EXT", pol="AHIGH",
         pause=0, width=75, gap=75, count=1,
         scope="GP12 idles LOW, one ~500ns HIGH pulse coincident with GP2."),
    dict(name="EXTERNAL ALOW, single pulse (inverted gate)", mode="EXT", pol="ALOW",
         pause=0, width=75, gap=75, count=1,
         scope="GP12 idles HIGH, one ~500ns LOW pulse (inverse of GP2). No trailing dip."),
    dict(name="EXTERNAL AHIGH, COUNT=3 burst", mode="EXT", pol="AHIGH",
         pause=0, width=75, gap=75, count=3,
         scope="GP12: 3 HIGH pulses (~500ns) with ~500ns gaps, matching GP2. All 3 present."),
    dict(name="EXTERNAL ALOW, COUNT=3 burst (inverted)", mode="EXT", pol="ALOW",
         pause=0, width=75, gap=75, count=3,
         scope="GP12: 3 LOW pulses with ~500ns gaps (inverse of GP2). All 3 present, no extra edges."),
    dict(name="EXTERNAL AHIGH, narrow pulse (WIDTH=15 ~100ns)", mode="EXT", pol="AHIGH",
         pause=0, width=15, gap=30, count=1,
         scope="GP12: one narrow ~100ns HIGH pulse == GP2 width. Zoom to ~50ns/div."),
    dict(name="EXTERNAL AHIGH, wide pulse (WIDTH=300 ~2us)", mode="EXT", pol="AHIGH",
         pause=0, width=300, gap=75, count=1,
         scope="GP12: one wide ~2us HIGH pulse == GP2, NOT truncated (soft-disarm lets the train finish)."),
    dict(name="EXTERNAL AHIGH, PAUSE delay (PAUSE=1500 ~10us)", mode="EXT", pol="AHIGH",
         pause=1500, width=75, gap=75, count=1,
         scope="~10us flat after the GLITCH trigger, THEN the GP12+GP2 pulse. Trigger on GP12 rising."),
    dict(name="EXTERNAL AHIGH, COUNT=5 long-ish train", mode="EXT", pol="AHIGH",
         pause=0, width=150, gap=150, count=5,
         scope="GP12: 5 full ~1us pulses with ~1us gaps (≈10us train). None truncated, no stuck pin."),
]


def psu_protect(r, volts_mv, ilimit_ma, use_psu):
    """Apply supply protection BEFORE any firing.

    With use_psu, claim the Pico-driven PSU (combo mode: GP10/11 = PSU UART, GP12 =
    crowbar gate) and push VOLT + CURR so a stuck-on short folds the PSU back to CC.
    Does NOT power the rail on — bring-up is drain-disconnected first, then you
    TARGET POWER ON when ready. Without use_psu (USB-driven PSU), just print the
    recommended limit to dial in by hand. Returns True if protection is in place."""
    print(c("1;35", "\n--- PSU current-limit protection ---"))
    if not use_psu:
        print(f"  USB-driven PSU: set it to {volts_mv/1000:.2f} V, current limit "
              f"{ilimit_ma} mA by hand BEFORE connecting the FET drain.")
        return True
    r.cmd("ARM OFF")
    idn = r.cmd("PSU ID", wait=2)
    if "PSU UART claimed" not in idn and "Active: yes" not in r.cmd("PSU"):
        print(c("31", f"  PSU not claimed (got: {idn.strip()!r}) — aborting --psu setup."))
        return False
    rv = r.cmd(f"PSU VOLT {volts_mv}", wait=1)
    ri = r.cmd(f"PSU CURR {ilimit_ma}", wait=1)
    print(f"  PSU VOLT {volts_mv} mV -> {rv.strip()}")
    print(f"  PSU CURR {ilimit_ma} mA -> {ri.strip()}")
    ok = "OK" in rv and "OK" in ri
    if not ok:
        print(c("31", "  PSU did not confirm VOLT/CURR — check the MAX3232 link."))
    else:
        print(c("32", f"  Protected: {volts_mv/1000:.2f} V, ILIM {ilimit_ma} mA. "
                       f"TARGET POWER ON when ready (drain connected)."))
    return ok


def configure(r, sc):
    if sc["mode"] == "EXT":
        r.cmd(f"TARGET POWER EXT {sc['pol']}")
    else:
        r.cmd("TARGET POWER INT")
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

    # Rest SAFELY for a crowbar bench: EXTERNAL (the GP12 gate idles de-asserted =
    # FET off, both polarities) and de-energized. NEVER INTERNAL here — that drives
    # GP12 as a ganged power output and can latch the FET on.
    r.cmd("ARM OFF")
    r.cmd("TARGET POWER EXT")
    r.cmd("TARGET POWER OFF")
    return ok


def main():
    ap = argparse.ArgumentParser(description="Crowbar/glitch scope-validation scenarios")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--shots", type=int, default=50, help="shots per scenario (>=1)")
    ap.add_argument("--rate", type=float, default=25.0, help="approx shots/sec in the burst")
    ap.add_argument("--only", type=int, default=None, help="run only this scenario number")
    ap.add_argument("--pause-between", type=float, default=2.0,
                    help="seconds to pause between scenarios (time to adjust the scope)")
    ap.add_argument("--psu", action="store_true",
                    help="Pico-driven PSU: claim it and set VOLT/CURR protection before firing")
    ap.add_argument("--volts", type=int, default=3300, help="PSU output voltage (mV), with --psu")
    ap.add_argument("--ilimit-ma", type=int, default=150,
                    help="PSU current limit (mA): >= target idle draw, <= FET safe sustained")
    ap.add_argument("--allow-int", action="store_true",
                    help="also run the INTERNAL baseline scenario (ONLY with NO crowbar FET attached)")
    ap.add_argument("--list", action="store_true", help="list scenarios and exit")
    args = ap.parse_args()

    if args.list:
        for i, sc in enumerate(SCENARIOS, 1):
            gate = "" if (sc["mode"] == "EXT" or args.allow_int) else "  [skipped: INT, needs --allow-int]"
            print(f"{i:2d}. {sc['name']}{gate}")
        return 0

    r = Raiden(args.port)
    print(c("1", f"Connected to {args.port}."))
    # Protection first, always — before any scenario can fire.
    psu_protect(r, args.volts, args.ilimit_ma, args.psu)
    results = []
    try:
        for i, sc in enumerate(SCENARIOS, 1):
            if args.only and i != args.only:
                continue
            if sc["mode"] == "INT" and not args.allow_int:
                print(c("33", f"\n=== Scenario {i}: {sc['name']} — SKIPPED "
                              f"(INTERNAL; pass --allow-int, and only with no crowbar FET) ==="))
                continue
            ok = run_scenario(r, i, sc, max(1, args.shots), args.rate)
            results.append((i, sc["name"], ok))
            if not args.only:
                time.sleep(args.pause_between)
    finally:
        # Leave the bench safe: disarmed, EXTERNAL (gate de-asserted), de-energized.
        r.cmd("ARM OFF")
        r.cmd("TARGET POWER EXT")
        r.cmd("TARGET POWER OFF")
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
