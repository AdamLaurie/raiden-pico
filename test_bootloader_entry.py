#!/usr/bin/env python3
"""
Test script to use OpenOCD to force bootloader entry, then verify via UART
"""

import subprocess
import serial
import time
import sys

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200

def run_openocd_command(commands):
    """Run OpenOCD with a series of commands"""
    cmd = ["openocd", "-f", "/usr/share/openocd/scripts/interface/jlink.cfg",
           "-f", "/usr/share/openocd/scripts/target/lpc2478.cfg"]
    for c in commands:
        cmd.extend(["-c", c])
    cmd.extend(["-c", "exit"])

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    return result.returncode == 0

def test_uart_sync():
    """Test UART sync with bootloader"""
    print("Opening serial port...")
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)

    # Clear any buffered data
    time.sleep(0.1)
    ser.read(ser.in_waiting)

    print("Sending '?' for autobaud sync...")
    ser.write(b'?')
    time.sleep(0.1)

    response = ser.read(100)
    print(f"Response: {response}")

    if b'Synchronized' in response:
        print("✓ Bootloader responded with 'Synchronized'!")
        ser.write(b'Synchronized\r\n')
        time.sleep(0.1)
        response = ser.read(100)
        print(f"Response: {response}")
        ser.close()
        return True
    else:
        print("✗ No bootloader response")
        ser.close()
        return False

# Method 1: Jump to bootloader ROM directly
print("=" * 70)
print("Method 1: Jumping to bootloader ROM (0x7FFF0000)")
print("=" * 70)
commands = [
    "init",
    "reset halt",
    "arm7_9 fast_memory_access enable",
    "reg pc 0x7FFF0000",  # Jump to bootloader ROM
    "resume"
]
if run_openocd_command(commands):
    print("✓ OpenOCD commands executed")
    time.sleep(0.5)
    if test_uart_sync():
        print("✓ Bootloader is running and responding via UART!")
        sys.exit(0)
else:
    print("✗ OpenOCD command failed")

# Method 2: Invalidate user code checksum
print("\n" + "=" * 70)
print("Method 2: Invalidating user code checksum at 0x14")
print("=" * 70)
commands = [
    "init",
    "reset halt",
    "arm7_9 fast_memory_access enable",
    "mww 0x00000014 0xFFFFFFFF",  # Invalidate checksum
    "reset run"
]
if run_openocd_command(commands):
    print("✓ OpenOCD commands executed")
    time.sleep(0.5)
    if test_uart_sync():
        print("✓ Bootloader is running and responding via UART!")
        sys.exit(0)
else:
    print("✗ OpenOCD command failed")

print("\n✗ Could not enter bootloader mode")
sys.exit(1)
