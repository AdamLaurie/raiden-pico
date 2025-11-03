#!/usr/bin/env python3
"""
Test glitching on reset with JTAG connectivity verification

Uses hardware trigger (GPIO rising edge) to glitch during LPC reset and
verifies the chip is still visible to OpenOCD via JTAG. This ensures glitch
parameters don't brick the device.

Usage:
    ./test_glitch_jtag_verify.py [iterations]
"""

import subprocess
import serial
import time
import sys

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200

# Glitch parameters (adjust these)
GLITCH_VOLTAGE = 300  # V
GLITCH_POSITION = 0   # μs delay from trigger
GLITCH_WIDTH = 150    # ns

def send_command(ser, cmd):
    """Send command and wait briefly"""
    ser.write(f"{cmd}\r\n".encode())
    time.sleep(0.05)
    ser.read(ser.in_waiting)

def wait_for_cs_armed(ser, timeout=5.0):
    """Poll ChipSHOUTER status until armed"""
    start_time = time.time()
    while time.time() - start_time < timeout:
        ser.read(ser.in_waiting)  # Clear buffer
        ser.write(b"CS STATUS\r\n")
        time.sleep(0.1)

        response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
        if 'State: ARMED' in response or 'ARMED' in response:
            return True
        time.sleep(0.2)
    return False

def check_jtag_connectivity():
    """Check if LPC is visible via JTAG using OpenOCD"""
    cmd = [
        "openocd",
        "-f", "/usr/share/openocd/scripts/interface/jlink.cfg",
        "-f", "/usr/share/openocd/scripts/target/lpc2478.cfg",
        "-c", "init",
        "-c", "exit"
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=5)
        output = result.stdout + result.stderr

        # Check for successful JTAG tap detection
        if "JTAG tap: lpc2478.cpu tap/device found: 0x4f1f0f0f" in output:
            return True, "JTAG OK"
        elif "Error" in output or "not found" in output:
            return False, "JTAG FAIL"
        else:
            return False, "UNKNOWN"
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT"
    except Exception as e:
        return False, f"ERROR: {e}"

def try_openocd_dump(halt_method, output_file):
    """Try dumping flash with a specific halt method"""
    cmd = [
        "openocd",
        "-f", "/usr/share/openocd/scripts/interface/jlink.cfg",
        "-f", "/usr/share/openocd/scripts/target/lpc2478.cfg",
        "-c", "init",
        "-c", halt_method,
        "-c", f"dump_image {output_file} 0x0 0x7E000",  # 504KB flash
        "-c", "exit"
    ]

    try:
        print("Starting OpenOCD dump...")
        # Use Popen to monitor output in real-time
        import time as t
        process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        output = ""
        start_time = t.time()
        halted = False
        failed = False

        # Monitor output for halt status
        while process.poll() is None:
            line = process.stdout.readline()
            if line:
                output += line
                if "target halted" in line.lower():
                    halted = True
                    print("✓ Target halted, waiting for dump to complete (~30 seconds)...")
                elif "Failed to halt CPU" in line:
                    failed = True
                    print("✗ Failed to halt CPU, aborting...")
                    process.terminate()
                    break

            # Overall timeout
            if t.time() - start_time > 90:
                process.terminate()
                break

        # Get any remaining output
        remaining = process.stdout.read()
        if remaining:
            output += remaining

        # IMPORTANT: Check for success FIRST, before checking for errors
        # OpenOCD may output both "dumped X bytes" AND error messages
        # If dump succeeded, the file will be created regardless of warnings

        # Check if dump was successful (look for "dumped X bytes")
        import re
        match = re.search(r'dumped (\d+) bytes', output)
        if match:
            byte_count = int(match.group(1))
            print(f"✓ Successfully dumped {byte_count} bytes to {output_file}")
            return True, "success"

        # No "dumped X bytes" found - check why it failed
        if "Failed to halt CPU" in output:
            print(f"✗ Flash dump failed: Failed to halt CPU")
            return False, "halt_failed"
        elif "timed out" in output.lower() or "halt timed out" in output.lower():
            print(f"✗ Flash dump failed: Target halt timeout")
            return False, "halt_timeout"
        elif "error" in output.lower() and "dumped" not in output:
            print(f"✗ Flash dump failed: OpenOCD error")
            return False, "error"
        else:
            print(f"✗ Flash dump failed: Unknown reason")
            return False, "unknown"
    except subprocess.TimeoutExpired:
        print("✗ Flash dump timeout")
        return False, "timeout"
    except Exception as e:
        print(f"✗ Flash dump error: {e}")
        return False, "error"

def dump_flash_via_jtag(ser, output_file):
    """Dump full flash memory via JTAG using OpenOCD - tries multiple halt methods"""
    print(f"\nAttempting to dump flash via JTAG to {output_file}...")

    # Try different halt methods in sequence
    halt_methods = [
        ("soft_reset_halt", "Soft reset + halt", 500),
        ("halt", "Direct halt", 1000),
        ("reset init", "Reset init", 1000),
    ]

    for idx, (method, description, reset_duration) in enumerate(halt_methods):
        if idx > 0:
            print(f"\nTrying alternative method #{idx+1}: {description}")

        # Power cycle target via long reset before each attempt
        print(f"Power cycling target (reset for {reset_duration}ms)...")
        send_command(ser, f"TARGET RESET {reset_duration}")
        time.sleep((reset_duration / 1000.0) + 0.2)  # Wait for reset + margin

        # Try the dump
        print(f"Using OpenOCD command: {method}")
        success, result = try_openocd_dump(method, output_file)

        if success:
            return True

        # If we got "Failed to halt CPU", try next method
        if result == "halt_failed":
            print(f"  -> Method failed, will try next method...")
            continue
        else:
            # Other errors (timeout, etc) - also try next method
            print(f"  -> Error: {result}, trying next method...")
            continue

    # All methods failed
    print("✗ All halt methods failed")
    return False

def execute_glitch_test(ser, iteration):
    """
    Execute one glitch test iteration

    Returns:
        tuple: (jtag_ok, dump_success)
        - jtag_ok: True if JTAG connectivity maintained
        - dump_success: True if flash dump succeeded (stop testing)
    """
    # Power on ARM target
    send_command(ser, "ARM ON")

    # Execute target reset - this triggers glitch via GPIO rising edge
    send_command(ser, "TARGET RESET")

    # Wait for glitch to complete and target to stabilize
    time.sleep(0.5)

    # Check JTAG connectivity
    jtag_ok, status = check_jtag_connectivity()

    result = "✓ PASS" if jtag_ok else "✗ FAIL"
    print(f"[{iteration:4d}] V={GLITCH_VOLTAGE}V P={GLITCH_POSITION}μs W={GLITCH_WIDTH}ns -> {result} ({status})")

    # If JTAG is accessible, try to dump flash
    if jtag_ok:
        from datetime import datetime
        timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
        dump_file = f"jtag_flash_dump_V{GLITCH_VOLTAGE}_P{GLITCH_POSITION}_W{GLITCH_WIDTH}_{timestamp}.bin"

        if dump_flash_via_jtag(ser, dump_file):
            print(f"\n✓✓✓ SUCCESS! Flash dumped successfully!")
            print(f"✓ File: {dump_file}")
            print(f"✓ Parameters: V={GLITCH_VOLTAGE}V, P={GLITCH_POSITION}μs, W={GLITCH_WIDTH}ns")
            return jtag_ok, True

    return jtag_ok, False

def main():
    # Parse arguments
    iterations = int(sys.argv[1]) if len(sys.argv) > 1 else 100

    print("="*70)
    print("Glitch + JTAG Verification Test")
    print("="*70)
    print(f"Glitch parameters: V={GLITCH_VOLTAGE}V, P={GLITCH_POSITION}μs, W={GLITCH_WIDTH}ns")
    print(f"Iterations: {iterations}")
    print("="*70)
    print()

    # Open Pico2 serial port
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    except serial.SerialException as e:
        print(f"✗ Failed to open {SERIAL_PORT}: {e}")
        return 1

    # Clear any buffered data
    time.sleep(0.1)
    ser.read(ser.in_waiting)

    # Configure ChipSHOUTER
    print("Configuring ChipSHOUTER...")
    send_command(ser, "CS RESET")

    # Wait for CS RESET to complete
    print("Waiting for ChipSHOUTER reset...")
    time.sleep(0.5)

    send_command(ser, f"CS VOLTAGE {GLITCH_VOLTAGE}")
    send_command(ser, f"CS POS {GLITCH_POSITION}")
    send_command(ser, f"CS WIDTH {GLITCH_WIDTH}")
    send_command(ser, "CS TRIGGER HARDWARE HIGH")
    send_command(ser, "TRIGGER GPIO RISING")
    send_command(ser, "CS ARM")

    # Wait for ChipSHOUTER to arm
    print("Waiting for ChipSHOUTER to arm...")
    if wait_for_cs_armed(ser, timeout=5.0):
        print("✓ ChipSHOUTER armed and ready\n")
    else:
        print("WARNING: ChipSHOUTER may not be armed (timeout)\n")

    # Run test iterations
    pass_count = 0
    fail_count = 0
    dump_success = False
    start_time = time.time()

    for i in range(1, iterations + 1):
        try:
            jtag_ok, dump_ok = execute_glitch_test(ser, i)
            if jtag_ok:
                pass_count += 1
            else:
                fail_count += 1

            # Stop if we successfully dumped flash
            if dump_ok:
                dump_success = True
                print(f"\n✓ Stopping test - flash dump successful!")
                break

        except KeyboardInterrupt:
            print("\n\nTest interrupted by user")
            break
        except Exception as e:
            print(f"✗ Error in iteration {i}: {e}")
            fail_count += 1

    elapsed_time = time.time() - start_time

    ser.close()

    # Print summary
    print("\n" + "="*70)
    print("TEST SUMMARY")
    print("="*70)
    total_tests = pass_count + fail_count
    if total_tests > 0:
        print(f"Total iterations: {total_tests}")
        print(f"JTAG OK:          {pass_count} ({100*pass_count/total_tests:.1f}%)")
        print(f"JTAG FAIL:        {fail_count} ({100*fail_count/total_tests:.1f}%)")
        print(f"Elapsed time:     {elapsed_time:.1f}s ({elapsed_time/total_tests:.2f}s per test)")
    print("="*70)

    if dump_success:
        print("\n✓✓✓ SUCCESS! Flash memory dumped via JTAG!")
        print("CRP bypass may have been achieved")
        return 0
    elif fail_count > 0:
        print("\n⚠ WARNING: Some glitches caused JTAG connectivity loss!")
        print("   Consider adjusting glitch parameters (lower voltage, shorter width)")
        return 1
    else:
        print("\n✓ All JTAG checks passed, but flash dump did not succeed")
        print("   CRP protection may still be active")
        return 0

if __name__ == "__main__":
    sys.exit(main())
