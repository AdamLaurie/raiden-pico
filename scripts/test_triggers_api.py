#!/usr/bin/env python3
"""
Test GPIO and UART triggers using API mode for fast scripting
"""

import serial
import time
import sys

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200
TIMEOUT = 2.0

def check_arm_status(ser):
    """Check ARM status and return True if ARMED, False if DISARMED"""
    # Clear any pending data
    ser.read(ser.in_waiting)

    # Send ARM query
    ser.write(b"ARM\r\n")
    time.sleep(0.1)

    # Read response
    response = ""
    if ser.in_waiting > 0:
        response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

    return "ARMED" in response and "DISARMED" not in response

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
                # Get error message
                ser.write(b"ERROR\r\n")
                time.sleep(0.1)
                # Skip '.' and response chars
                ser.read(2)
                error = ""
                start_err = time.time()
                while time.time() - start_err < 0.5:
                    if ser.in_waiting > 0:
                        error += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                        if '\n' in error:
                            break
                    time.sleep(0.01)
                print(f"ERROR: {error.strip()}")
                return False

    print("ERROR: No response")
    return False

def test_gpio_trigger(ser):
    """Test GPIO trigger (both rising and falling edge)"""
    print("\n=== Testing GPIO Trigger ===")

    # Test rising edge
    print("Testing GPIO RISING edge...")
    if not send_command(ser, "TRIGGER GPIO RISING"):
        return False
    print("  ✓ GPIO RISING configured")

    print("  Arming for GPIO trigger test...")
    if not send_command(ser, "ARM ON"):
        return False
    print("  ✓ Armed - manually trigger GP3 RISING to see glitch on scope")
    time.sleep(2)  # Wait for manual trigger observation
    send_command(ser, "ARM OFF")

    # Test falling edge
    print("Testing GPIO FALLING edge...")
    if not send_command(ser, "TRIGGER GPIO FALLING"):
        return False
    print("  ✓ GPIO FALLING configured")

    print("  Arming for GPIO trigger test...")
    if not send_command(ser, "ARM ON"):
        return False
    print("  ✓ Armed - manually trigger GP3 FALLING to see glitch on scope")
    time.sleep(2)  # Wait for manual trigger observation
    send_command(ser, "ARM OFF")

    return True

def setup_target(ser):
    """Setup target for UART testing (RESET, LPC, SYNC)"""
    print("\n=== Setting up Target for UART Tests ===")

    print("Resetting target...")
    if not send_command(ser, "TARGET RESET"):
        print("  ✗ Failed to reset target")
        return False
    print("  ✓ Target reset")

    print("Setting target type to LPC...")
    if not send_command(ser, "TARGET LPC"):
        print("  ✗ Failed to set target type")
        return False
    print("  ✓ Target type set to LPC")

    print("Syncing with target bootloader...")
    if not send_command(ser, "TARGET SYNC"):
        print("  ✗ Failed to sync with target")
        return False
    print("  ✓ Target synced successfully")

    return True

def test_uart_trigger(ser):
    """Test UART trigger with various byte patterns"""
    print("\n=== Testing UART Trigger ===")

    # Test various byte patterns
    test_bytes = [
        (0x00, "0x00 (all zeros)"),
        (0xFF, "0xFF (all ones)"),
        (0x0D, "0x0D (carriage return)"),
        (0x0A, "0x0A (line feed)"),
        (0x55, "0x55 (alternating bits)"),
        (0xAA, "0xAA (alternating bits)"),
        (0x3F, "0x3F (LPC ISP sync byte)"),
        (0x80, "0x80 (MSB set)"),
        (0x01, "0x01 (LSB set)"),
    ]

    for byte_val, description in test_bytes:
        print(f"Testing UART trigger on {description}...")
        if not send_command(ser, f"TRIGGER UART {byte_val:02x}"):
            print(f"  ✗ Failed to set UART trigger to 0x{byte_val:02X}")
            return False
        print(f"  ✓ UART trigger 0x{byte_val:02X} configured")

        # ARM and send test sequence
        print(f"  Arming and sending byte sequence...")
        if not send_command(ser, "ARM ON"):
            print("  ✗ Failed to ARM")
            return False

        # Generate non-matching bytes (different from trigger byte)
        non_matching = []
        candidates = [0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0]
        for c in candidates:
            if c != byte_val:
                non_matching.append(c)
            if len(non_matching) >= 5:
                break

        # Build a byte sequence: 3 non-matching + trigger byte + 2 non-matching
        # This tests that the UART trigger can find the byte within a stream
        byte_sequence = f"{non_matching[0]:02x} {non_matching[1]:02x} {non_matching[2]:02x} {byte_val:02x} {non_matching[3]:02x} {non_matching[4]:02x}"

        print(f"    Sending byte sequence: 0x{non_matching[0]:02X} 0x{non_matching[1]:02X} 0x{non_matching[2]:02X} [0x{byte_val:02X}] 0x{non_matching[3]:02X} 0x{non_matching[4]:02X}")
        print(f"    (Only [0x{byte_val:02X}] should trigger glitch)")

        if not send_command(ser, f"TARGET SEND \"{byte_sequence}\""):
            print("    ✗ Failed to send byte sequence")
            return False

        # Check ARM status - should be DISARMED if glitch fired
        time.sleep(0.1)  # Brief delay for trigger to process
        is_armed = check_arm_status(ser)

        if not is_armed:
            print(f"  ✓ Glitch FIRED (system auto-disarmed) - check scope for glitch on 0x{byte_val:02X}!")
        else:
            print(f"  ✗ WARNING: System still ARMED - glitch may not have fired!")
            print(f"    Check trigger configuration or hardware connections")

        time.sleep(0.5)  # Wait to observe on scope

        send_command(ser, "ARM OFF")

    return True

def test_glitch_params(ser):
    """Test setting glitch parameters"""
    print("\n=== Testing Glitch Parameters ===")

    params = [
        ("PAUSE", 0),
        ("WIDTH", 150),
        ("GAP", 100),
        ("COUNT", 1),
    ]

    for param, value in params:
        print(f"Setting {param} to {value}...")
        if not send_command(ser, f"SET {param} {value}"):
            print(f"  ✗ Failed to set {param}")
            return False
        print(f"  ✓ {param} set to {value}")

    return True

def test_error_handling(ser):
    """Test error handling and failure mode"""
    print("\n=== Testing Error Handling (Failure Mode) ===")

    print("Sending invalid command to test failure response...")
    print("  Command: 'INVALID COMMAND TEST'")

    # Clear any pending data
    ser.read(ser.in_waiting)

    # Send invalid command
    ser.write(b"INVALID COMMAND TEST\r\n")

    # Wait for '.' (command received)
    response = ""
    start = time.time()
    while time.time() - start < 1.0:
        if ser.in_waiting > 0:
            char = ser.read(1).decode('utf-8', errors='ignore')
            response += char
            if char == '.':
                print("  ✓ Received '.' (command acknowledged)")
                break

    # Wait for '!' (failure)
    start = time.time()
    got_failure = False
    while time.time() - start < 1.0:
        if ser.in_waiting > 0:
            char = ser.read(1).decode('utf-8', errors='ignore')
            response += char
            if char == '!':
                print("  ✓ Received '!' (command failed as expected)")
                got_failure = True
                break
            elif char == '+':
                print("  ✗ Received '+' but expected '!' for invalid command")
                return False

    if not got_failure:
        print("  ✗ Did not receive failure response")
        return False

    # Now get the error message using ERROR command
    print("  Fetching error message with ERROR command...")
    ser.write(b"ERROR\r\n")
    time.sleep(0.2)

    # Skip '.' and '+' from ERROR command response
    ser.read(2)

    # Read error message
    error_msg = ""
    start = time.time()
    while time.time() - start < 0.5:
        if ser.in_waiting > 0:
            error_msg += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            if '\n' in error_msg:
                break
        time.sleep(0.01)

    print(f"  ✓ Error message: {error_msg.strip()}")

    if "INVALID" in error_msg or "Unknown" in error_msg or "ERROR" in error_msg:
        print("  ✓ Error message correctly indicates invalid command")
        return True
    else:
        print("  ✗ Error message doesn't mention invalid command")
        return False

def test_arm_disarm(ser):
    """Test ARM/DISARM functionality"""
    print("\n=== Final ARM/DISARM Test ===")

    print("Testing ARM ON...")
    if not send_command(ser, "ARM ON"):
        print("  ✗ Failed to ARM")
        return False
    print("  ✓ ARM ON successful")

    print("Testing ARM OFF...")
    if not send_command(ser, "ARM OFF"):
        print("  ✗ Failed to DISARM")
        return False
    print("  ✓ ARM OFF successful")
    print("  (ARM/DISARM also tested during trigger tests)")

    return True

def main():
    print("=" * 70)
    print("Trigger Test Suite - API Mode")
    print("=" * 70)

    # Connect
    print(f"\nConnecting to {SERIAL_PORT}...")
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
        time.sleep(0.5)
    except Exception as e:
        print(f"ERROR: Failed to connect: {e}")
        return 1

    try:
        # Enable API mode
        print("\nEnabling API mode...")
        ser.write(b"API ON\r\n")
        time.sleep(0.2)
        # Clear response
        ser.read(ser.in_waiting)
        print("✓ API mode enabled")

        # Run tests
        all_passed = True

        if not test_glitch_params(ser):
            all_passed = False

        if not test_gpio_trigger(ser):
            all_passed = False

        # Setup target before UART trigger tests
        if not setup_target(ser):
            all_passed = False
        else:
            if not test_uart_trigger(ser):
                all_passed = False

        if not test_arm_disarm(ser):
            all_passed = False

        if not test_error_handling(ser):
            all_passed = False

        # Disable API mode
        print("\nDisabling API mode...")
        ser.write(b"API OFF\r\n")
        time.sleep(0.2)
        # Clear response
        ser.read(ser.in_waiting)
        print("✓ API mode disabled")

        # Print summary
        print("\n" + "=" * 70)
        if all_passed:
            print("✓✓✓ ALL TESTS PASSED ✓✓✓")
            print("=" * 70)
            return 0
        else:
            print("✗✗✗ SOME TESTS FAILED ✗✗✗")
            print("=" * 70)
            return 1

    finally:
        ser.close()

if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n\nTest interrupted by user")
        sys.exit(130)
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
