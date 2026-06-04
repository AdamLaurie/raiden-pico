#!/usr/bin/env python3
"""
efm32_link.py - Host-side library for the EFM32 Leopard Gecko debug-unlock glitch.

The Pico2 (RP2350) firmware implements the EFM32LG SWD/glitch surface
(src/efm32_target.c). This module is the host counterpart: it opens the Pico's
USB-CDC CLI and drives the `TARGET EFM ...` / `TARGET GLITCH DEBUGUNLOCK[RST]`
commands, parsing the human-readable responses back into Python values.

Firmware command surface (see src/command_parser.c around "EFM"):
    TARGET EFM32                       - select the EFM32LG target type
    TARGET EFM INFO                    - DPIDR + lock state + DI page (part/flash/uid)
    TARGET EFM STATUS                  - one-line DPIDR + UNLOCKED/LOCKED
    TARGET EFM AAP                     - scan AP IDRs for the Authentication Access Port
    TARGET EFM ERASE                   - AAP DEVICEERASE recovery (DESTRUCTIVE)
    TARGET EFM DUMP [addr] [bytes]     - hex-dump memory over the AHB-AP (needs unlock)
    TARGET EFM SHOT [delay_us] [width] - one crowbar pulse, power-cycle (scope bring-up)
    TARGET EFM SHOTRST [delay] [w] [hold] - one crowbar pulse, nRST reboot (scope)
    TARGET GLITCH DEBUGUNLOCK    [d0 d1 dstep w0 w1 wstep off settle tries]
    TARGET GLITCH DEBUGUNLOCKRST [d0 d1 dstep w0 w1 wstep hold settle tries]

Pin map (firmware, fixed, shared with the nRF SWD module):
    SWCLK=GP17  SWDIO=GP18  nRST=GP15  crowbar->DECOUPLE=GP2  scope marker=GP22
    target power=GP10/11/12.  See EFM32_GLITCH.md for the wiring schematic.

Attack model: glitch the Debug Lock Word (DLW) latch in the tRESET ~163 us boot
window so the AHB-AP comes up enabled, then read flash over SWD WITHOUT triggering
the AAP mass-erase. Crowbar injects on the DECOUPLE pin (core LDO), not main VDD.
See WORK/efm32_findings.md.

Nothing here powers or glitches the target on import; every action is an explicit
method call. Default port /dev/ttyACM0 (override with --port / EFM_PORT env).
"""

import os
import sys
import time
import serial   # pyserial


DEFAULT_PORT = os.environ.get("EFM_PORT", "/dev/ttyACM0")
DEFAULT_BAUD = 115200

# Cortex-M3 SW-DP IDCODE (shared by many M3 parts; positive ID is the DI PART read).
EFM32_DPIDR_M3 = 0x2BA01477


def project_dir(name="efm32"):
    """Per-target output folder `scripts/<name>/` for dumps/logs, created on demand
    so the scripts/ dir stays clean."""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    os.makedirs(d, exist_ok=True)
    return d


def project_path(filename, name="efm32"):
    """Full path to `filename` inside the per-target output folder."""
    return os.path.join(project_dir(name), filename)


class EfmError(RuntimeError):
    pass


class EfmLink:
    """Thin wrapper around the Pico CLI for EFM32LG SWD/glitch operations."""

    def __init__(self, port=DEFAULT_PORT, baud=DEFAULT_BAUD, verbose=False):
        self.verbose = verbose
        self.ser = serial.Serial(port, baud, timeout=0.2)
        time.sleep(0.3)
        self._drain()
        # Select the EFM32LG target type so later `TARGET EFM` ops have context.
        self.converse("TARGET EFM32")

    # -- low level ---------------------------------------------------------
    def _drain(self):
        try:
            n = self.ser.in_waiting
            if n:
                self.ser.read(n)
        except OSError:
            pass

    def converse(self, cmd, quiet_s=0.15, overall_s=4.0):
        """Send one CLI command, return the response as decoded text.

        Reads until the link goes quiet for `quiet_s` or `overall_s` elapses.
        """
        self._drain()
        self.ser.write((cmd + "\r\n").encode())
        if self.verbose:
            print(f">> {cmd}", file=sys.stderr)

        buf = bytearray()
        start = time.time()
        last = start
        while True:
            chunk = self.ser.read(4096)
            now = time.time()
            if chunk:
                buf += chunk
                last = now
            else:
                if now - last >= quiet_s:
                    break
            if now - start >= overall_s:
                break

        text = buf.decode("utf-8", errors="replace")
        if text.startswith(cmd):
            text = text[len(cmd):]
        if self.verbose:
            print(text, file=sys.stderr)
        return text

    # -- high level --------------------------------------------------------
    def status(self):
        """Return (dpidr:int|None, unlocked:bool)."""
        text = self.converse("TARGET EFM STATUS")
        dpidr = _grep_hex(text, "DPIDR=0x")
        unlocked = "UNLOCKED" in text and "LOCKED" not in text.replace("UNLOCKED", "")
        return dpidr, unlocked

    def info(self):
        """Return a dict: dpidr, locked, part, flash_kb, sram_kb, uid, raw."""
        text = self.converse("TARGET EFM INFO")
        d = {"raw": text, "connected": "NOT CONNECTED" not in text}
        d["dpidr"] = _grep_hex(text, "DPIDR:")
        d["locked"] = "LOCKED (DLW" in text or ("LOCKED" in text and "UNLOCKED" not in text)
        d["part"] = _grep_hex(text, "DI.PART:")
        d["flash_kb"] = _grep_int(text, "Flash:")
        d["sram_kb"] = _grep_int(text, "SRAM:")
        d["uid"] = _grep_hex(text, "Unique ID:")
        return d

    def aap_probe(self):
        """Scan AP IDRs for the AAP. Returns the raw transcript."""
        return self.converse("TARGET EFM AAP")

    def device_erase(self):
        """AAP DEVICEERASE recovery (DESTRUCTIVE). Returns True if unlocked after."""
        text = self.converse("TARGET EFM ERASE", overall_s=8.0)
        return "SUCCEEDED" in text

    def dump(self, addr, length, chunk=256, progress=None):
        """Read `length` bytes over the AHB-AP. Requires an unlocked part.
        Chunked so each chunk re-enters SWD cleanly; addresses are absolute."""
        out = bytearray()
        got = 0
        while got < length:
            n = min(chunk, length - got)
            a = addr + got
            text = self.converse(f"TARGET EFM DUMP 0x{a:08X} {n}",
                                 overall_s=max(4.0, n * 0.03))
            if "locked" in text.lower() and "cannot read" in text.lower():
                raise EfmError(f"part is LOCKED - glitch first.\n--- raw ---\n{text}")
            rows = _parse_hexdump(text)
            if not rows:
                raise EfmError(f"no hex rows parsed at 0x{a:08X}\n--- raw ---\n{text}")
            for _row_addr, row_bytes in rows:
                out += row_bytes
            got = len(out)
            if progress:
                progress(min(got, length), length)
        return bytes(out[:length])

    def shot(self, delay_us, width_cyc):
        """Single crowbar pulse, power-cycle entry (no SWD probe). GP22 marks it."""
        return self.converse(f"TARGET EFM SHOT {delay_us} {width_cyc}",
                             overall_s=3.0 + delay_us / 1e6)

    def shot_reset(self, delay_us, width_cyc, hold_us):
        """Single crowbar pulse, nRST-reboot entry (no SWD probe). GP15 = scope trig."""
        return self.converse(f"TARGET EFM SHOTRST {delay_us} {width_cyc} {hold_us}",
                             overall_s=3.0 + delay_us / 1e6)

    def glitch_unlock(self, d_start, d_end, d_step, w_start, w_end, w_step,
                      off_ms, settle_ms, max_attempts, reset=False,
                      overall_s=None, on_line=None):
        """Run the firmware debug-unlock sweep (power-cycle or nRST variant).

        Returns the full transcript. A hit prints a line containing
        '*** UNLOCKED'. `on_line`, if given, is called with each decoded line.
        For the reset variant `off_ms` is reused as the nRST hold time (us).
        """
        # max_attempts=0 means "one full grid pass". The firmware only auto-computes
        # that when the tries arg is OMITTED; since we always send it, compute the
        # grid size here so a 0 doesn't run an empty sweep.
        if not max_attempts:
            ds = d_step or 1
            ws = w_step or 1
            n_delays = ((d_end - d_start) // ds + 1) if d_end >= d_start else 1
            n_widths = ((w_end - w_start) // ws + 1) if w_end >= w_start else 1
            max_attempts = n_delays * n_widths

        verb = "DEBUGUNLOCKRST" if reset else "DEBUGUNLOCK"
        cmd = (f"TARGET GLITCH {verb} "
               f"{d_start} {d_end} {d_step} {w_start} {w_end} {w_step} "
               f"{off_ms} {settle_ms} {max_attempts}")
        self._drain()
        self.ser.write((cmd + "\r\n").encode())
        if self.verbose:
            print(f">> {cmd}", file=sys.stderr)

        buf = bytearray()
        line = bytearray()
        start = time.time()
        while True:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
                for byte in chunk:
                    if byte == 0x0A:  # newline
                        s = line.decode("utf-8", errors="replace").rstrip("\r")
                        if on_line and s:
                            on_line(s)
                        line = bytearray()
                    else:
                        line.append(byte)
            text = buf.decode("utf-8", errors="replace")
            if "*** UNLOCKED" in text or "No unlock" in text or "*** ABORT" in text:
                break
            if overall_s is not None and time.time() - start >= overall_s:
                break
        return buf.decode("utf-8", errors="replace")

    def close(self):
        try:
            self.ser.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


# -- parsing helpers -------------------------------------------------------
def _grep_hex(text, marker):
    """Find `marker` then read the 0x.... hex value that follows it."""
    j = text.find(marker)
    if j == -1:
        return None
    rest = text[j + len(marker):].lstrip()
    if rest[:2] in ("0x", "0X"):
        rest = rest[2:]
    h = ""
    for c in rest:
        if c in "0123456789abcdefABCDEF":
            h += c
        else:
            break
    return int(h, 16) if h else None


def _grep_int(text, marker):
    """Find `marker` then read the decimal integer that follows it."""
    j = text.find(marker)
    if j == -1:
        return None
    rest = text[j + len(marker):].lstrip()
    n = ""
    for c in rest:
        if c.isdigit():
            n += c
        else:
            break
    return int(n) if n else None


def _parse_hexdump(text):
    """Parse firmware dump rows '00ABCDEF: 12 34 56 78' -> [(addr, bytes), ...]."""
    rows = []
    for raw in text.splitlines():
        s = raw.strip().lstrip(".+->").strip()
        if ":" not in s:
            continue
        addr_part, _, data_part = s.partition(":")
        addr_part = addr_part.strip()
        # 8-hex-digit address prefix (firmware prints "%08X:")
        if len(addr_part) < 8 or any(c not in "0123456789abcdefABCDEF" for c in addr_part):
            continue
        try:
            row_addr = int(addr_part, 16)
            row_bytes = bytes(int(b, 16) for b in data_part.split())
        except ValueError:
            continue
        if row_bytes:
            rows.append((row_addr, row_bytes))
    return rows


def add_common_args(parser):
    """Shared CLI args for the efm32_* tools."""
    parser.add_argument("-p", "--port", default=DEFAULT_PORT,
                        help=f"serial port (default {DEFAULT_PORT}, or $EFM_PORT)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="echo the raw CLI traffic to stderr")
    return parser


if __name__ == "__main__":
    print(__doc__)
    print("This is a library. Use efm32_glitch.py.")
