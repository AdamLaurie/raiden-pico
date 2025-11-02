#!/usr/bin/env python3
"""Test GPIO trigger functionality on GP3"""

import serial
import time
import sys

def send_cmd(ser, cmd, wait=0.1):
    """Send command and wait for response"""
    ser.write(f'{cmd}\r\n'.encode())
    time.sleep(wait)
    response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
    return response

def get_status(ser):
    """Get armed status and glitch count"""
    resp = send_cmd(ser, 'STATUS', wait=0.2)
    armed = 'YES' in resp and 'Armed:' in resp

    # Extract glitch count
    count = 0
    for line in resp.split('\n'):
        if 'Glitch Count:' in line:
            try:
                count = int(line.split(':')[1].strip())
            except:
                pass

    return armed, count, resp

def main():
    port = '/dev/ttyACM0'

    print("=" * 60)
    print("GPIO TRIGGER TEST - GP3 Rising Edge")
    print("=" * 60)
    print(f"Port: {port}")
    print()

    try:
        ser = serial.Serial(port, 115200, timeout=1)
        time.sleep(0.5)

        # Clear any boot messages
        ser.read(ser.in_waiting)

        print("Step 1: Configure glitch parameters")
        print("-" * 60)
        send_cmd(ser, 'SET WIDTH 150')
        send_cmd(ser, 'SET PAUSE 0')
        send_cmd(ser, 'SET COUNT 1')
        print("  Width: 150 cycles")
        print("  Pause: 0 cycles")
        print("  Count: 1 pulse")
        print()

        print("Step 2: Configure GPIO trigger on GP3, RISING edge")
        print("-" * 60)
        resp = send_cmd(ser, 'TRIGGER GPIO RISING', wait=0.2)
        print(f"  Response: {resp.strip()}")
        print()

        print("Step 3: Disarm (ensure clean state)")
        print("-" * 60)
        send_cmd(ser, 'ARM OFF')
        time.sleep(0.1)
        print()

        print("Step 4: Arm the glitch")
        print("-" * 60)
        resp = send_cmd(ser, 'ARM ON', wait=0.2)
        print(f"  Response: {resp.strip()}")
        print()

        print("Step 5: Check status BEFORE trigger")
        print("-" * 60)
        armed_before, count_before, status_before = get_status(ser)
        print(f"  Armed: {armed_before}")
        print(f"  Count: {count_before}")
        print()

        if not armed_before:
            print("ERROR: System failed to arm!")
            print(status_before)
            ser.close()
            return 1

        print("Step 6: Execute TARGET RESET (triggers GP15 -> GP3)")
        print("-" * 60)
        resp = send_cmd(ser, 'TARGET RESET', wait=0.5)
        print(f"  Response: {resp.strip()}")
        print()

        print("Step 7: Check status AFTER trigger")
        print("-" * 60)
        armed_after, count_after, status_after = get_status(ser)
        print(f"  Armed: {armed_after}")
        print(f"  Count: {count_after}")
        print()

        print("=" * 60)
        print("TEST RESULTS")
        print("=" * 60)

        success = True

        # Check if armed status changed
        if armed_after:
            print("✗ FAIL: System still armed after trigger")
            success = False
        else:
            print("✓ PASS: System disarmed after trigger")

        # Check if glitch count incremented
        if count_after == count_before + 1:
            print("✓ PASS: Glitch count incremented")
        else:
            print(f"✗ FAIL: Glitch count did not increment ({count_before} -> {count_after})")
            success = False

        print()

        if success:
            print("✓✓✓ GPIO TRIGGER WORKING! ✓✓✓")
            ser.close()
            return 0
        else:
            print("✗✗✗ GPIO TRIGGER FAILED ✗✗✗")
            print()
            print("Debug info:")
            print(status_after)
            ser.close()
            return 1

    except Exception as e:
        print(f"ERROR: {e}")
        import traceback
        traceback.print_exc()
        return 1

if __name__ == '__main__':
    sys.exit(main())
