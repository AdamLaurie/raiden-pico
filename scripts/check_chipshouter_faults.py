#!/usr/bin/env python3
"""Check and clear ChipSHOUTER faults."""

import serial
import time

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200
TIMEOUT = 2.0

def send_command(ser, cmd, wait_time=0.5):
    """Send command and return response."""
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)

    print(f">>> {cmd}")
    ser.write(f"{cmd}\r\n".encode())

    # For ChipSHOUTER commands, wait for full response
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

    print(response)
    return response

# Connect
ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
time.sleep(0.5)

print("Checking ChipSHOUTER status...\n")
response = send_command(ser, "CS STATUS")

print("\nClearing any faults...\n")
send_command(ser, "CS CLEAR FAULTS")

print("\nResetting ChipSHOUTER...\n")
send_command(ser, "CS RESET")
time.sleep(5.0)

print("\nChecking status after reset...\n")
send_command(ser, "CS STATUS")

ser.close()
