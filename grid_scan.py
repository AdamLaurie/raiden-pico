#!/usr/bin/env python3
"""
Grid scanning script for Grbl XY platform
Scans a 30x30mm grid at 1mm intervals, continuously looping
"""

import serial
import time
import sys

def send_command(ser, cmd):
    """Send command and wait for it to process"""
    ser.write(f"{cmd}\r\n".encode())
    time.sleep(0.2)  # Give the command time to process
    # Drain any response
    if ser.in_waiting:
        ser.read(ser.in_waiting)

def main():
    print("=== Grbl Grid Scanner ===")
    print("Scanning 30x30mm grid at 1mm intervals")
    print("Press Ctrl+C to stop\n")

    # Open serial connection
    ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)
    time.sleep(0.5)

    cycle_count = 0

    try:
        while True:
            cycle_count += 1
            print(f"\n--- Starting cycle {cycle_count} ---")

            # Scan through the grid
            point_count = 0
            total_points = 31 * 31  # 0 to 30 inclusive

            for x in range(0, 31):  # 0 to 30mm
                for y in range(0, 31):  # 0 to 30mm
                    point_count += 1

                    # Move to position
                    cmd = f"grbl move {x} {y}"
                    send_command(ser, cmd)

                    # Progress indicator every 100 points
                    if point_count % 100 == 0:
                        print(f"  Progress: {point_count}/{total_points} points "
                              f"({100*point_count//total_points}%) - "
                              f"Current: X={x}mm Y={y}mm")

                    # Wait 1 second at each position
                    time.sleep(1.0)

            print(f"  Completed {total_points} points")

            # Return to home (0,0)
            print("  Returning to home (0,0)...")
            send_command(ser, "grbl move 0 0")
            time.sleep(2)

            print(f"--- Cycle {cycle_count} complete ---")

    except KeyboardInterrupt:
        print("\n\n=== Stopping grid scan ===")
        print("Returning to home...")
        send_command(ser, "grbl move 0 0")
        time.sleep(2)
        print("Done.")

    finally:
        ser.close()

if __name__ == "__main__":
    main()
