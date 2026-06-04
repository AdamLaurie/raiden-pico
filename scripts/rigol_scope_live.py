#!/usr/bin/env python3
"""
rigol_scope_live.py -- live Rigol scope capture loop: configure the channels/trigger,
then repeatedly grab a screenshot to a viewable PNG. Headless (writes a PNG) -- pair with
rigol_view.py to display it, or open the PNG in any image viewer.

LAN-ONLY: talks to the Rigol over VXI-11 (`lxi`) and never opens the Pico serial port, so it
runs in PARALLEL with an attack (nrf_attack.py / nrf_autopwn.py) without contention.

The default channel setup is the nRF52840 glitch rig: CH1 = GP2 (the glitch pulse, used as the
trigger) and CH2 = P0.13 (the boot marker), RUN / NORMAL sweep. Each attack shot fires GP2 so the
scope re-triggers and the frame tracks the latest glitch; read boot survival off CH2 (marker rises
= boot ok; marker gone = the glitch crashed the boot). Override --trig-ch/--trig-level/--tb for
other rigs.

Output goes to a per-target folder like the other tools (default scripts/nrf52840/scope_live.png);
--dir picks another target (e.g. pic18) or a path. Refreshes every --interval seconds.

Examples:
  ./rigol_scope_live.py                            # CH1=GP2, CH2=P0.13, 50 us/div, 40 min
  ./rigol_scope_live.py --interval 1 --secs 7200   # faster refresh, run 2 h
  ./rigol_scope_live.py --tb 0.0001 --dir pic18    # 100 us/div, write into scripts/pic18/
"""
import argparse
import os
import subprocess
import time

from colors import ColorHelpFormatter
from nrf_recovery import resolve_outdir   # shared per-target output-folder helper


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=ColorHelpFormatter)
    ap.add_argument("--scope-ip", default="10.0.0.10", help="Rigol scope IP (lxi)")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between screenshots")
    ap.add_argument("--tb", default="0.00005", help="scope timebase, s/div (default 50us)")
    ap.add_argument("--secs", type=float, default=2400.0, help="total run time, s")
    ap.add_argument("--trig-ch", type=int, default=1, help="scope channel to trigger on (default 1 = GP2)")
    ap.add_argument("--trig-level", type=float, default=1.5, help="trigger level, V")
    ap.add_argument("--dir", default=None,
                    help="destination folder: a target name (scripts/<name>/) or a path "
                         "(default: scripts/nrf52840/)")
    ap.add_argument("--out", default=None,
                    help="viewable PNG to (re)write each frame (default: <dir>/scope_live.png)")
    a = ap.parse_args()
    out = a.out or os.path.join(resolve_outdir(a.dir), "scope_live.png")

    def lxi(scpi, t=8):
        r = subprocess.run(["lxi", "scpi", "-a", a.scope_ip, "-t", str(t), scpi],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"{scpi!r}: {r.stderr.strip()}")
        return r.stdout.strip()

    lxi(":CHANnel1:DISPlay ON"); lxi(":CHANnel1:COUPling DC")
    lxi(":CHANnel1:SCALe 1.0"); lxi(":CHANnel1:OFFSet -2.0")     # CH1 (nRF rig: GP2 glitch pulse)
    lxi(":CHANnel2:DISPlay ON"); lxi(":CHANnel2:COUPling DC")
    lxi(":CHANnel2:SCALe 1.0"); lxi(":CHANnel2:OFFSet -2.0")     # CH2 (nRF rig: P0.13 marker)
    lxi(f":TIMebase:MAIN:SCALe {a.tb}")
    lxi(":TIMebase:MAIN:OFFSet 0")          # trigger centred; marker sits to its right
    lxi(":TRIGger:MODE EDGE"); lxi(f":TRIGger:EDGE:SOURce CHANnel{a.trig_ch}")
    lxi(":TRIGger:EDGE:SLOPe POSitive"); lxi(f":TRIGger:EDGE:LEVel {a.trig_level}")
    lxi(":TRIGger:SWEep NORMal")
    lxi(":RUN")
    print(f"scope live: CH{a.trig_ch}=trigger, {a.tb}s/div, NORM/RUN -> {out} "
          f"(refresh {a.interval}s, {a.secs:.0f}s total)")

    raw = out + ".raw"
    t0 = time.time(); n = 0
    while time.time() - t0 < a.secs:
        subprocess.run(["lxi", "screenshot", "-a", a.scope_ip, "-t", "15", raw],
                       capture_output=True, text=True)
        try:
            from PIL import Image
            Image.open(raw).save(out)
        except Exception:
            pass
        n += 1
        if n % 15 == 0:
            print(f"[{int(time.time()-t0)}s] {n} frames")
        time.sleep(a.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped")
