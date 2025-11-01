#!/usr/bin/env python3
"""Debug script to see exact SYNC responses"""

import serial
import time

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=2)
time.sleep(0.5)

# Clear buffer
ser.read(ser.in_waiting)

# Configure
print("Configuring...")
ser.write(b"TARGET LPC\r\n")
time.sleep(0.2)
print(ser.read(ser.in_waiting).decode('utf-8', errors='ignore'))

ser.write(b"TRIGGER GPIO RISING\r\n")
time.sleep(0.2)
print(ser.read(ser.in_waiting).decode('utf-8', errors='ignore'))

ser.write(b"SET PAUSE 0\r\n")
time.sleep(0.2)
print(ser.read(ser.in_waiting).decode('utf-8', errors='ignore'))

ser.write(b"SET WIDTH 150\r\n")
time.sleep(0.2)
print(ser.read(ser.in_waiting).decode('utf-8', errors='ignore'))

# Arm and reset
print("\nArming...")
ser.write(b"ARM ON\r\n")
time.sleep(0.2)
print(ser.read(ser.in_waiting).decode('utf-8', errors='ignore'))

print("\nResetting target...")
ser.write(b"TARGET RESET\r\n")
time.sleep(1.5)
reset_resp = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
print(f"RESET response: '{reset_resp}'")

# Try SYNC
print("\nSending SYNC...")
ser.write(b"TARGET SYNC 115200 12000 10\r\n")

# Read response in chunks to see what arrives
print("Waiting for SYNC response...")
full_response = ""
for i in range(80):  # 8 seconds total
    time.sleep(0.1)
    if ser.in_waiting > 0:
        chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
        full_response += chunk
        print(f"[{i*0.1:.1f}s] Received: {repr(chunk)}")
        if "complete" in chunk or "failed" in chunk.lower():
            break

print(f"\n\nFull response:\n{full_response}")
print(f"\n\nResponse length: {len(full_response)} chars")

ser.close()
