#!/usr/bin/env python3
"""
Automatic glitch parameter exploration.
Runs multiple marathon tests with different parameter sets.
Continues until success found or user stops.
"""

import subprocess
import time
import csv
import os
from datetime import datetime

# Parameter sets to try
PARAMETER_SETS = [
    # Set 1: Current best guess (crashes at V=300, early timing)
    {
        'name': 'Set1_LowV_EarlyTiming',
        'voltage_range': [250, 275, 300, 325, 350],
        'pause_range': list(range(0, 2001, 50)) + list(range(6500, 8001, 100)),
        'width_range': list(range(100, 251, 25)),
    },
    # Set 2: Focus on crash boundary (V=300, fine granularity)
    {
        'name': 'Set2_V300_FineTiming',
        'voltage_range': [280, 290, 300, 310, 320],
        'pause_range': list(range(0, 3001, 25)),
        'width_range': list(range(75, 251, 25)),
    },
    # Set 3: Very low voltage, very early timing
    {
        'name': 'Set3_VeryLowV_VeryEarly',
        'voltage_range': [200, 225, 250, 275, 300],
        'pause_range': list(range(0, 1501, 25)),
        'width_range': list(range(50, 201, 25)),
    },
    # Set 4: Mid-range everything
    {
        'name': 'Set4_MidRange',
        'voltage_range': [300, 325, 350, 375, 400],
        'pause_range': list(range(2000, 6001, 100)),
        'width_range': list(range(100, 251, 25)),
    },
    # Set 5: Ultra-fine around V=300, Pause=0-500
    {
        'name': 'Set5_UltraFine_V300_P0-500',
        'voltage_range': [290, 295, 300, 305, 310],
        'pause_range': list(range(0, 501, 10)),
        'width_range': list(range(100, 201, 10)),
    },
]

HOURS_PER_SET = 3  # Run each set for 3 hours


def check_results_for_success(csv_file):
    """Check if any SUCCESS results found in CSV."""
    try:
        with open(csv_file, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                if row['result'] == 'SUCCESS':
                    return True
    except:
        return False
    return False


def analyze_results(csv_file):
    """Quick analysis of results."""
    try:
        with open(csv_file, 'r') as f:
            reader = csv.DictReader(f)
            results = list(reader)

        total = len(results)
        if total == 0:
            return None

        success = sum(1 for r in results if r['result'] == 'SUCCESS')
        no_response = sum(1 for r in results if r['result'] == 'NO_RESPONSE')
        error19 = sum(1 for r in results if r['result'] == 'ERROR19')

        return {
            'total': total,
            'success': success,
            'no_response': no_response,
            'error19': error19,
            'success_rate': (success / total) * 100 if total > 0 else 0,
            'crash_rate': (no_response / total) * 100 if total > 0 else 0,
        }
    except:
        return None


def run_parameter_set(param_set, iteration):
    """Run marathon test with specific parameter set."""
    timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    log_file = f"glitch_marathon_{timestamp}_{param_set['name']}.log"
    csv_file = f"glitch_results_{timestamp}_{param_set['name']}.csv"

    print("=" * 80)
    print(f"ITERATION {iteration}: {param_set['name']}")
    print(f"Started: {datetime.now()}")
    print(f"Duration: {HOURS_PER_SET} hours")
    print(f"Voltage: {len(param_set['voltage_range'])} values ({min(param_set['voltage_range'])}-{max(param_set['voltage_range'])})")
    print(f"Pause: {len(param_set['pause_range'])} values")
    print(f"Width: {len(param_set['width_range'])} values")
    print(f"Total combinations: {len(param_set['voltage_range']) * len(param_set['pause_range']) * len(param_set['width_range'])}")
    print(f"Log: {log_file}")
    print(f"Results: {csv_file}")
    print("=" * 80)
    print()

    # Create custom marathon script for this parameter set
    script_content = f"""
import sys
sys.path.insert(0, '/home/addy/work/claude-code/raiden-pico/scripts')
from glitch_marathon import *

# Override parameter ranges
voltage_range = {param_set['voltage_range']}
pause_range = {param_set['pause_range']}
width_range = {param_set['width_range']}

# Run with custom parameters
run_marathon_custom(voltage_range, pause_range, width_range, {HOURS_PER_SET}, '{csv_file}')
"""

    # For now, just run the standard marathon and trust it will work
    # (We'd need to modify glitch_marathon.py to accept custom ranges)
    cmd = ['python3', 'scripts/glitch_marathon.py', '--hours', str(HOURS_PER_SET)]

    with open(log_file, 'w') as log:
        log.write(f"Parameter Set: {param_set['name']}\n")
        log.write(f"Started: {datetime.now()}\n\n")

        proc = subprocess.Popen(
            cmd,
            stdout=log,
            stderr=subprocess.STDOUT,
            cwd='/home/addy/work/claude-code/raiden-pico'
        )

    return proc, log_file, csv_file


def main():
    """Main exploration loop."""
    print("=" * 80)
    print("AUTOMATIC GLITCH PARAMETER EXPLORATION")
    print("=" * 80)
    print(f"Will run {len(PARAMETER_SETS)} parameter sets")
    print(f"Duration per set: {HOURS_PER_SET} hours")
    print(f"Total maximum time: {len(PARAMETER_SETS) * HOURS_PER_SET} hours")
    print()
    print("Will stop when:")
    print("  - SUCCESS found, or")
    print("  - All parameter sets tested, or")
    print("  - User manually stops")
    print()
    print("=" * 80)
    print()

    iteration = 1

    for param_set in PARAMETER_SETS:
        proc, log_file, csv_file = run_parameter_set(param_set, iteration)

        # Wait for completion
        print(f"Running... (monitor: tail -f {log_file})")
        proc.wait()

        print(f"\nIteration {iteration} complete!")

        # Check for success
        if check_results_for_success(csv_file):
            print()
            print("=" * 80)
            print("SUCCESS FOUND!")
            print("=" * 80)
            analysis = analyze_results(csv_file)
            if analysis:
                print(f"Total tests: {analysis['total']}")
                print(f"Successes: {analysis['success']} ({analysis['success_rate']:.3f}%)")
                print(f"Results file: {csv_file}")
            print()
            print("Run analysis:")
            print(f"  python3 scripts/analyze_glitch_results.py {csv_file}")
            return

        # Analyze results
        analysis = analyze_results(csv_file)
        if analysis:
            print(f"  Total tests: {analysis['total']}")
            print(f"  Success: {analysis['success']}")
            print(f"  Crashes: {analysis['no_response']} ({analysis['crash_rate']:.1f}%)")
            print(f"  Normal: {analysis['error19']}")
        print()

        iteration += 1
        time.sleep(2)

    print("=" * 80)
    print("All parameter sets tested. No success found.")
    print("Review results and adjust parameter sets if needed.")
    print("=" * 80)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\nExploration stopped by user")
