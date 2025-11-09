#!/usr/bin/env python3
"""
Test parsing UUE data from Raiden Pico hex dump format
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

print("\nSending R command to read 1024 bytes from 0x200...")
ser.write(b'TARGET SEND "R 512 1024"\r\n')

# Parse UUE lines from hex dump
uue_lines = []
current_line = ""
timeout = time.time() + 10

while time.time() < timeout:
    try:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
    except:
        time.sleep(0.01)
        continue

    if not line:
        time.sleep(0.01)
        continue

    print(f"Read: {line[:80]}")  # Show first 80 chars

    # Look for hex dump lines
    if '|' in line:
        parts = line.split('|', 1)
        if len(parts) == 2:
            hex_part = parts[0].strip()
            hex_bytes = hex_part.split()

            for hex_byte in hex_bytes:
                try:
                    byte_val = int(hex_byte, 16)
                    char = chr(byte_val)

                    if byte_val == 0x0A:  # Newline
                        if current_line:
                            uue_line = current_line.replace('`', ' ')
                            print(f"  -> UUE line: {repr(uue_line)}")

                            # Check if end marker
                            if uue_line == ' ':
                                print("  -> END MARKER!")
                                break
                            elif uue_line and uue_line[0] in ' !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLM':
                                uue_lines.append(uue_line)
                            elif uue_line.isdigit():
                                print(f"  -> CHECKSUM: {uue_line}")
                                print("     Sending OK...")
                                ser.write(b'TARGET SEND "OK"\r\n')
                                timeout = time.time() + 10  # Reset timeout

                            current_line = ""
                    elif byte_val != 0x0D:
                        current_line += char
                except ValueError:
                    pass

print(f"\n\nCollected {len(uue_lines)} UUE lines")
if uue_lines:
    print("\nFirst 5 lines:")
    for i, line in enumerate(uue_lines[:5]):
        print(f"  {i+1}: {repr(line)}")

    print("\nLast 5 lines:")
    for i, line in enumerate(uue_lines[-5:]):
        print(f"  {len(uue_lines)-4+i}: {repr(line)}")

print("\nDisabling API mode...")
ser.write(b"API OFF\r\n")
time.sleep(0.2)
ser.close()
print("Done.")
