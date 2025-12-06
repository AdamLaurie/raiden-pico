#!/usr/bin/env python3
"""
Inward spiral fill pattern - covers every 1mm point within a 30x30mm square.
Starts at outer edge, completes the box perimeter, then moves 1mm inward and repeats
until reaching the center.
"""
import serial
import time

s = serial.Serial('/dev/ttyACM0', 115200, timeout=35)
time.sleep(1)

def send_cmd(cmd):
    s.write((cmd + '\r\n').encode())
    time.sleep(0.1)
    response = ""
    start = time.time()
    while time.time() - start < 35:
        if s.in_waiting:
            response += s.read(s.in_waiting).decode('utf-8', errors='ignore')
            if 'OK:' in response or 'ERROR:' in response or 'complete' in response.lower():
                break
        time.sleep(0.05)
    return response

def move_to(x, y):
    print(f"  ({x}, {y})", flush=True)
    resp = send_cmd(f'grbl move {x} {y}')
    if 'ERROR' in resp:
        print(f"    ERROR: {resp}", flush=True)
        return False
    time.sleep(1)  # 1 second pause after each move
    return True

# Soft reset Grbl first to clear any stuck alarm state
print("=== Soft reset Grbl ===", flush=True)
resp = send_cmd('grbl reset')
print(f"Reset response: {resp}", flush=True)
time.sleep(2)  # Wait for Grbl to reboot

# Unlock in case we're in alarm state
print("=== Unlocking Grbl ===", flush=True)
resp = send_cmd('grbl unlock')
print(f"Unlock response: {resp}", flush=True)
time.sleep(0.5)

# Autohome to ensure we have travel space
print("=== Auto-homing ===", flush=True)
resp = send_cmd('grbl autohome')
print(f"Autohome response: {resp}", flush=True)
if 'ERROR' in resp:
    print("Homing failed, trying unlock again...", flush=True)
    send_cmd('grbl unlock')
    time.sleep(0.5)

# Set current position as origin after homing
print("=== Setting home position ===", flush=True)
resp = send_cmd('grbl send "G92 X0 Y0 Z0"')
print(f"Set origin response: {resp}", flush=True)
time.sleep(0.5)

print("\n=== Starting inward spiral fill ===", flush=True)
print("Covering every 1mm point in 30x30mm square", flush=True)

size = 30
offset = 0

while offset <= size // 2:
    min_c = offset
    max_c = size - offset

    # Skip if we've reached the center
    if min_c > max_c:
        break

    # Single point at center
    if min_c == max_c:
        print(f"\n--- Center point ({min_c}, {min_c}) ---", flush=True)
        move_to(min_c, min_c)
        break

    print(f"\n--- Ring {offset}: ({min_c},{min_c}) to ({max_c},{max_c}) ---", flush=True)

    # Start at bottom-left corner of this ring
    move_to(min_c, min_c)

    # Bottom edge: left to right
    print(f"  Bottom edge (y={min_c}):", flush=True)
    for x in range(min_c + 1, max_c + 1):
        if not move_to(x, min_c):
            break

    # Right edge: bottom to top
    print(f"  Right edge (x={max_c}):", flush=True)
    for y in range(min_c + 1, max_c + 1):
        if not move_to(max_c, y):
            break

    # Top edge: right to left
    print(f"  Top edge (y={max_c}):", flush=True)
    for x in range(max_c - 1, min_c - 1, -1):
        if not move_to(x, max_c):
            break

    # Left edge: top to bottom (but not back to start - that's the next ring's start)
    print(f"  Left edge (x={min_c}):", flush=True)
    for y in range(max_c - 1, min_c, -1):  # Stop at min_c+1, next ring starts at min_c+1
        if not move_to(min_c, y):
            break

    offset += 1

print("\n=== Pattern complete! ===", flush=True)
print(f"Total points covered: {(size+1) * (size+1)}", flush=True)
s.close()
