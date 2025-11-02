#!/usr/bin/env python3
"""
Adaptive voltage glitch test.
Starts at specified voltage and automatically reduces if crash rate is too high.
Runs 5000 tests total, adjusting voltage as needed to find optimal parameters.
"""

import serial
import time
import csv
from datetime import datetime
import sys

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200
TIMEOUT = 2.0

# Target crash rate range (will adjust voltage to stay in this range)
TARGET_CRASH_RATE_MIN = 10  # Minimum 10% crashes
TARGET_CRASH_RATE_MAX = 60  # Maximum 60% crashes
SAMPLE_SIZE = 50  # Check every 50 tests

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


def check_and_clear_chipshouter_faults(ser, voltage=None):
    """Check for ChipSHOUTER faults and clear them.

    Returns: (was_faulted, did_reset)
    """
    response = send_command(ser, "CS STATUS", wait_time=0.5)

    if "fault" in response.lower():
        print("\n⚠ WARNING: ChipSHOUTER is faulted! Clearing faults...", flush=True)

        # Try to clear faults first (don't reset unless necessary)
        send_command(ser, "CS CLEAR FAULTS", wait_time=0.5)
        time.sleep(1.0)

        # Check if clear worked
        response = send_command(ser, "CS STATUS", wait_time=0.5)
        if "fault" not in response.lower():
            print("  ✓ ChipSHOUTER faults cleared successfully", flush=True)
            # Re-arm after clearing
            send_command(ser, "CS ARM", wait_time=1.0)
            if voltage is not None:
                send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5)
            return True, False

        # Clear didn't work - reset as last resort
        print("  ⚠ Clear faults didn't work, resetting ChipSHOUTER...", flush=True)
        send_command(ser, "CS RESET", wait_time=0.5)
        time.sleep(5.0)

        # Re-configure voltage and re-arm after reset
        if voltage is not None:
            print(f"  Reconfiguring voltage to {voltage}V after reset...", flush=True)
            send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5)
            time.sleep(1.0)

        # Re-arm
        send_command(ser, "CS ARM", wait_time=1.0)

        print("  ✓ ChipSHOUTER reset complete", flush=True)
        return True, True

    return False, False


def setup_system(ser):
    """Initial setup."""
    print("Setting up system...", flush=True)
    send_command(ser, "TARGET LPC", wait_time=0.2)
    send_command(ser, "TRIGGER UART 0d", wait_time=0.2)
    send_command(ser, "CS TRIGGER HARDWARE HIGH", wait_time=0.5)
    send_command(ser, "CS ARM", wait_time=1.0)
    print("Setup complete\n", flush=True)


def test_single_glitch(ser, voltage, pause, width):
    """Test single glitch parameters."""
    # Update ChipSHOUTER voltage
    send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5)

    # Update Pico pause
    send_command(ser, f"SET PAUSE {pause}", wait_time=0.2)

    # Update Pico width
    send_command(ser, f"SET WIDTH {width}", wait_time=0.2)

    # Sync with bootloader
    ser.write(b"TARGET SYNC 115200 12000 10\r\n")
    success, _ = wait_for_response(ser, "LPC ISP sync complete", timeout=5.0)
    if not success:
        return "SYNC_FAIL", ""

    # Arm Pico trigger
    send_command(ser, "ARM ON", wait_time=0.2)

    # Send read command to target
    ser.write(b'TARGET SEND "R 0 516096"\r\n')

    # Wait for response with longer timeout to capture target data
    response = ""
    start_time = time.time()
    max_wait = 3.0

    while time.time() - start_time < max_wait:
        time.sleep(0.1)
        if ser.in_waiting > 0:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            response += chunk
            if "> " in chunk or "ARM ON" in chunk:
                break

    # Parse result based on actual response content
    if "Response (" in response:
        # Target responded - check what it sent
        if "31 39 0D 0A" in response or "| 19.." in response:
            return "ERROR19", ""
        elif "30 0D 0A" in response or "30 " in response and "| 0" in response:
            return "SUCCESS", ""
        else:
            # Extract hex data
            hex_data = ""
            for line in response.split('\n'):
                if line.strip() and not line.startswith('>') and not line.startswith('OK:') and not line.startswith('TARGET'):
                    if any(c in line for c in '0123456789ABCDEF') and len(line) > 10:
                        hex_data += line.strip() + " "
            return "GARBAGE", hex_data.strip()
    elif "No response data" in response:
        return "NO_RESPONSE", ""
    elif not response.strip():
        return "NO_RESPONSE", ""
    else:
        return "UNKNOWN", response[:100]


def main():
    # Test parameters
    starting_voltage = 290
    pause = 0
    width = 150
    total_iterations = 5000

    print("=" * 70)
    print(f"ADAPTIVE VOLTAGE GLITCH TEST")
    print("=" * 70)
    print(f"Starting voltage: {starting_voltage}V")
    print(f"Pause: {pause} cycles")
    print(f"Width: {width} cycles")
    print(f"Total iterations: {total_iterations}")
    print(f"Target crash rate: {TARGET_CRASH_RATE_MIN}-{TARGET_CRASH_RATE_MAX}%")
    print(f"Will adjust voltage every {SAMPLE_SIZE} tests")
    print("=" * 70)
    print()

    # Create CSV log
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    csv_file = f"adaptive_voltage_test_{timestamp}.csv"

    # Connect
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
    time.sleep(0.5)

    try:
        setup_system(ser)

        # Counters
        current_voltage = starting_voltage
        success_count = 0
        no_response_count = 0
        error19_count = 0
        garbage_count = 0
        sync_fail_count = 0
        unknown_count = 0

        # Windowed counters for voltage adjustment
        window_success = 0
        window_crash = 0
        window_normal = 0

        start_time = time.time()
        last_print_time = start_time
        last_fault_check = start_time

        with open(csv_file, 'w', newline='', buffering=1) as f:
            writer = csv.writer(f)
            writer.writerow(['iteration', 'timestamp', 'voltage', 'pause', 'width', 'result', 'elapsed_time', 'response_data'])

            for i in range(total_iterations):
                test_start = time.time()
                result, extra_data = test_single_glitch(ser, current_voltage, pause, width)
                test_elapsed = time.time() - test_start

                # Log to CSV
                ts = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                writer.writerow([i+1, ts, current_voltage, pause, width, result, f"{test_elapsed:.2f}", extra_data])
                f.flush()

                # Update counters
                if result == "SUCCESS":
                    success_count += 1
                    window_success += 1
                    print(f"\n[{i+1}/{total_iterations}] V={current_voltage} ✓✓✓ SUCCESS ✓✓✓", flush=True)
                elif result == "ERROR19":
                    error19_count += 1
                    window_normal += 1
                elif result == "NO_RESPONSE":
                    no_response_count += 1
                    window_crash += 1
                elif result == "GARBAGE":
                    garbage_count += 1
                    print(f"\n[{i+1}/{total_iterations}] V={current_voltage} ⚠ GARBAGE: {extra_data[:60]}", flush=True)
                elif result == "SYNC_FAIL":
                    sync_fail_count += 1
                else:
                    unknown_count += 1

                # Check if we should adjust voltage (every SAMPLE_SIZE tests)
                if (i + 1) % SAMPLE_SIZE == 0:
                    window_total = window_success + window_crash + window_normal
                    if window_total > 0:
                        window_crash_rate = (window_crash / window_total) * 100
                        window_success_rate = (window_success / window_total) * 100

                        print(f"\n[VOLTAGE CHECK] V={current_voltage}", flush=True)
                        print(f"  Last {SAMPLE_SIZE} tests: Success={window_success_rate:.1f}% Crash={window_crash_rate:.1f}%", flush=True)

                        # Adjust voltage if crash rate is too high
                        if window_crash_rate > TARGET_CRASH_RATE_MAX:
                            old_voltage = current_voltage
                            current_voltage -= 1
                            if current_voltage < 150:
                                current_voltage = 150
                            print(f"  ⚠ Crash rate too high! Reducing voltage: {old_voltage}V → {current_voltage}V", flush=True)

                        # Increase voltage if crash rate is too low (more room for glitching)
                        elif window_crash_rate < TARGET_CRASH_RATE_MIN and window_success_rate == 0:
                            old_voltage = current_voltage
                            current_voltage += 1
                            if current_voltage > 500:
                                current_voltage = 500
                            print(f"  ↑ Crash rate too low, trying higher voltage: {old_voltage}V → {current_voltage}V", flush=True)

                    # Reset window counters
                    window_success = 0
                    window_crash = 0
                    window_normal = 0

                # Check for ChipSHOUTER faults every 60 seconds
                current_time = time.time()
                if current_time - last_fault_check >= 60:
                    print(f"\n[Periodic fault check]", flush=True)
                    try:
                        was_faulted, did_reset = check_and_clear_chipshouter_faults(ser, voltage=current_voltage)
                        if was_faulted:
                            print("  WARNING: ChipSHOUTER was faulted but has been cleared", flush=True)
                            if did_reset:
                                # After reset, need to reconfigure trigger
                                send_command(ser, "CS TRIGGER HARDWARE HIGH", wait_time=0.5)
                    except Exception as e:
                        print(f"  Fault check error: {e}", flush=True)
                    last_fault_check = current_time

                # Print progress every 30 seconds
                if current_time - last_print_time >= 30:
                    elapsed = current_time - start_time
                    remaining = (elapsed / (i + 1)) * (total_iterations - (i + 1))
                    tests_per_sec = (i + 1) / elapsed

                    print(f"\n[{i+1}/{total_iterations}] Progress: {(i+1)/total_iterations*100:.1f}% | V={current_voltage}", flush=True)
                    print(f"  Success: {success_count} ({success_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  Crash: {no_response_count} ({no_response_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  Normal: {error19_count} ({error19_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  Garbage: {garbage_count} ({garbage_count/(i+1)*100:.2f}%)", flush=True)
                    print(f"  Speed: {tests_per_sec:.2f} tests/sec | Remaining: {remaining/60:.1f} min\n", flush=True)

                    last_print_time = current_time

        # Final summary
        elapsed = time.time() - start_time
        print("\n" + "=" * 70)
        print("TEST COMPLETE")
        print("=" * 70)
        print(f"Duration: {elapsed/60:.1f} minutes")
        print(f"Total tests: {total_iterations}")
        print()
        print("Results:")
        print(f"  SUCCESS:     {success_count:5d} ({success_count/total_iterations*100:6.2f}%)")
        print(f"  NO_RESPONSE: {no_response_count:5d} ({no_response_count/total_iterations*100:6.2f}%)")
        print(f"  ERROR19:     {error19_count:5d} ({error19_count/total_iterations*100:6.2f}%)")
        print(f"  GARBAGE:     {garbage_count:5d} ({garbage_count/total_iterations*100:6.2f}%)")
        print()
        print(f"Results saved to: {csv_file}")
        print("=" * 70)

    finally:
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
