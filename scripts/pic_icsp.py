#!/usr/bin/env python3
"""
pic_icsp.py - Host-side library for talking to a PIC18 over ICSP via Raiden-Pico.

The Pico2 (RP2350) firmware already implements bit-banged PIC18 ICSP
(src/pic18_target.c). This module is the host counterpart: it opens the Pico's
USB-CDC CLI and drives the `TARGET PIC ...` command surface, parsing the
human-readable responses back into Python values.

Firmware command surface (see src/command_parser.c around "PIC"):
    TARGET PIC18                       - select PIC18 target type
    TARGET PIC INFO                    - DEVID + CONFIG5L + decoded CP state
    TARGET PIC STATUS                  - one-line DEVID + PROTECTED/OPEN
    TARGET PIC DUMP [addr] [bytes]     - hex-dump program memory (0x00 on CP'd blocks)
    TARGET PIC SHOT [delay_us] [width] - one crowbar pulse (scope bring-up, GP22 marker)
    TARGET PIC GLITCH [probe d_start d_end d_step w_start w_end w_step max]
                                       - CP-bypass delay x width sweep

Pin map (firmware, fixed): PGC=GP17  PGD=GP18  MCLR=GP15  PGM=GP19  crowbar->VDD=GP2
                           scope trigger (GLITCH_FIRED)=GP22  target power=GP10/11/12
LVP entry flavours: 'key' (FX220/X320, default) or 'pgm' (PIC18F4321 family, RB5/PGM=GP19).
See PIC18_ICSP.md for the wiring schematic.

Nothing here powers or glitches the target on import; every action is an explicit
method call. Default port /dev/ttyACM0 (override with --port / PIC_PORT env).
"""

import os
import sys
import time
import serial   # pyserial


DEFAULT_PORT = os.environ.get("PIC_PORT", "/dev/ttyACM0")
DEFAULT_BAUD = 115200


def project_dir(name="pic18"):
    """Per-target output folder `scripts/<name>/` for dumps/logs, created on demand
    so the scripts/ dir stays clean. One folder per target chip."""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    os.makedirs(d, exist_ok=True)
    return d


def project_path(filename, name="pic18"):
    """Full path to `filename` inside the per-target output folder (see project_dir)."""
    return os.path.join(project_dir(name), filename)


# DEVID -> part name. DEVID2:DEVID1; the low 5 bits of DEVID1 are the silicon
# revision (REV4:REV0), so mask with 0xFFE0 before lookup. These are seeds for
# the families staged in WORK/ - ALWAYS confirm against the device's own
# programming spec DEVID table before trusting a name (FIND-108: the F4321
# family is different silicon from the FX320 family).
KNOWN_DEVID = {
    # 0xXXXX (rev masked off): "part"   # <-- verify against datasheet DEVID table
    # Seed table intentionally small; extend as parts are confirmed on the bench.
}


class PicError(RuntimeError):
    pass


class PicLink:
    """Thin wrapper around the Pico CLI for PIC18 ICSP operations."""

    def __init__(self, port=DEFAULT_PORT, baud=DEFAULT_BAUD, verbose=False, lvp="key"):
        self.verbose = verbose
        self.ser = serial.Serial(port, baud, timeout=0.2)
        time.sleep(0.3)
        self._drain()
        # Select the PIC18 target type so later `TARGET PIC` ops have context.
        self.converse("TARGET PIC18")
        # Pick the LVP entry method: "key" = FX220/X320 MCHP-key (firmware default),
        # "pgm" = PIC18F4321 family (RB5/PGM held high, GP19). Wrong mode => the
        # part won't connect (FIND-108: F4321 is different silicon from FX320).
        self.set_lvp(lvp)

    def set_lvp(self, mode):
        """Select LVP entry: 'key' (FX320) or 'pgm' (F4321 family on GP19/PGM)."""
        mode = mode.lower()
        if mode not in ("key", "pgm"):
            raise PicError("lvp mode must be 'key' or 'pgm'")
        self.converse("TARGET PIC LVP " + mode.upper())

    # -- low level ---------------------------------------------------------
    def _drain(self):
        try:
            n = self.ser.in_waiting
            if n:
                self.ser.read(n)
        except OSError:
            pass

    def converse(self, cmd, quiet_s=0.15, overall_s=3.0):
        """Send one CLI command, return the response as decoded text.

        Reads until the link goes quiet for `quiet_s` (the firmware streams
        line by line) or `overall_s` elapses - whichever first. Works whether
        or not API mode is on; we just strip the framing characters.
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
        # Strip the command echo if the firmware echoed it back.
        if text.startswith(cmd):
            text = text[len(cmd):]
        if self.verbose:
            print(text, file=sys.stderr)
        return text

    # -- high level --------------------------------------------------------
    def status(self):
        """Return (devid:int, protected:bool) or (None, None) if no link."""
        text = self.converse("TARGET PIC STATUS")
        if "NOT CONNECTED" in text:
            return None, None
        devid = _grep_hex(text, "DEVID=0x")
        protected = "PROTECTED" in text
        return devid, protected

    def info(self):
        """Return a dict: devid, config5l, cp{0..3} (True=protected), protected, raw."""
        text = self.converse("TARGET PIC INFO")
        if "NOT CONNECTED" in text:
            return {"devid": None, "connected": False, "raw": text}
        devid = _grep_hex(text, "DEVID:")
        cfg5l = _grep_hex(text, "CONFIG5L:")
        cp = {}
        # Line: "Code protect: CP0=P CP1=- CP2=- CP3=-"  (P=protected, -=open)
        for i in range(4):
            tok = f"CP{i}="
            j = text.find(tok)
            cp[i] = (text[j + len(tok)] == "P") if j != -1 else None
        return {
            "devid": devid,
            "config5l": cfg5l,
            "revision": (devid & 0x1F) if devid is not None else None,
            "cp": cp,
            "protected": any(v for v in cp.values() if v),
            "connected": True,
            "raw": text,
        }

    def dump(self, addr, length, chunk=256, progress=None):
        """Read `length` bytes of program memory starting at `addr`.

        Returns a bytes object. Protected blocks read back as 0x00 (until a
        glitch lands). Chunked so each chunk re-enters ICSP cleanly; addresses
        are absolute so chunking is safe.
        """
        out = bytearray()
        got = 0
        while got < length:
            n = min(chunk, length - got)
            a = addr + got
            text = self.converse(f"TARGET PIC DUMP 0x{a:06X} {n}",
                                  overall_s=max(3.0, n * 0.03))
            rows = _parse_hexdump(text)
            if not rows:
                raise PicError(f"no hex rows parsed at 0x{a:06X}\n--- raw ---\n{text}")
            for _row_addr, row_bytes in rows:
                out += row_bytes
            got = len(out)
            if progress:
                progress(min(got, length), length)
        return bytes(out[:length])

    def shot(self, delay_us, width_cyc):
        """Single crowbar pulse for scope bring-up (no ICSP probe). GP22 marks it."""
        return self.converse(f"TARGET PIC SHOT {delay_us} {width_cyc}",
                             overall_s=2.0 + delay_us / 1e6)

    def glitch_cp(self, probe_addr, d_start, d_end, d_step,
                  w_start, w_end, w_step, max_attempts,
                  overall_s=None, on_line=None):
        """Run the firmware CP-bypass sweep. Returns the full transcript text.

        The sweep runs entirely on the Pico; this call blocks streaming its
        output. A hit prints a line containing '*** CP BYPASS'. `on_line`, if
        given, is called with each decoded line as it arrives (live monitor).
        """
        cmd = (f"TARGET PIC GLITCH 0x{probe_addr:06X} "
               f"{d_start} {d_end} {d_step} {w_start} {w_end} {w_step} {max_attempts}")
        self._drain()
        self.ser.write((cmd + "\r\n").encode())
        if self.verbose:
            print(f">> {cmd}", file=sys.stderr)

        buf = bytearray()
        line = bytearray()
        start = time.time()
        # No fixed quiet window: a long sweep is silent between the periodic
        # "attempt N/M" prints. Stop on the hit line, the exhausted line, or
        # the caller-supplied overall budget.
        while True:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
                for byte in chunk:
                    if byte in (0x0A,):  # newline
                        s = line.decode("utf-8", errors="replace").rstrip("\r")
                        if on_line and s:
                            on_line(s)
                        line = bytearray()
                    else:
                        line.append(byte)
            text = buf.decode("utf-8", errors="replace")
            if "*** CP BYPASS" in text or "exhausted attempts" in text:
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
    if rest.startswith("0x") or rest.startswith("0X"):
        rest = rest[2:]
    h = ""
    for c in rest:
        if c in "0123456789abcdefABCDEF":
            h += c
        else:
            break
    return int(h, 16) if h else None


def _parse_hexdump(text):
    """Parse firmware dump rows '0x00ABCD: 12 34 56 ...' -> [(addr, bytes), ...]."""
    rows = []
    for raw in text.splitlines():
        s = raw.strip()
        # tolerate API framing chars
        s = s.lstrip(".+->").strip()
        if ":" not in s or not (s.startswith("0x") or s.startswith("0X")):
            continue
        addr_part, _, data_part = s.partition(":")
        try:
            row_addr = int(addr_part, 16)
        except ValueError:
            continue
        try:
            row_bytes = bytes(int(b, 16) for b in data_part.split())
        except ValueError:
            continue
        if row_bytes:
            rows.append((row_addr, row_bytes))
    return rows


def part_name(devid):
    """Best-effort DEVID -> name (revision masked off). None if unknown."""
    if devid is None:
        return None
    return KNOWN_DEVID.get(devid & 0xFFE0)


def add_common_args(parser):
    """Shared CLI args for the pic_* tools."""
    parser.add_argument("-p", "--port", default=DEFAULT_PORT,
                        help=f"serial port (default {DEFAULT_PORT}, or $PIC_PORT)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="echo the raw CLI traffic to stderr")
    parser.add_argument("--lvp", choices=("key", "pgm"), default="key",
                        help="LVP entry: 'key' = FX220/X320 (default), "
                             "'pgm' = PIC18F4321 family (RB5/PGM on GP19)")
    return parser


if __name__ == "__main__":
    print(__doc__)
    print("This is a library. Use pic_info.py / pic_dump.py / pic_glitch.py.")
