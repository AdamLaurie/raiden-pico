#!/usr/bin/env python3
"""
Test LPC bootloader entry and UART sync via Pico2

Uses OpenOCD to reset the LPC and then tests bootloader UART sync
through the Pico2 UART bridge.
"""

import subprocess
import serial
import time
import sys

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200

def run_openocd_reset():
    """Reset LPC via OpenOCD"""
    print("Resetting LPC via OpenOCD + J-Link...")

    cmd = [
        "openocd",
        "-f", "/usr/share/openocd/scripts/interface/jlink.cfg",
        "-f", "/usr/share/openocd/scripts/target/lpc2478.cfg",
        "-c", "init",
        "-c", "reset run",  # Reset and let it run
        "-c", "exit"
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        if result.returncode == 0:
            print("✓ Reset successful")
            return True
        else:
            print(f"✗ Reset failed: {result.stderr}")
            return False
    except subprocess.TimeoutExpired:
        print("✗ OpenOCD timeout")
        return False
    except Exception as e:
        print(f"✗ Error: {e}")
        return False

def test_bootloader_sync():
    """Test UART sync with bootloader via Pico2"""
    print("\nOpening Pico2 serial port...")

    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    except serial.SerialException as e:
        print(f"✗ Failed to open {SERIAL_PORT}: {e}")
        return False

    # Wait a moment for bootloader to initialize
    time.sleep(0.2)

    # Clear any buffered data
    ser.read(ser.in_waiting)

    print("Initializing target via Pico2...")
    ser.write(b'TARGET LPC\r\n')
    time.sleep(0.1)
    ser.read(ser.in_waiting)  # Clear response

    print("Setting timeout...")
    ser.write(b'TARGET TIMEOUT 100\r\n')
    time.sleep(0.1)
    ser.read(ser.in_waiting)  # Clear response

    print("Attempting bootloader sync...")
    ser.write(b'TARGET SYNC 115200 12000 10\r\n')

    # Read response with timeout
    start_time = time.time()
    response = b''
    while time.time() - start_time < 3.0:
        if ser.in_waiting:
            response += ser.read(ser.in_waiting)
        time.sleep(0.1)

    ser.close()

    # Check for successful sync
    response_str = response.decode('utf-8', errors='ignore')
    print(f"\nResponse received ({len(response)} bytes):")
    print(response_str)

    if 'Synchronized' in response_str:
        print("\n✓ SUCCESS! Bootloader responded with 'Synchronized'")
        print("✓ LPC bootloader is active and responding via UART")
        return True
    else:
        print("\n✗ No 'Synchronized' response from bootloader")
        print("✗ Bootloader may not be running or CRP is active")
        return False

def main():
    print("="*70)
    print("LPC Bootloader UART Sync Test via Pico2")
    print("="*70)
    print()

    # Step 1: Reset LPC via OpenOCD
    if not run_openocd_reset():
        print("\n✗ Failed to reset LPC")
        return 1

    # Wait for bootloader to start
    print("\nWaiting for bootloader to initialize...")
    time.sleep(1.0)

    # Step 2: Test UART sync via Pico2
    if test_bootloader_sync():
        print("\n" + "="*70)
        print("RESULT: Bootloader is accessible!")
        print("="*70)
        return 0
    else:
        print("\n" + "="*70)
        print("RESULT: Cannot access bootloader")
        print("="*70)
        print("\nPossible reasons:")
        print("  1. Valid user code is running (checksum OK)")
        print("  2. CRP protection is active")
        print("  3. ISP pin (P2.10) is not held low")
        print("  4. UART not properly connected")
        print("="*70)
        return 1

if __name__ == "__main__":
    sys.exit(main())
