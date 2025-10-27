#!/usr/bin/env python3
"""Test script to verify glitch is actually firing."""

import serial
import time

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200
TIMEOUT = 2.0

def send_command(ser, cmd, wait_time=0.2):
    """Send command and return response."""
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)

    ser.write(f"{cmd}\r\n".encode())
    time.sleep(wait_time)

    response = ""
    if ser.in_waiting > 0:
        response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

    return response

# Connect
ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
time.sleep(0.5)

print("Checking initial glitch count...")
response = send_command(ser, "STATUS", wait_time=0.3)
print(response)

print("\nConfiguring glitch...")
send_command(ser, "SET PAUSE 7000")
send_command(ser, "SET WIDTH 150")
send_command(ser, "TRIGGER UART 0d")

print("\nArming and triggering...")
send_command(ser, "ARM ON")
send_command(ser, 'TARGET SEND "R 0 516096"', wait_time=1.5)

print("\nChecking glitch count after trigger...")
response = send_command(ser, "STATUS", wait_time=0.3)
print(response)

ser.close()
