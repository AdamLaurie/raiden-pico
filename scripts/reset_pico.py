#!/usr/bin/env python3
"""Reset the Pico 2 by pulsing FTDI DTR low against its EN pin.

Wiring:
    FTDI DTR  -> Pico 2 EN (RUN) pin
    FTDI GND  -> Pico 2 GND

Usage:
    python3 scripts/reset_pico.py                  # default: /dev/ttyUSB0, 100 ms pulse
    python3 scripts/reset_pico.py --port /dev/ttyUSB1 --ms 200
    python3 scripts/reset_pico.py --wait           # also wait for ttyACM0 to come back

The FTDI breakout's DTR signal goes electrically LOW when pyserial asserts
DTR (i.e. `ser.dtr = True`), which is what pulls EN low and resets the chip.
"""
import argparse
import os
import sys
import time

import serial


def reset(port: str, pulse_ms: int) -> None:
    s = serial.Serial(port, 9600, timeout=1)
    try:
        s.dtr = True   # DTR asserted -> EN low (reset)
        time.sleep(pulse_ms / 1000.0)
        s.dtr = False  # release -> EN high (boot)
    finally:
        s.close()


def wait_for_acm(path: str = "/dev/ttyACM0", timeout: float = 10.0) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(path):
            # Let CDC settle and confirm with VERSION
            time.sleep(0.5)
            try:
                acm = serial.Serial(path, 115200, timeout=2)
                time.sleep(0.3)
                acm.reset_input_buffer()
                acm.write(b"VERSION\r\n")
                time.sleep(0.3)
                r = acm.read(acm.in_waiting or 1).decode("utf-8", errors="replace")
                acm.close()
                if "Raiden" in r:
                    return True
            except serial.SerialException:
                pass
        time.sleep(0.2)
    return False


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--port", default="/dev/ttyUSB0", help="FTDI serial device")
    p.add_argument("--ms", type=int, default=100, help="DTR low pulse duration (ms)")
    p.add_argument("--wait", action="store_true",
                   help="Wait for /dev/ttyACM0 and confirm boot via VERSION")
    args = p.parse_args()

    if not os.path.exists(args.port):
        print(f"ERROR: {args.port} not found", file=sys.stderr)
        return 1

    print(f"Pulsing DTR low on {args.port} for {args.ms} ms...")
    reset(args.port, args.ms)

    if args.wait:
        print("Waiting for /dev/ttyACM0...")
        if wait_for_acm():
            print("OK: Pico 2 responded to VERSION")
            return 0
        print("ERROR: Pico 2 did not respond after reset", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
