#!/usr/bin/env python3
"""
Test flash reading without API mode - simpler and faster
"""

import serial
import time

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200

def send_command(ser, cmd):
    """Send command and wait for prompt"""
    ser.write(f"{cmd}\r\n".encode())
    # Wait for '>' prompt
    while True:
        ch = ser.read(1)
        if ch == b'>':
            return True

print("Opening serial port...")
ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.1)

# Clear any buffered data
ser.read(ser.in_waiting)

print("Initializing target...")
send_command(ser, "TARGET LPC")

print("Setting timeout to 10ms...")
send_command(ser, "TARGET TIMEOUT 10")

print("Syncing with bootloader...")
send_command(ser, "TARGET SYNC 115200 12000 10")
time.sleep(1.0)

# Clear sync response
ser.read(ser.in_waiting)

def read_line(ser):
    """Read a single line from serial"""
    while True:
        line = ser.read_until(b'\r').decode('utf-8', errors='ignore').strip()
        if line:  # Skip empty lines
            return line
        if not ser.in_waiting:  # No more data
            return None

def calculate_uue_checksum(lines):
    """Calculate checksum for UUE lines (sum of all decoded bytes)"""
    total = 0
    for line in lines:
        # Decode UUE line
        length = ord(line[0]) - 32
        for i in range(0, length, 3):
            chunk = line[1 + (i // 3) * 4 : 1 + (i // 3) * 4 + 4]
            if len(chunk) == 4:
                # Decode 4 UUE chars to 3 bytes
                b1 = (ord(chunk[0]) - 32) << 2 | (ord(chunk[1]) - 32) >> 4
                b2 = ((ord(chunk[1]) - 32) & 0xF) << 4 | (ord(chunk[2]) - 32) >> 2
                b3 = ((ord(chunk[2]) - 32) & 0x3) << 6 | (ord(chunk[3]) - 32)
                if i < length:
                    total += b1
                if i + 1 < length:
                    total += b2
                if i + 2 < length:
                    total += b3
    return total & 0xFFFFFFFF

# Full flash dump parameters
start_addr = 0
bytes_to_read = 516096

print(f"\nReading full flash: {bytes_to_read} bytes from 0x{start_addr:X}...")

# Calculate expected number of UUE lines
expected_lines = (bytes_to_read + 44) // 45  # Round up
print(f"Expecting {expected_lines} UUE lines total\n")

# Send single R command for full dump
print(f"Sending R command...")
ser.write(f'TARGET SEND "R {start_addr} {bytes_to_read}"\r\n'.encode())

# Read firmware echo
fw_echo = read_line(ser)

# Read target echo (up to first \r)
target_echo = read_line(ser)
print(f"Target echo: {target_echo}")

# Read error code (next line after \r)
error_line = read_line(ser)
if not error_line or not error_line[0].isdigit():
    print(f"ERROR: Expected error code, got: {repr(error_line)}")
    ser.close()
    exit(1)

error_code = error_line[0]
if error_code != '0':
    print(f"ERROR: Command failed with error code {error_code}")
    ser.close()
    exit(1)

print(f"✓ Error code: {error_code}\n")

# Step 2: Read UUE data following continuation protocol
# Protocol: up to 20 UUE lines, checksum, send OK (wait for >), repeat
uue_lines = []
lines_remaining = expected_lines
first_chunk = True

while lines_remaining > 0:
    chunk_size = min(20, lines_remaining)
    chunk_lines = []

    # If this is a continuation (not the first chunk), we need to read echo responses first
    if not first_chunk:
        # Read firmware echo ("> TARGET SEND "OK"" or similar)
        fw_echo = read_line(ser)
        # Read Pico prompt (">")
        pico_prompt = read_line(ser)
        # Read target echo ("OK")
        target_ok_echo = read_line(ser)

    first_chunk = False

    # Read chunk_size UUE lines
    for i in range(chunk_size):
        line = read_line(ser)
        if not line:
            print(f"ERROR: Timeout reading line {len(uue_lines) + i + 1}/{expected_lines}")
            ser.close()
            exit(1)

        # Verify it's a UUE line
        if line[0] not in ' !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLM':
            print(f"ERROR: Invalid UUE line at {len(uue_lines) + i + 1}: {repr(line)}")
            ser.close()
            exit(1)

        chunk_lines.append(line)

    # Read checksum
    checksum_line = read_line(ser)

    if not checksum_line or not checksum_line.isdigit():
        print(f"ERROR: Expected checksum after line {len(uue_lines) + chunk_size}, got: {repr(checksum_line)}")
        ser.close()
        exit(1)

    received_checksum = int(checksum_line)

    # Calculate expected checksum for this chunk
    # Replace backticks with spaces before calculating (LPC uses backticks for spaces)
    chunk_lines_fixed = [line.replace('`', ' ') for line in chunk_lines]
    calculated_checksum = calculate_uue_checksum(chunk_lines_fixed)

    # Verify checksum
    if received_checksum != calculated_checksum:
        print(f"ERROR: Checksum mismatch at line {len(uue_lines)}!")
        print(f"  Expected: {calculated_checksum}")
        print(f"  Received: {received_checksum}")
        ser.close()
        exit(1)

    # Consume the \n after checksum if present
    if ser.in_waiting > 0:
        ser.read(1)

    uue_lines.extend(chunk_lines)
    lines_remaining -= chunk_size

    # Progress update every 100 lines
    if len(uue_lines) % 100 == 0 or lines_remaining == 0:
        print(f"  Progress: {len(uue_lines)}/{expected_lines} lines ({len(uue_lines) * 45} bytes)")

    # If more lines to read, send OK to continue
    if lines_remaining > 0:
        ser.write(b'TARGET SEND "OK"\r\n')
        # The Pico will send firmware echo, then target echo
        # Both will be read at start of next loop iteration

# Read end marker (backtick character - LPC uses backtick for space)
# In non-API mode, we may get '>' prompt directly instead
end_marker = read_line(ser)
if end_marker not in ('`', '>'):
    print(f"WARNING: Unexpected end marker: {repr(end_marker)}")

print(f"\n✓ Successfully read {len(uue_lines)} UUE lines")

# Save to .uue file and decode
output_file = "flash_test.uue"
print(f"\nSaving to {output_file}...")

with open(output_file, 'w') as f:
    f.write("begin 644 flash.bin\n")
    for line in uue_lines:
        # Replace backticks with spaces (LPC ISP uses backticks for spaces)
        fixed_line = line.replace('`', ' ')
        f.write(fixed_line + "\n")
    f.write(" \n")  # Empty line (0 bytes) - space, not backtick
    f.write("end\n")

print(f"✓ Saved {len(uue_lines)} lines to {output_file}")

# Try to decode
print("\nDecoding UUE file...")
import subprocess
result = subprocess.run(['uudecode', '-o', '/tmp/flash_test.bin', output_file],
                       capture_output=True, text=True)

if result.returncode == 0:
    import os
    decoded_size = os.path.getsize('/tmp/flash_test.bin')
    print(f"✓ Successfully decoded to {decoded_size} bytes")
    print(f"  Expected: {bytes_to_read} bytes")
    if decoded_size == bytes_to_read:
        print(f"✓ Size matches!")
        # Show first 32 bytes as hex
        with open('/tmp/flash_test.bin', 'rb') as f:
            data = f.read(32)
            hex_str = ' '.join(f'{b:02X}' for b in data)
            print(f"  First 32 bytes: {hex_str}")
    else:
        print(f"✗ Size mismatch!")
else:
    print(f"✗ Decode failed: {result.stderr}")

print("\nClosing serial port...")
ser.close()
print("Done.")
