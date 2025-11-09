#!/usr/bin/env python3
"""
Test full flash extraction (515584 bytes from 0x200) following LPC ISP protocol exactly
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

def read_hex_line(ser):
    """Read next hex line from serial and convert to ASCII string"""
    timeout = time.time() + 5.0
    while time.time() < timeout:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if not line:
            time.sleep(0.01)
            continue

        # Check if end of response
        if line.startswith('+>') or line.startswith('>'):
            return None

        # Parse hex bytes
        hex_bytes = line.split()
        if hex_bytes and all(len(b) == 2 and all(c in '0123456789ABCDEFabcdef' for c in b) for b in hex_bytes):
            chars = ''.join(chr(int(b, 16)) for b in hex_bytes)
            # Replace LPC's backtick with space
            return chars.replace('`', ' ')

    raise TimeoutError("Timeout waiting for hex line")

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

# Flash extraction parameters
start_addr = 0x200
total_size = 515584  # 0x7DE00
chunk_size = 360  # 8 UUE lines * 45 bytes = 360 bytes (fits in 512-byte firmware buffer)

print(f"\nExtracting flash: {total_size} bytes from 0x{start_addr:X}...")
print(f"Reading in {chunk_size}-byte chunks...")

all_uue_lines = []
offset = 0

while offset < total_size:
    remaining = total_size - offset
    read_size = min(chunk_size, remaining)
    current_addr = start_addr + offset

    print(f"\n  Chunk {offset // chunk_size + 1}: Reading {read_size} bytes from 0x{current_addr:X}...")

    ser.write(f'TARGET SEND "R {current_addr} {read_size}"\r\n'.encode())
    time.sleep(0.5)

    # Wait for Response header
    print("    Waiting for Response header...")
    response_header = None
    timeout = time.time() + 10.0

    while time.time() < timeout:
        line = ser.readline().decode('utf-8', errors='ignore').strip()

        if line.startswith('Response ('):
            response_header = line
            print(f"    ✓ Found Response header: {response_header}")
            break
        elif line.startswith('!'):
            print(f"    ERROR: Command failed: {line}")
            ser.close()
            exit(1)

    if not response_header:
        print("    ERROR: Timeout waiting for Response header")
        ser.close()
        exit(1)

    # Extract expected byte count
    match = re.search(r'Response \((\d+) bytes\)', response_header)
    if not match:
        print(f"    ERROR: Could not parse byte count from: {response_header}")
        ser.close()
        exit(1)

    expected_bytes = int(match.group(1))
    print(f"    ✓ Expecting {expected_bytes} bytes in response")

    # Follow LPC ISP protocol exactly:
    # 1. Error code (1 line)
    # 2. Loop: 20 UUE lines, then 1 checksum (send OK), repeat until end marker

    print(f"    Reading response following LPC ISP protocol...")

    # Step 1: Read error code
    error_code = read_hex_line(ser)
    if error_code is None or not error_code.isdigit():
        print(f"    ERROR: Expected error code, got: {error_code}")
        ser.close()
        exit(1)

    print(f"    Error code: {error_code}")
    if error_code != "0":
        print(f"    ERROR: Command failed with error code {error_code}")
        ser.close()
        exit(1)

    # Step 2: Read UUE data in blocks of 20 lines
    chunk_uue_lines = []
    chunk_line_count = 0
    checksum_count = 0
    found_end_marker = False

    while not found_end_marker:
        # Read up to 20 UUE lines
        for i in range(20):
            uue_line = read_hex_line(ser)
            if uue_line is None:
                # End of this chunk's response
                found_end_marker = True
                break

            # Check for end marker (space indicates end of UUE data)
            if uue_line == ' ':
                chunk_uue_lines.append(uue_line)
                found_end_marker = True
                print(f"    ✓ Found end marker after {chunk_line_count} UUE lines in this chunk")
                break

            # Add UUE line
            chunk_uue_lines.append(uue_line)
            chunk_line_count += 1

        if found_end_marker:
            break

        # Read checksum
        checksum = read_hex_line(ser)
        if checksum is None or not checksum.isdigit():
            print(f"    ERROR: Expected checksum, got: {checksum}")
            break

        checksum_count += 1
        print(f"    Checksum #{checksum_count}: {checksum}, sending OK...", flush=True)

        # Send OK response
        ser.write(b'TARGET SEND "OK"\r\n')
        time.sleep(0.2)

        # Discard command echo and OK response
        discard_timeout = time.time() + 2.0
        while time.time() < discard_timeout:
            discard_line = ser.readline().decode('utf-8', errors='ignore').strip()
            if discard_line.startswith('+'):
                break

    # Add this chunk's UUE lines to the total (except end marker)
    all_uue_lines.extend([line for line in chunk_uue_lines if line != ' '])
    print(f"    ✓ Chunk complete: {chunk_line_count} UUE lines (total so far: {len(all_uue_lines)} lines)")

    # Clear any remaining response data before next chunk
    time.sleep(0.2)
    ser.read(ser.in_waiting)

    # Advance offset for next chunk
    offset += read_size

print(f"\n✓ Extraction complete!")
print(f"  Total UUE lines: {len(all_uue_lines)}")
print(f"  Total chunks processed: {(offset // chunk_size)}")

# Save to .uue file
output_file = "flash_dump_test.uue"
print(f"\nSaving to {output_file}...")

with open(output_file, 'w') as f:
    f.write("begin 644 flash.bin\n")
    for line in all_uue_lines:
        f.write(line + "\n")
    # Add UUE end marker
    f.write(" \n")
    f.write("end\n")

print(f"✓ Saved {len(all_uue_lines)} lines to {output_file}")

# Verify we can decode it
print("\nVerifying UUE decode...")
import subprocess
result = subprocess.run(['uudecode', '-o', '/tmp/flash_test.bin', output_file],
                       capture_output=True, text=True)

if result.returncode == 0:
    # Check file size
    import os
    decoded_size = os.path.getsize('/tmp/flash_test.bin')
    print(f"✓ Successfully decoded to {decoded_size} bytes")
    print(f"  Expected: {total_size} bytes")
    if decoded_size == total_size:
        print(f"✓ Size matches!")
    else:
        print(f"✗ Size mismatch!")
else:
    print(f"✗ Decode failed: {result.stderr}")

print("\nDisabling API mode...")
ser.write(b"API OFF\r\n")
time.sleep(0.2)
ser.close()
print("Done.")
