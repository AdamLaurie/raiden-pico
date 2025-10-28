#!/usr/bin/env python3
"""
ChipSHOUTER LPC Glitch Marathon Test

Long-running test script designed to run for hours.
Tests parameter space repeatedly to find rare glitch windows.
Logs all results to CSV for analysis.
"""

import serial
import time
import sys
import csv
import os
from datetime import datetime

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200
TIMEOUT = 2.0

# Results CSV - will be set with timestamp in run_marathon()
RESULTS_FILE = None


def send_command(ser, cmd, wait_time=0.2, verbose=False):
    """Send a command to the Pico and return the response."""
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)

    ser.write(f"{cmd}\r\n".encode())

    # For ChipSHOUTER commands, poll for response over longer period
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
    """Wait for a specific response text."""
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


def check_and_clear_chipshouter_faults(ser):
    """Check for ChipSHOUTER faults and clear them."""
    # Check status
    response = send_command(ser, "CS STATUS", wait_time=0.5)

    # Look for fault state
    if "fault" in response.lower():
        print("WARNING: ChipSHOUTER is faulted! Clearing faults...")

        # Clear faults
        send_command(ser, "CS CLEAR FAULTS", wait_time=0.5)
        time.sleep(1.0)

        # Reset to be sure
        send_command(ser, "CS RESET", wait_time=0.5)
        time.sleep(5.0)

        # Check status again
        response = send_command(ser, "CS STATUS", wait_time=0.5)
        if "fault" in response.lower():
            raise Exception("ChipSHOUTER still faulted after clear/reset!")

        print("ChipSHOUTER faults cleared successfully")
        return True

    return False


def initial_setup(ser):
    """Perform one-time setup."""
    print("Performing initial setup...")

    # Reboot Pico
    try:
        send_command(ser, "REBOOT", wait_time=0.1)
        ser.close()
    except:
        pass

    time.sleep(3.0)

    # Reconnect
    max_retries = 10
    for retry in range(max_retries):
        try:
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
            time.sleep(0.5)
            break
        except (serial.SerialException, OSError):
            if retry < max_retries - 1:
                time.sleep(0.5)
            else:
                raise Exception("Failed to reconnect to Pico after reboot")

    # Check and clear any ChipSHOUTER faults
    print("\nChecking ChipSHOUTER status...")
    check_and_clear_chipshouter_faults(ser)

    # Reset ChipSHOUTER
    print("\nResetting ChipSHOUTER...")
    try:
        send_command(ser, "CS RESET", wait_time=0.5)
        time.sleep(5.0)
    except Exception as e:
        print(f"Warning: ChipSHOUTER reset error: {e}")

    # Set target to LPC
    response = send_command(ser, "TARGET LPC", wait_time=0.2)
    if "OK:" not in response and "Target type set" not in response:
        raise Exception(f"TARGET LPC command failed")

    # Configure UART trigger
    response = send_command(ser, "TRIGGER UART 0d", wait_time=0.2)
    if "OK:" not in response:
        raise Exception(f"TRIGGER UART command failed")

    # Set ChipSHOUTER to hardware trigger high
    ser.write(b"CS TRIGGER HARDWARE HIGH\r\n")
    time.sleep(0.5)

    # Arm ChipSHOUTER
    response = send_command(ser, "CS ARM", wait_time=1.0)
    if "#" not in response:
        raise Exception(f"CS ARM command failed")

    print("Initial setup complete\n")
    return ser


def test_glitch(ser, voltage, pause, width):
    """Test a single set of glitch parameters."""
    # Update ChipSHOUTER voltage
    send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5)

    # Update Pico pause
    send_command(ser, f"SET PAUSE {pause}", wait_time=0.2)

    # Update Pico width
    send_command(ser, f"SET WIDTH {width}", wait_time=0.2)

    # Sync with bootloader
    ser.write(b"TARGET SYNC 115200 12000 10\r\n")
    success, response = wait_for_response(ser, "LPC ISP sync complete", timeout=5.0)
    if not success:
        return "SYNC_FAIL"

    # Arm Pico trigger
    send_command(ser, "ARM ON", wait_time=0.2)

    # Send read command to target
    ser.write(b'TARGET SEND "R 0 516096"\r\n')
    time.sleep(1.0)

    response = ""
    if ser.in_waiting > 0:
        response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

    # Parse result
    if "19" in response:
        return "ERROR19"
    elif "No response data" in response or not response.strip():
        return "NO_RESPONSE"
    elif "0" in response and "19" not in response:
        return "SUCCESS"
    else:
        return "UNKNOWN"


def log_result(voltage, pause, width, result, elapsed_time):
    """Log result to CSV file."""
    file_exists = os.path.exists(RESULTS_FILE)

    with open(RESULTS_FILE, 'a', newline='') as f:
        writer = csv.writer(f)
        if not file_exists:
            writer.writerow(['timestamp', 'voltage', 'pause', 'width', 'result', 'elapsed_time'])

        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        writer.writerow([timestamp, voltage, pause, width, result, f"{elapsed_time:.2f}"])


def run_marathon(duration_hours=12):
    """Run marathon test for specified duration."""

    global RESULTS_FILE

    # Create unique filename with timestamp
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    RESULTS_FILE = f'glitch_results_{timestamp}.csv'

    print("=" * 70)
    print(f"ChipSHOUTER LPC Glitch Marathon Test")
    print(f"Duration: {duration_hours} hours")
    print(f"Results will be logged to: {RESULTS_FILE}")
    print("=" * 70)
    print()

    # Parameter ranges - focusing on most promising areas
    # Based on scope verification: response at ~8000 cycles
    voltage_range = [300, 350, 400, 450, 500]  # Focus on higher voltages
    pause_range = list(range(6500, 8001, 50))   # Fine granularity near response
    width_range = list(range(100, 251, 25))     # Mid-range widths

    # Also test some broader ranges
    pause_range_broad = list(range(0, 8001, 500))

    # Combine for comprehensive coverage
    all_pause_values = sorted(set(pause_range + pause_range_broad))

    print(f"Parameter space:")
    print(f"  Voltage: {len(voltage_range)} values ({min(voltage_range)}-{max(voltage_range)}V)")
    print(f"  Pause: {len(all_pause_values)} values ({min(all_pause_values)}-{max(all_pause_values)} cycles)")
    print(f"  Width: {len(width_range)} values ({min(width_range)}-{max(width_range)} cycles)")
    print(f"  Total combinations: {len(voltage_range) * len(all_pause_values) * len(width_range)}")
    print()

    # Initial setup
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
    time.sleep(0.5)
    ser = initial_setup(ser)

    # Tracking
    start_time = time.time()
    end_time = start_time + (duration_hours * 3600)
    test_count = 0
    success_count = 0
    error19_count = 0
    no_response_count = 0
    unknown_count = 0
    sync_fail_count = 0

    last_print_time = start_time
    last_test_count = 0
    last_fault_check = start_time

    try:
        # Run until time expires
        while time.time() < end_time:
            # Test all combinations
            for voltage in voltage_range:
                for pause in all_pause_values:
                    for width in width_range:
                        if time.time() >= end_time:
                            break

                        test_count += 1
                        test_start = time.time()

                        try:
                            result = test_glitch(ser, voltage, pause, width)

                            # Log to CSV
                            log_result(voltage, pause, width, result, time.time() - test_start)

                            # Track results
                            if result == "SUCCESS":
                                success_count += 1
                                print(f"\n{'='*70}")
                                print(f"SUCCESS FOUND!")
                                print(f"V={voltage}, Pause={pause}, Width={width}")
                                print(f"{'='*70}\n")
                            elif result == "ERROR19":
                                error19_count += 1
                            elif result == "NO_RESPONSE":
                                no_response_count += 1
                                print(f"\nNO_RESPONSE: V={voltage}, Pause={pause}, Width={width}\n")
                            elif result == "SYNC_FAIL":
                                sync_fail_count += 1
                            else:
                                unknown_count += 1
                                print(f"\nUNKNOWN: V={voltage}, Pause={pause}, Width={width}\n")

                        except Exception as e:
                            print(f"\nERROR in test: {e}")
                            sync_fail_count += 1
                            log_result(voltage, pause, width, f"ERROR:{e}", time.time() - test_start)

                        # Print progress every 30 seconds
                        current_time = time.time()
                        if current_time - last_print_time >= 30:
                            elapsed = current_time - start_time
                            remaining = end_time - current_time
                            tests_per_sec = (test_count - last_test_count) / (current_time - last_print_time)

                            print(f"[{elapsed/3600:.1f}h/{duration_hours}h] "
                                  f"Tests: {test_count} ({tests_per_sec:.2f}/s) | "
                                  f"Success: {success_count} | "
                                  f"NO_RESP: {no_response_count} | "
                                  f"ERROR19: {error19_count} | "
                                  f"Remaining: {remaining/3600:.1f}h")

                            last_print_time = current_time
                            last_test_count = test_count

                        # Check for ChipSHOUTER faults every 5 minutes
                        if current_time - last_fault_check >= 300:
                            print("\n[Periodic fault check]")
                            try:
                                was_faulted = check_and_clear_chipshouter_faults(ser)
                                if was_faulted:
                                    print("WARNING: ChipSHOUTER was faulted but has been cleared")
                                    # Re-arm after clearing faults
                                    send_command(ser, "CS ARM", wait_time=1.0)
                            except Exception as e:
                                print(f"Fault check error: {e}")
                            last_fault_check = current_time
                            print()

    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")

    finally:
        # Cleanup
        if ser:
            try:
                ser.close()
            except:
                pass

        # Final summary
        elapsed = time.time() - start_time
        print("\n" + "=" * 70)
        print("Marathon Test Complete")
        print("=" * 70)
        print(f"Duration:        {elapsed/3600:.2f} hours")
        print(f"Total Tests:     {test_count}")
        print(f"Tests/second:    {test_count/elapsed:.2f}")
        print()
        print(f"Results:")
        print(f"  Success:       {success_count} ({success_count/test_count*100 if test_count else 0:.3f}%)")
        print(f"  No Response:   {no_response_count} ({no_response_count/test_count*100 if test_count else 0:.3f}%)")
        print(f"  Error 19:      {error19_count} ({error19_count/test_count*100 if test_count else 0:.3f}%)")
        print(f"  Unknown:       {unknown_count}")
        print(f"  Sync Fail:     {sync_fail_count}")
        print()
        print(f"Results saved to: {RESULTS_FILE}")


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description='Long-running glitch parameter sweep (hours)'
    )
    parser.add_argument(
        '--hours',
        type=float,
        default=12.0,
        help='Duration in hours (default: 12)'
    )

    args = parser.parse_args()

    try:
        run_marathon(duration_hours=args.hours)
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
        sys.exit(0)
    except Exception as e:
        print(f"\nFatal error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
