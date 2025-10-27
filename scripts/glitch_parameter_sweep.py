#!/usr/bin/env python3
"""
ChipSHOUTER LPC Glitch Parameter Sweep Script

Fast parameter exploration without resetting Pico/ChipSHOUTER between tests.
Only voltage, pause, and width are reconfigured between iterations.
"""

import serial
import time
import sys
import argparse

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200
TIMEOUT = 2.0


def send_command(ser, cmd, wait_time=0.2, verbose=False):
    """Send a command to the Pico and return the response."""
    if verbose:
        print(f">>> {cmd}")

    # Clear any pending data first
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)

    ser.write(f"{cmd}\r\n".encode())

    # For ChipSHOUTER commands, poll for response over longer period
    if cmd.startswith("CS "):
        response = ""
        max_wait = 3.0  # Maximum 3 seconds to wait
        start_time = time.time()

        while time.time() - start_time < max_wait:
            time.sleep(0.1)
            if ser.in_waiting > 0:
                chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                response += chunk
                # If we see the ChipSHOUTER prompt, we're done
                if "#" in chunk:
                    break

        if verbose and response.strip():
            print(response.strip())
        return response
    else:
        # Normal command - just wait for the specified time
        time.sleep(wait_time)
        response = ""
        if ser.in_waiting > 0:
            response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            if verbose and response.strip():
                print(response.strip())
        return response


def wait_for_response(ser, expected_text, timeout=5.0, verbose=False):
    """Wait for a specific response text."""
    start_time = time.time()
    full_response = ""

    while time.time() - start_time < timeout:
        if ser.in_waiting > 0:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            full_response += chunk
            if verbose:
                print(chunk, end='', flush=True)

            if expected_text in full_response:
                return True, full_response

        time.sleep(0.01)

    return False, full_response


def initial_setup(ser, verbose=False):
    """Perform one-time setup: reboot Pico, reset ChipSHOUTER, configure target."""
    if verbose:
        print("=" * 60)
        print("Initial Setup")
        print("=" * 60)
        print("\nRebooting Pico...")

    try:
        send_command(ser, "REBOOT", wait_time=0.1, verbose=verbose)
        ser.close()
    except:
        pass

    # Wait for reboot
    time.sleep(3.0)

    # Reconnect
    max_retries = 10
    for retry in range(max_retries):
        try:
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
            time.sleep(0.5)
            if verbose:
                print("Reconnected to Pico")
            break
        except (serial.SerialException, OSError):
            if retry < max_retries - 1:
                time.sleep(0.5)
            else:
                raise Exception("Failed to reconnect to Pico after reboot")

    # Reset ChipSHOUTER
    if verbose:
        print("Resetting ChipSHOUTER...")

    try:
        response = send_command(ser, "CS RESET", wait_time=0.5, verbose=verbose)
        time.sleep(5.0)
        if verbose:
            print("ChipSHOUTER reset complete")
    except Exception as e:
        if verbose:
            print(f"Warning: ChipSHOUTER reset error: {e}")

    # Set target to LPC
    if verbose:
        print("\nSetting up LPC target...")

    response = send_command(ser, "TARGET LPC", wait_time=0.2, verbose=verbose)
    if "OK:" not in response and "Target type set" not in response:
        raise Exception(f"TARGET LPC command failed - response: {response}")

    # Configure UART trigger (only needs to be set once)
    response = send_command(ser, "TRIGGER UART 0d", wait_time=0.2, verbose=verbose)
    if "OK:" not in response:
        raise Exception(f"TRIGGER UART command failed - response: {response}")

    # Set ChipSHOUTER to hardware trigger high (only needs to be set once)
    ser.write(b"CS TRIGGER HARDWARE HIGH\r\n")
    if verbose:
        print(">>> CS TRIGGER HARDWARE HIGH")
    time.sleep(0.5)

    # Arm ChipSHOUTER (stays armed for all tests)
    response = send_command(ser, "CS ARM", wait_time=1.0, verbose=verbose)
    if "#" not in response:
        raise Exception(f"CS ARM command failed - no ChipSHOUTER prompt in response: '{response}'")

    if "Error: Change System State not allowed" in response or "fault] CMD-> arm" in response:
        if verbose:
            print("ChipSHOUTER already armed")
    elif "# arm" in response or "# arming" in response:
        if verbose:
            print("ChipSHOUTER armed successfully")
    else:
        raise Exception(f"Failed to arm ChipSHOUTER - unexpected response: '{response}'")

    if verbose:
        print("\nInitial setup complete\n")

    return ser


def test_glitch_parameters(ser, voltage, pause, width, verbose=False):
    """Test a single set of glitch parameters."""
    # Update ChipSHOUTER voltage
    response = send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5, verbose=verbose)
    if "#" not in response:
        raise Exception(f"CS VOLTAGE command failed - no ChipSHOUTER prompt in response: '{response}'")

    # Update Pico pause
    response = send_command(ser, f"SET PAUSE {pause}", wait_time=0.2, verbose=verbose)
    if "OK:" not in response:
        raise Exception(f"SET PAUSE command failed - response: {response}")

    # Update Pico width
    response = send_command(ser, f"SET WIDTH {width}", wait_time=0.2, verbose=verbose)
    if "OK:" not in response:
        raise Exception(f"SET WIDTH command failed - response: {response}")

    # Sync with bootloader
    ser.write(b"TARGET SYNC 115200 12000 10\r\n")
    success, response = wait_for_response(ser, "LPC ISP sync complete", timeout=5.0, verbose=verbose)
    if not success:
        raise Exception("Failed to sync with LPC bootloader")

    # Arm Pico trigger
    response = send_command(ser, "ARM ON", wait_time=0.2, verbose=verbose)
    if "OK:" not in response and "armed" not in response.lower():
        raise Exception(f"ARM ON command failed - response: {response}")

    # Send read command to target (triggers glitch)
    if verbose:
        print("Sending target command: R 0 516096")
    ser.write(b'TARGET SEND "R 0 516096"\r\n')

    # Wait for response
    time.sleep(1.0)

    response = ""
    if ser.in_waiting > 0:
        response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
        if verbose:
            print(response)

    # Parse result
    if "19" in response:
        return "ERROR19"
    elif "No response data" in response or not response.strip():
        return "NO_RESPONSE"
    elif "0" in response and "19" not in response:
        return "SUCCESS"
    else:
        return "UNKNOWN"


def run_parameter_sweep(voltage_range, pause_range, width_range, verbose=True):
    """Run parameter sweep across all combinations."""
    print("ChipSHOUTER LPC Glitch Parameter Sweep")
    print("=" * 60)

    # Initial setup (only once)
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
    time.sleep(0.5)
    ser = initial_setup(ser, verbose=verbose)

    # Track results
    results = []
    start_time = time.time()

    # Test each parameter combination
    test_num = 0
    total_tests = len(voltage_range) * len(pause_range) * len(width_range)

    for voltage in voltage_range:
        for pause in pause_range:
            for width in width_range:
                test_num += 1
                print(f"\n[{test_num}/{total_tests}] Testing V={voltage}, Pause={pause}, Width={width}")
                print("-" * 60)

                try:
                    result = test_glitch_parameters(ser, voltage, pause, width, verbose=verbose)

                    # Print result
                    if result == "SUCCESS":
                        print(f"✓ SUCCESS - V={voltage}, Pause={pause}, Width={width}")
                    elif result == "ERROR19":
                        print(f"✗ ERROR19 - V={voltage}, Pause={pause}, Width={width}")
                    elif result == "NO_RESPONSE":
                        print(f"✗ NO_RESPONSE (crash) - V={voltage}, Pause={pause}, Width={width}")
                    else:
                        print(f"? UNKNOWN - V={voltage}, Pause={pause}, Width={width}")

                    results.append({
                        'voltage': voltage,
                        'pause': pause,
                        'width': width,
                        'result': result
                    })

                except Exception as e:
                    print(f"✗ ERROR: {e}")
                    results.append({
                        'voltage': voltage,
                        'pause': pause,
                        'width': width,
                        'result': 'ERROR'
                    })

                # Small delay between tests
                time.sleep(0.3)

    # Close serial
    if ser:
        try:
            ser.close()
        except:
            pass

    # Print summary
    end_time = time.time()
    total_time = end_time - start_time

    print("\n" + "=" * 60)
    print("Parameter Sweep Summary")
    print("=" * 60)

    success_count = sum(1 for r in results if r['result'] == 'SUCCESS')
    error19_count = sum(1 for r in results if r['result'] == 'ERROR19')
    no_response_count = sum(1 for r in results if r['result'] == 'NO_RESPONSE')
    unknown_count = sum(1 for r in results if r['result'] == 'UNKNOWN')
    error_count = sum(1 for r in results if r['result'] == 'ERROR')

    print(f"Total Tests:     {len(results)}")
    print(f"Success:         {success_count}")
    print(f"Error 19:        {error19_count}")
    print(f"No Response:     {no_response_count}")
    print(f"Unknown:         {unknown_count}")
    print(f"Errors:          {error_count}")
    print(f"\nTotal Time:      {total_time:.2f}s")
    print(f"Time per Test:   {total_time/len(results):.2f}s")

    # Print successful parameters
    if success_count > 0:
        print("\n" + "=" * 60)
        print("Successful Parameters:")
        print("=" * 60)
        for r in results:
            if r['result'] == 'SUCCESS':
                print(f"V={r['voltage']}, Pause={r['pause']}, Width={r['width']}")

    # Print crash parameters
    if no_response_count > 0:
        print("\n" + "=" * 60)
        print("Crash Parameters (No Response):")
        print("=" * 60)
        for r in results:
            if r['result'] == 'NO_RESPONSE':
                print(f"V={r['voltage']}, Pause={r['pause']}, Width={r['width']}")


def main():
    parser = argparse.ArgumentParser(
        description='Sweep ChipSHOUTER glitch parameters for LPC bootloader'
    )
    parser.add_argument(
        '--voltage-start',
        type=int,
        default=150,
        help='Starting voltage (default: 150)'
    )
    parser.add_argument(
        '--voltage-end',
        type=int,
        default=500,
        help='Ending voltage (default: 500)'
    )
    parser.add_argument(
        '--voltage-step',
        type=int,
        default=50,
        help='Voltage step (default: 50)'
    )
    parser.add_argument(
        '--pause-start',
        type=int,
        default=0,
        help='Starting pause in cycles (default: 0)'
    )
    parser.add_argument(
        '--pause-end',
        type=int,
        default=2000,
        help='Ending pause in cycles (default: 2000)'
    )
    parser.add_argument(
        '--pause-step',
        type=int,
        default=200,
        help='Pause step in cycles (default: 200)'
    )
    parser.add_argument(
        '--width-start',
        type=int,
        default=100,
        help='Starting width in cycles (default: 100)'
    )
    parser.add_argument(
        '--width-end',
        type=int,
        default=1000,
        help='Ending width in cycles (default: 1000)'
    )
    parser.add_argument(
        '--width-step',
        type=int,
        default=100,
        help='Width step in cycles (default: 100)'
    )
    parser.add_argument(
        '-q', '--quiet',
        action='store_true',
        help='Suppress verbose output'
    )

    args = parser.parse_args()

    # Generate ranges
    voltage_range = range(args.voltage_start, args.voltage_end + 1, args.voltage_step)
    pause_range = range(args.pause_start, args.pause_end + 1, args.pause_step)
    width_range = range(args.width_start, args.width_end + 1, args.width_step)

    try:
        run_parameter_sweep(
            voltage_range,
            pause_range,
            width_range,
            verbose=not args.quiet
        )
    except KeyboardInterrupt:
        print("\n\nSweep interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"\nFatal error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
