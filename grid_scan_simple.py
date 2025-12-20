#!/usr/bin/env python3
"""
Simple grid scanning script for Grbl XY platform
Scans a 30x30mm grid at 1mm intervals, continuously looping
"""

import serial
import time
import sys

def main():
    print("=== Grbl Grid Scanner ===", flush=True)
    print("Scanning 30x30mm grid at 1mm intervals", flush=True)
    print("Press Ctrl+C to stop\n", flush=True)

    # Open serial connection
    ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
    time.sleep(1)  # Let serial port stabilize

    cycle_count = 0

    try:
        while True:
            cycle_count += 1
            print(f"--- Starting cycle {cycle_count} ---", flush=True)

            # Scan through the grid
            point_count = 0
            total_points = 31 * 31  # 0 to 30 inclusive

            for x in range(0, 31):  # 0 to 30mm
                for y in range(0, 31):  # 0 to 30mm
                    point_count += 1

                    # Send move command
                    cmd = f"grbl move {x} {y}\r\n"
                    if point_count <= 5 or point_count % 50 == 0:
                        print(f"  Sending: X={x} Y={y} (point {point_count})", flush=True)
                    ser.write(cmd.encode())

                    # Progress indicator every 100 points
                    if point_count % 100 == 0:
                        print(f"  Progress: {point_count}/{total_points} points "
                              f"({100*point_count//total_points}%) - "
                              f"Current: X={x}mm Y={y}mm", flush=True)

                    # Wait 1 second at each position
                    time.sleep(1.0)

            print(f"  Completed {total_points} points", flush=True)

            # Return to home (0,0)
            print("  Returning to home (0,0)...", flush=True)
            ser.write(b"grbl move 0 0\r\n")
            time.sleep(2)

            print(f"--- Cycle {cycle_count} complete ---\n", flush=True)

    except KeyboardInterrupt:
        print("\n\n=== Stopping grid scan ===", flush=True)
        print("Returning to home...", flush=True)
        ser.write(b"grbl move 0 0\r\n")
        time.sleep(2)
        print("Done.", flush=True)

    finally:
        ser.close()

if __name__ == "__main__":
    main()
