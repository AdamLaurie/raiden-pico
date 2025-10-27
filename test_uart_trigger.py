#!/usr/bin/env python3
"""
Test script for UART-triggered glitching with proper TTY management.
Always closes serial port properly to prevent hanging.
"""

import serial
import time
import sys

def main():
    # Configuration
    PORT = '/dev/ttyACM0'
    BAUD = 115200

    try:
        # Open serial port with context manager for automatic cleanup
        with serial.Serial(PORT, BAUD, timeout=1) as ser:
            time.sleep(1)

            print("Setting up LPC target...")
            setup_commands = [
                'TARGET LPC',            # Set chip type to LPC
                'TARGET RESET PIN 15 PERIOD 100', # Configure reset: GP15, 100ms pulse, active low (default)
                'TARGET DEBUG OFF',      # Disable debug for cleaner output
            ]

            for cmd in setup_commands:
                print(f'> {cmd}')
                ser.write((cmd + '\r\n').encode())
                time.sleep(0.15)
                while ser.in_waiting:
                    response = ser.readline().decode('utf-8', errors='ignore').strip()
                    if response and response != '>':
                        print(f'  {response}')

            print("\nResetting target and entering bootloader...")
            ser.write(b'TARGET SYNC 115200 12000 500\r\n')  # Use SYNC which does reset + bootloader
            time.sleep(3.0)  # Wait for reset and sync
            while ser.in_waiting:
                response = ser.readline().decode('utf-8', errors='ignore').strip()
                if response and response != '>':
                    print(f'  {response}')

            print("\nConfiguring glitch parameters...")
            glitch_commands = [
                'ARM OFF',               # Ensure system is disarmed
                'SET PAUSE 40',          # 40us delay from trigger to glitch
                'SET WIDTH 10',          # 10us pulse width
                'SET COUNT 1',           # Single pulse
                'TRIGGER UART 0x0D',     # Trigger on \r (carriage return)
                'ARM ON',                # Arm the system
            ]

            for cmd in glitch_commands:
                print(f'> {cmd}')
                ser.write((cmd + '\r\n').encode())
                time.sleep(0.15)
                while ser.in_waiting:
                    response = ser.readline().decode('utf-8', errors='ignore').strip()
                    if response and response != '>':
                        print(f'  {response}')

            print("\nSending read command to trigger glitch...")
            ser.write(b'TARGET SEND R 0 516096\r\n')
            time.sleep(0.5)

            # Read all responses
            while ser.in_waiting:
                response = ser.readline().decode('utf-8', errors='ignore').strip()
                if response and response != '>':
                    print(f'  {response}')

            # Check glitch count
            print("\nChecking glitch count...")
            ser.write(b'GET COUNT\r\n')
            time.sleep(0.1)
            while ser.in_waiting:
                response = ser.readline().decode('utf-8', errors='ignore').strip()
                if response and 'COUNT' not in response and response != '>':
                    print(f'Glitch count: {response}')

        # Serial port automatically closed by context manager
        print("\n[Serial port closed properly]")
        print("SUCCESS: UART trigger test completed")
        return 0

    except serial.SerialException as e:
        print(f"Serial port error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        return 130
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
