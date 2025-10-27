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
6. Arms ChipSHOUTER and Pico
7. Sends read command "R 0 516096"
8. Checks result: "19" = fail, "0" = success
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

    ser.write(f"{cmd}\r\n".encode())
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
    """Reboot the Pico and wait for it to come back online."""
    if verbose:
        print("Rebooting Pico...")

    try:
        send_command(ser, "REBOOT", wait_time=0.1, verbose=verbose)
        ser.close()
    except:
        pass

    # Wait for reboot to complete
    time.sleep(3.0)

    # Reconnect
    max_retries = 10
    for retry in range(max_retries):
        try:
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
            time.sleep(0.5)
            if verbose:
                print("Reconnected to Pico")
            return ser
        except (serial.SerialException, OSError):
            if retry < max_retries - 1:
                time.sleep(0.5)
            else:
                raise Exception("Failed to reconnect to Pico after reboot")

    return None


def setup_lpc_target(ser, verbose=False):
    """Set target to LPC and perform sync."""
    if verbose:
        print("\nSetting up LPC target...")

    # Set target type
    send_command(ser, "TARGET LPC", wait_time=0.2, verbose=verbose)

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


def configure_glitch(ser, verbose=False):
    """Configure trigger and ChipSHOUTER settings."""
    if verbose:
        print("\nConfiguring glitch parameters...")

    # Set UART trigger to 0x0d
    send_command(ser, "TRIGGER UART 0d", wait_time=0.2, verbose=verbose)

    # Set ChipSHOUTER to hardware trigger high
    send_command(ser, "CS TRIGGER HARDWARE HIGH", wait_time=0.2, verbose=verbose)


def perform_glitch_test(ser, verbose=False):
    """Perform the actual glitch test."""
    if verbose:
        print("\nPerforming glitch test...")

    # Arm ChipSHOUTER
    send_command(ser, "CS ARM", wait_time=0.2, verbose=verbose)

    # Arm Pico trigger
    send_command(ser, "ARM ON", wait_time=0.2, verbose=verbose)

    # Send read command to target (this will trigger the glitch)
    if verbose:
        print("Sending target command: R 0 516096")

    ser.write(b"TARGET SEND R 0 516096\r\n")

    # Wait for response
    time.sleep(1.0)

    response = ""
    if ser.in_waiting > 0:
        response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
        if verbose:
            print(response)

    # Parse the response to determine glitch result
    # Response "19" means glitch failed (normal operation)
    # Response "0" means glitch succeeded (security bypass)

    if "19" in response:
        return "FAILED"
    elif "0" in response and "19" not in response:
        return "SUCCESS"
    else:
        return "UNKNOWN"


def run_glitch_test(num_iterations=1, verbose=True):
    """Run the complete glitch test sequence."""

    success_count = 0
    fail_count = 0
    unknown_count = 0

    print(f"ChipSHOUTER LPC Bootloader Glitch Test")
    print("=" * 60)

    for iteration in range(num_iterations):
        if num_iterations > 1:
            print(f"\nIteration {iteration + 1}/{num_iterations}")
            print("-" * 60)

        try:
            # Connect to Pico
            ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
            time.sleep(0.5)

            # Reboot Pico for clean state
            ser = reboot_pico(ser, verbose=verbose)

            # Setup LPC target and sync
            setup_lpc_target(ser, verbose=verbose)

            # Configure glitch parameters
            configure_glitch(ser, verbose=verbose)

            # Perform glitch test
            result = perform_glitch_test(ser, verbose=verbose)

            # Report result
            if result == "SUCCESS":
                print("\n✓ GLITCH SUCCESS - Security bypass detected!")
                success_count += 1
            elif result == "FAILED":
                print("\n✗ GLITCH FAILED - Normal operation (response: 19)")
                fail_count += 1
            else:
                print("\n? GLITCH UNKNOWN - Unexpected response")
                unknown_count += 1

            ser.close()

        except Exception as e:
            print(f"\n✗ ERROR: {e}")
            fail_count += 1
            try:
                ser.close()
            except:
                pass

        # Small delay between iterations
        if iteration < num_iterations - 1:
            time.sleep(1.0)

    # Print summary
    if num_iterations > 1:
        print("\n" + "=" * 60)
        print("Test Summary:")
        print(f"  Glitch Success: {success_count}/{num_iterations}")
        print(f"  Glitch Failed:  {fail_count}/{num_iterations}")
        print(f"  Unknown:        {unknown_count}/{num_iterations}")
        print(f"  Success Rate:   {(success_count / num_iterations) * 100:.1f}%")


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
        '-q', '--quiet',
        action='store_true',
        help='Suppress verbose output'
    )

    args = parser.parse_args()

    try:
        run_glitch_test(
            num_iterations=args.num_iterations,
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
