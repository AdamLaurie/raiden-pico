#!/usr/bin/env python3
"""
Test flash reading with new Response header parsing
"""

import serial
import time
import re

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200

def send_command(ser, cmd):
    """Send command and wait for completion markers"""
    ser.write(f"{cmd}\r\n".encode())

    # Wait for command receipt (.) and completion (+)
    timeout = time.time() + 5.0
    while time.time() < timeout:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if line.startswith('.'):
            continue
        if line.startswith('+'):
            return True
        if line.startswith('!'):
            print(f"ERROR: {line}")
            return False
    return False

print("Opening serial port...")
ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.5)
time.sleep(0.5)

# Clear any buffered data
ser.read(ser.in_waiting)

print("Enabling API mode...")
ser.write(b"API ON\r\n")
time.sleep(0.2)
ser.read(ser.in_waiting)

print("Initializing target...")
send_command(ser, "TARGET LPC")
time.sleep(0.2)

print("Syncing with bootloader...")
send_command(ser, "TARGET SYNC 115200 12000 10")
time.sleep(3.0)

# Clear sync response
ser.read(ser.in_waiting)

print("\nSending R command to read 1024 bytes from 0x200...")
ser.write(b'TARGET SEND "R 512 1024"\r\n')
time.sleep(0.5)

# Parse using new approach: wait for Response header
print("\nWaiting for Response header...")
response_header = None
timeout = time.time() + 5.0

while time.time() < timeout:
    line = ser.readline().decode('utf-8', errors='ignore').strip()
    print(f"Read: {repr(line)}")

    if line.startswith('Response ('):
        response_header = line
        print(f"✓ Found Response header: {response_header}")
        break
    elif line.startswith('!'):
        print(f"ERROR: Command failed: {line}")
        ser.close()
        exit(1)

if not response_header:
    print("ERROR: Timeout waiting for Response header")
    ser.close()
    exit(1)

# Extract expected byte count
match = re.search(r'Response \((\d+) bytes\)', response_header)
if not match:
    print(f"ERROR: Could not parse byte count from: {response_header}")
    ser.close()
    exit(1)

expected_bytes = int(match.group(1))
print(f"\n✓ Expecting {expected_bytes} bytes in response")

# Read first few hex lines to verify parsing
print("\nReading first few hex lines...")
uue_lines = []
timeout = time.time() + 10.0

while time.time() < timeout and len(uue_lines) < 5:
    line = ser.readline().decode('utf-8', errors='ignore').strip()

    if not line:
        time.sleep(0.01)
        continue

    # Check if end marker
    if line.startswith('+>') or line.startswith('>'):
        print("✓ Received end of response marker")
        break

    # Check if hex line
    hex_bytes = line.split()
    if hex_bytes and all(len(b) == 2 and all(c in '0123456789ABCDEFabcdef' for c in b) for b in hex_bytes):
        # Convert to string
        chars = ''.join(chr(int(b, 16)) for b in hex_bytes)

        if chars:
            # Replace backtick with space
            uue_line = chars.replace('`', ' ')

            # Check if UUE line
            if uue_line and uue_line[0] in ' !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLM':
                uue_lines.append(uue_line)
                print(f"  UUE line #{len(uue_lines)}: {repr(uue_line[:60])}")
            elif uue_line.isdigit():
                print(f"  Checksum: {uue_line}")
                # Don't send OK for this test, just observe
                break

print(f"\n✓ Successfully parsed {len(uue_lines)} UUE lines")

print("\nDisabling API mode...")
ser.write(b"API OFF\r\n")
time.sleep(0.2)
ser.close()
print("Done.")
