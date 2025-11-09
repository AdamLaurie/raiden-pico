#!/usr/bin/env python3
"""
Test parsing UUE data from new hex format
"""

import serial
import time

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200

print("Opening serial port...")
ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.5)
time.sleep(0.5)

print("Enabling API mode...")
ser.write(b"API ON\r\n")
time.sleep(0.2)
ser.read(ser.in_waiting)

# First initialize target and enter bootloader
print("\nInitializing target...")
ser.write(b"TARGET LPC\r\n")
time.sleep(0.2)
ser.read(ser.in_waiting)

print("Entering bootloader...")
ser.write(b"TARGET SYNC 115200 12000 10\r\n")
time.sleep(3.0)
response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
print(f"Sync response: {response[:200]}")

print("\nSending R command to read 1024 bytes from 0x200...")
ser.write(b'TARGET SEND "R 512 1024"\r\n')

# Parse UUE lines from hex format
uue_lines = []
timeout = time.time() + 10

print("\nReading response...")
while time.time() < timeout:
    try:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
    except:
        time.sleep(0.01)
        continue

    if not line:
        time.sleep(0.01)
        continue

    print(f"Line: {line[:80]}")

    # Skip header lines
    if line.startswith('Response') or line.startswith('.') or line.startswith('+>') or line.startswith('>'):
        continue
    if line.startswith('OK:') or line.startswith('TARGET'):
        continue

    # Check if this is a hex line
    hex_bytes = line.split()
    if hex_bytes and all(len(b) == 2 and all(c in '0123456789ABCDEFabcdef' for c in b) for b in hex_bytes):
        # Convert hex to string
        chars = ''.join(chr(int(b, 16)) for b in hex_bytes if int(b, 16) not in [0x0D, 0x0A])

        if chars:
            print(f"  -> Parsed: {repr(chars)}")

            # Check if this is a UUE line
            if chars and chars[0] in ' `!"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLM':
                uue_line = chars.replace('`', ' ')
                if uue_line == ' ':
                    print("  -> END MARKER!")
                    break
                uue_lines.append(uue_line)
                print(f"  -> Added UUE line #{len(uue_lines)}")
            elif chars.isdigit():
                print(f"  -> CHECKSUM: {chars}, sending OK")
                ser.write(b'TARGET SEND "OK"\r\n')
                timeout = time.time() + 10

print(f"\n\nCollected {len(uue_lines)} UUE lines")
if uue_lines:
    print("\nFirst 3 lines:")
    for i, line in enumerate(uue_lines[:3]):
        print(f"  {i+1}: {repr(line[:60])}")

print("\nDisabling API mode...")
ser.write(b"API OFF\r\n")
time.sleep(0.2)
ser.close()
print("Done.")
