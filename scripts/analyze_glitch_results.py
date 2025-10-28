#!/usr/bin/env python3
"""
Analyze glitch test results from CSV log.

Shows success rate, identifies successful parameter ranges,
and generates visualizations of results.
"""

import csv
import sys
from collections import defaultdict


def load_results(filename='glitch_results.csv'):
    """Load results from CSV file."""
    results = []

    try:
        with open(filename, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                results.append({
                    'timestamp': row['timestamp'],
                    'voltage': int(row['voltage']),
                    'pause': int(row['pause']),
                    'width': int(row['width']),
                    'result': row['result'],
                    'elapsed_time': float(row['elapsed_time'])
                })
    except FileNotFoundError:
        print(f"Error: {filename} not found")
        sys.exit(1)

    return results


def analyze_results(results):
    """Analyze test results."""

    if not results:
        print("No results to analyze")
        return

    print("=" * 70)
    print("Glitch Test Results Analysis")
    print("=" * 70)
    print()

    # Count by result type
    result_counts = defaultdict(int)
    for r in results:
        result_counts[r['result']] += 1

    total = len(results)
    print(f"Total tests: {total}")
    print()
    print("Results:")
    for result_type in sorted(result_counts.keys()):
        count = result_counts[result_type]
        pct = (count / total) * 100
        print(f"  {result_type:20s}: {count:6d} ({pct:6.3f}%)")
    print()

    # Success analysis
    successes = [r for r in results if r['result'] == 'SUCCESS']
    if successes:
        print("=" * 70)
        print(f"SUCCESS PARAMETERS ({len(successes)} found)")
        print("=" * 70)
        print()

        for i, s in enumerate(successes, 1):
            print(f"{i}. V={s['voltage']}, Pause={s['pause']}, Width={s['width']}")
            print(f"   Time: {s['timestamp']}")
        print()

        # Parameter ranges that succeeded
        voltages = set(s['voltage'] for s in successes)
        pauses = set(s['pause'] for s in successes)
        widths = set(s['width'] for s in successes)

        print("Successful parameter ranges:")
        print(f"  Voltage: {sorted(voltages)}")
        print(f"  Pause:   {sorted(pauses)}")
        print(f"  Width:   {sorted(widths)}")
        print()

    # No response analysis (crashes)
    no_response = [r for r in results if r['result'] == 'NO_RESPONSE']
    if no_response:
        print("=" * 70)
        print(f"NO RESPONSE (CRASH) PARAMETERS ({len(no_response)} found)")
        print("=" * 70)
        print()

        # Show first 20
        for i, s in enumerate(no_response[:20], 1):
            print(f"{i}. V={s['voltage']}, Pause={s['pause']}, Width={s['width']}")
            if i == 20 and len(no_response) > 20:
                print(f"   ... and {len(no_response) - 20} more")
                break
        print()

        # Parameter ranges that crashed
        voltages = set(s['voltage'] for s in no_response)
        pauses = set(s['pause'] for s in no_response)
        widths = set(s['width'] for s in no_response)

        print("Crash parameter ranges:")
        print(f"  Voltage: {sorted(voltages)}")
        print(f"  Pause:   {min(pauses)}-{max(pauses)} cycles")
        print(f"  Width:   {sorted(widths)}")
        print()

    # Timing statistics
    avg_time = sum(r['elapsed_time'] for r in results) / len(results)
    min_time = min(r['elapsed_time'] for r in results)
    max_time = max(r['elapsed_time'] for r in results)

    print("Timing statistics:")
    print(f"  Average test time: {avg_time:.3f}s")
    print(f"  Min test time:     {min_time:.3f}s")
    print(f"  Max test time:     {max_time:.3f}s")
    print()

    # Test rate
    first_time = results[0]['timestamp']
    last_time = results[-1]['timestamp']
    print(f"Test period: {first_time} to {last_time}")


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description='Analyze glitch test results from CSV'
    )
    parser.add_argument(
        'filename',
        nargs='?',
        default='glitch_results.csv',
        help='CSV file to analyze (default: glitch_results.csv)'
    )

    args = parser.parse_args()

    results = load_results(args.filename)
    analyze_results(results)


if __name__ == "__main__":
    main()
