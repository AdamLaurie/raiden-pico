#!/usr/bin/env python3
"""
PIO uart_rx_decoder torture test.

For each test case:
  1. TRIGGER UART <byte>
  2. ARM ON
  3. send the test stream out the FTDI
  4. STAT  ->  if "Armed: NO" and "Glitch Count" went up, the SM matched.
              if "Armed: YES"           the SM missed.

Wiring:
  FTDI TX  -> Pico GP5  (target UART RX, default PIO trigger pin)
  FTDI GND -> Pico GND
  (FTDI 3.3V logic only - RP2350 IO is not 5V tolerant)

Usage:
  python3 pio_trigger_torture.py
  python3 pio_trigger_torture.py --iters 200
  python3 pio_trigger_torture.py --only "lpc,multi-match"
"""

import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pip install pyserial")


# --------------------------------------------------------------- Pico CLI I/O
# We enable API mode on the Pico so every command is bracketed by:
#   "."  (acknowledge)
#   ...response body...
#   "+"  (success) or "!" (failure)
# That gives us a clean terminator for the response read.
class Pico:
    def __init__(self, port, timeout=2.0):
        self.s = serial.Serial(port, 115200, timeout=timeout)
        time.sleep(0.3)
        self.s.reset_input_buffer()
        # Try to enable API mode in case it isn't already on
        self.s.write(b"API ON\r\n")
        time.sleep(0.4)
        self.s.reset_input_buffer()

    def cmd(self, line, tmo=3.0):
        """Send a command in API mode, return raw response (excluding markers)."""
        self.s.reset_input_buffer()
        self.s.write((line + "\r\n").encode())
        buf = b""
        end = time.time() + tmo
        status = None  # '+' or '!'
        while time.time() < end:
            chunk = self.s.read(self.s.in_waiting or 1)
            if chunk:
                buf += chunk
                # Strip the leading '.' ack so it doesn't trip the success check
                # and look for + / ! anywhere after the ack.
                idx = buf.find(b".")
                if idx >= 0:
                    tail = buf[idx + 1:]
                    if b"+" in tail or b"!" in tail:
                        m = re.search(rb"[+!]", tail)
                        status = chr(tail[m.start()])
                        body = tail[: m.start()]
                        return body.decode(errors="replace"), status
            else:
                time.sleep(0.01)
        raise TimeoutError(f"no API terminator for {line!r}; got: {buf!r}")

    def stat(self):
        """Return (armed_bool, glitch_count_int) parsed from STAT."""
        body, st = self.cmd("STAT")
        if st != "+":
            raise RuntimeError(f"STAT failed: {body!r}")
        m_armed = re.search(r"Armed:\s*(YES|NO)", body)
        m_count = re.search(r"Glitch Count:\s*(\d+)", body)
        if not (m_armed and m_count):
            raise RuntimeError(f"could not parse STAT body:\n{body}")
        return (m_armed.group(1) == "YES", int(m_count.group(1)))

    def reset(self):
        self.cmd("RESET")

    def close(self):
        try:
            self.cmd("ARM OFF", tmo=1.0)
        except Exception:
            pass
        self.s.close()


# --------------------------------------------------------------- one trial
def trial(pico, ftdi, target_byte, stream, settle_ms):
    pico.cmd(f"TRIGGER UART 0x{target_byte:02X}")
    pico.cmd("SET PAUSE 0")
    pico.cmd("SET WIDTH 100")
    pico.cmd("SET GAP 100")
    pico.cmd("SET COUNT 1")

    pre_armed, pre_count = pico.stat()
    if pre_armed:
        # Stuck armed from a previous test - disarm and retry
        pico.cmd("ARM OFF")
        pre_armed, pre_count = pico.stat()

    body, st = pico.cmd("ARM ON")
    if st != "+":
        return {"fired": False, "delta": 0, "stuck_armed": True, "still_armed": False, "error": body.strip()}

    if stream:
        ftdi.write(stream)
        ftdi.flush()
    # Let the UART stream complete + the main loop run glitch_get_count
    # 1 byte at 115200 ~= 87us; add settle_ms slack.
    drain_s = len(stream) * (10.0 / 115200.0) + settle_ms / 1000.0
    time.sleep(drain_s)

    post_armed, post_count = pico.stat()
    pico.cmd("ARM OFF")
    return {
        "fired": post_count > pre_count,
        "delta": post_count - pre_count,
        "still_armed": post_armed,
        "stuck_armed": False,
        "error": None,
    }


# --------------------------------------------------------------- test suite
def suite():
    cr = 0x0D
    # LPC echo+response stream where we first observed the bug
    lpc_like = bytes([0x52, 0x20, 0x30, 0x20, 0x34, 0x0D, 0x31, 0x39, 0x0D, 0x0A])

    return [
        # (name, target_byte, lambda i: stream, tags, must_fire)
        ("single-CR",                cr,   lambda i: bytes([cr]),                            "basic,single",                   True),
        ("two-CR back-to-back",      cr,   lambda i: bytes([cr, cr]),                        "basic,repeat,multi-match",       True),
        ("five-CR",                  cr,   lambda i: bytes([cr]*5),                          "repeat,multi-match",             True),
        ("64-CR",                    cr,   lambda i: bytes([cr]*64),                         "repeat,multi-match,burst",       True),
        ("LPC echo+response",        cr,   lambda i: lpc_like,                               "lpc,multi-match",                True),
        ("LPC echo only",            cr,   lambda i: lpc_like[:6],                           "lpc,single",                     True),

        ("single-0x55",              0x55, lambda i: bytes([0x55]),                          "basic,nibble",                   True),
        ("two-0x55",                 0x55, lambda i: bytes([0x55]*2),                        "repeat,nibble,multi-match",      True),
        ("five-0x55",                0x55, lambda i: bytes([0x55]*5),                        "repeat,nibble,multi-match",      True),
        ("single-0xAA",              0xAA, lambda i: bytes([0xAA]),                          "basic,nibble",                   True),
        ("two-0xAA",                 0xAA, lambda i: bytes([0xAA]*2),                        "repeat,nibble,multi-match",      True),
        ("five-0xAA",                0xAA, lambda i: bytes([0xAA]*5),                        "repeat,nibble,multi-match",      True),
        ("two-0xFF",                 0xFF, lambda i: bytes([0xFF]*2),                        "repeat,nibble,multi-match",      True),
        ("two-0x00",                 0x00, lambda i: bytes([0x00]*2),                        "repeat,nibble,multi-match",      True),

        ("two-0xD0",                 0xD0, lambda i: bytes([0xD0]*2),                        "nibble,multi-match",             True),
        ("two-0xDD",                 0xDD, lambda i: bytes([0xDD]*2),                        "nibble,multi-match",             True),
        ("two-0x0E",                 0x0E, lambda i: bytes([0x0E]*2),                        "nibble,multi-match",             True),

        ("target-amongst-noise",     cr,   lambda i: bytes([0x55, 0xAA, cr, 0x33, cr]),       "multi-match,noise",              True),
        ("target-after-prefix",      cr,   lambda i: bytes([0x52, 0x20, 0x30, 0x20, 0x34, cr]),"multi-match,lpc,single",        True),
        ("target-only-at-end",       cr,   lambda i: b"some prefix bytes\r",                  "single",                         True),

        ("CR-X-CR (X=0x55)",         cr,   lambda i: bytes([cr, 0x55, cr]),                  "repeat,multi-match",             True),
        ("CR-X-CR (X=0x0E)",         cr,   lambda i: bytes([cr, 0x0E, cr]),                  "repeat,multi-match,nibble",      True),

        # Sanity: must NOT fire
        ("no-match (all 0x55)",      cr,   lambda i: bytes([0x55]*10),                       "negative",                       False),
        ("no-match (empty)",         cr,   lambda i: b"",                                    "negative",                       False),
    ]


def main():
    ap = argparse.ArgumentParser(description="PIO uart_rx_decoder torture test")
    ap.add_argument("--pico", default="/dev/ttyACM0")
    ap.add_argument("--ftdi", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--iters", type=int, default=50, help="iterations per test (default 50)")
    ap.add_argument("--settle-ms", type=int, default=30, help="settle slack after each stream (ms)")
    ap.add_argument("--only", default="", help="comma-separated tags to filter tests")
    args = ap.parse_args()

    print(f"Pico CLI : {args.pico}")
    print(f"FTDI TX  : {args.ftdi}  @ {args.baud}")
    print(f"Iters    : {args.iters}")
    print()

    pico = Pico(args.pico)
    pico.reset()  # zero glitch count + clear config
    ftdi = serial.Serial(args.ftdi, args.baud, timeout=1.0)
    time.sleep(0.2)
    ftdi.reset_output_buffer()
    ftdi.reset_input_buffer()

    filter_tags = [t.strip() for t in args.only.split(",") if t.strip()]
    cases = suite()

    failures = []
    print("==== PIO trigger torture suite ====")
    for name, target_byte, build, tags, must_fire in cases:
        if filter_tags and not any(t in tags for t in filter_tags):
            continue
        fired = 0
        still_armed = 0
        stuck = 0
        errors = 0
        for i in range(args.iters):
            try:
                r = trial(pico, ftdi, target_byte, build(i), args.settle_ms)
            except Exception as e:
                errors += 1
                continue
            if r["error"]:
                errors += 1
                continue
            if r["stuck_armed"]:
                stuck += 1
                continue
            if r["fired"]:
                fired += 1
            if r["still_armed"]:
                still_armed += 1

        n = args.iters - errors - stuck
        rate = (fired / n * 100.0) if n else 0.0
        if must_fire:
            ok = rate >= 99.0
        else:
            ok = fired == 0
        flag = "OK   " if ok else "FAIL "
        extra = []
        if errors: extra.append(f"errors={errors}")
        if stuck:  extra.append(f"stuck={stuck}")
        extra_str = (" [" + " ".join(extra) + "]") if extra else ""
        print(f"  [{flag}] {name:<28s} trig=0x{target_byte:02X}  "
              f"fired {fired}/{n}  ({rate:5.1f}%)"
              f"{'' if must_fire else '  (must NOT fire)'}{extra_str}")
        if not ok:
            failures.append((name, f"fired {fired}/{n} ({rate:.1f}%); expected {'>=99%' if must_fire else '0%'}"))

    print()
    if failures:
        print("==== FAILURES ====")
        for n, why in failures:
            print(f"  - {n}: {why}")
    else:
        print("All tests passed.")

    pico.close()
    ftdi.close()


if __name__ == "__main__":
    main()
