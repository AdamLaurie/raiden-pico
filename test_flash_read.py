#!/usr/bin/env python3
"""
Quick test of flash memory reading - assumes target is already in bootloader mode
"""

import serial
import time

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200

def send_command_get_data(ser, cmd, timeout=1.0):
    """Send command in API mode and return response data"""
    # Clear any pending data
    ser.read(ser.in_waiting)

    # Send command
    ser.write(f"{cmd}\r\n".encode())

    # Wait for '.' (command received)
    start = time.time()
    while time.time() - start < timeout:
        if ser.in_waiting > 0:
            char = ser.read(1).decode('utf-8', errors='ignore')
            if char == '.':
                break
        time.sleep(0.01)

    # Wait for '+' (success) or '!' (failure)
    start = time.time()
    while time.time() - start < timeout:
        if ser.in_waiting > 0:
            char = ser.read(1).decode('utf-8', errors='ignore')
            if char == '+':
                # Success! Now read the actual data
                time.sleep(0.1)  # Give it time to send data
                if ser.in_waiting > 0:
                    return ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                return ""
            elif char == '!':
                return None
        time.sleep(0.01)

    return None

ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=2.0)
time.sleep(0.5)

print("Enabling API mode...")
ser.write(b"API ON\r\n")
time.sleep(0.2)
ser.read(ser.in_waiting)

print("\nTesting flash read (reading just 1024 bytes from 0x200)...")
ser.write(b"TARGET SEND \"R 512 1024\"\r\n")
time.sleep(0.5)

print("\nWaiting for data...")
time.sleep(2.0)

print("\nCalling TARGET RESPONSE...")
response = send_command_get_data(ser, "TARGET RESPONSE", timeout=1.0)

if response is None:
    print("ERROR: TARGET RESPONSE failed")
elif response == "":
    print("No data available")
else:
    print(f"Got response ({len(response)} bytes):")
    print(response[:500])
    print("\n---")

    # Try to parse hex
    for line in response.split('\r\n'):
        line = line.strip()
        if not line:
            continue
        print(f"Line: {line}")

        # Strip formatting
        if line.startswith('+>') or line.startswith('>'):
            line = line.split('>', 1)[-1].strip()
        if line.startswith('.'):
            continue

        # Try to parse as hex
        hex_pairs = line.split()
        if hex_pairs and all(len(p) == 2 for p in hex_pairs):
            try:
                text = ''.join(chr(int(p, 16)) for p in hex_pairs)
                print(f"  -> Text: {repr(text)}")
            except:
                pass

ser.write(b"API OFF\r\n")
time.sleep(0.2)
ser.close()
