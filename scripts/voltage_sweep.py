#!/usr/bin/env python3
"""
Quick voltage sweep to find crash threshold.
Tests multiple voltages with small iteration count.
"""

import serial
import time
import sys

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200
TIMEOUT = 2.0

def send_command(ser, cmd, wait_time=0.2):
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)
    ser.write(f"{cmd}\r\n".encode())
    time.sleep(wait_time)
    response = ""
    if ser.in_waiting > 0:
        response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
    return response


def wait_for_response(ser, expected_text, timeout=5.0):
    start_time = time.time()
    full_response = ""
    while time.time() - start_time < timeout:
        if ser.in_waiting > 0:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            full_response += chunk
            if expected_text in full_response:
                return True, full_response
        time.sleep(0.01)
    return False, full_response


def setup_system(ser):
    """Initial setup."""
    # Enable API mode
    ser.write(b"API ON\r\n")
    time.sleep(0.2)
    ser.read(ser.in_waiting)

    send_command(ser, "TARGET LPC", wait_time=0.2)
    send_command(ser, "CS TRIGGER HARDWARE HIGH", wait_time=0.5)
    send_command(ser, "CS ARM", wait_time=1.0)


def test_single_glitch(ser, voltage, pause, width):
    """Test single glitch parameters."""
    # Update ChipSHOUTER voltage
    send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5)

    # Update Pico pause (glitch delay)
    send_command(ser, f"SET PAUSE {pause}", wait_time=0.2)

    # Update Pico width (glitch duration)
    send_command(ser, f"SET WIDTH {width}", wait_time=0.2)

    # Sync with bootloader (with 2 retries)
    ser.write(b"TARGET SYNC 115200 12000 10 2\r\n")
    success, _ = wait_for_response(ser, "Synchronized", timeout=5.0)
    if not success:
        return "SYNC_FAIL"

    # Arm Pico trigger
    send_command(ser, "ARM ON", wait_time=0.2)

    # Send read command to target
    ser.write(b'TARGET SEND "R 0 516096"\r\n')

    # Wait for response
    response = ""
    start_time = time.time()
    max_wait = 3.0

    while time.time() - start_time < max_wait:
        time.sleep(0.1)
        if ser.in_waiting > 0:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            response += chunk
            if "> " in chunk or "ARM ON" in chunk:
                break

    # Parse result - check for UUE data (SUCCESS)
    if any(line.strip().startswith('M') for line in response.split('\n')):
        return "SUCCESS"

    # Check for error code 19
    lines = response.split('\n')
    for line in lines:
        if line.strip() == "19":
            return "ERROR19"

    # Check for no response
    if "No response data" in response or not response.strip():
        return "NO_RESPONSE"

    return "UNKNOWN"


def main():
    pause = 0
    width = 150
    iterations = 20  # Quick test per voltage

    print("=" * 70)
    print("VOLTAGE SWEEP TO FIND CRASH THRESHOLD")
    print("=" * 70)
    print(f"Parameters: P={pause}, W={width}")
    print(f"Iterations per voltage: {iterations}")
    print("=" * 70)
    print()

    # Connect
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
    time.sleep(0.5)

    try:
        setup_system(ser)

        # Test voltages from 400V to 500V in 10V steps
        for voltage in range(400, 510, 10):
            print(f"\nTesting V={voltage}...", end=" ", flush=True)

            success_count = 0
            crash_count = 0
            error19_count = 0
            sync_fail_count = 0

            for i in range(iterations):
                result = test_single_glitch(ser, voltage, pause, width)

                if result == "SUCCESS":
                    success_count += 1
                elif result == "NO_RESPONSE":
                    crash_count += 1
                elif result == "ERROR19":
                    error19_count += 1
                elif result == "SYNC_FAIL":
                    sync_fail_count += 1

            success_rate = success_count / iterations * 100
            crash_rate = crash_count / iterations * 100

            print(f"Success: {success_count:2d}/{iterations} ({success_rate:5.1f}%) | "
                  f"Crash: {crash_count:2d} ({crash_rate:5.1f}%) | "
                  f"Error19: {error19_count:2d} | Sync_fail: {sync_fail_count:2d}")

            # If we found crashes, we can stop or continue
            if crash_count > 0:
                print(f"\n✓ Found crash threshold around V={voltage}")

            # Stop if we reach very high crash rate
            if crash_rate > 80:
                print(f"\nStopping - crash rate too high (>80%)")
                break

    finally:
        # Cleanup
        try:
            ser.write(b"API OFF\r\n")
            time.sleep(0.1)
        except:
            pass
        ser.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
