#!/usr/bin/env python3
"""
Fine-grained timing optimization - test all pause values in range.
"""

import subprocess
import sys
import re

# Best parameters from previous optimization
VOLTAGE = 210
WIDTH = 90
ITERATIONS = 200

# Test all pause values from 6274 to 6280
PAUSE_START = 6274
PAUSE_END = 6280
PAUSE_VALUES = list(range(PAUSE_START, PAUSE_END + 1))

results = []

print(f"=== Fine Timing Optimization ===")
print(f"Voltage: {VOLTAGE}V")
print(f"Width: {WIDTH} cycles")
print(f"Pause range: {PAUSE_START} to {PAUSE_END}")
print(f"Iterations per test: {ITERATIONS}")
print(f"Total tests: {len(PAUSE_VALUES)}")
print()

for pause in PAUSE_VALUES:
    print(f"\n--- Testing pause={pause} ---")

    cmd = [
        'python3', '-u', 'scripts/crp3_fast_glitch.py',
        str(VOLTAGE), str(pause), str(WIDTH), str(ITERATIONS),
        '--no-save'
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        output = result.stdout + result.stderr

        # Parse results
        success_match = re.search(r'SUCCESS:\s+(\d+)\s+\((\d+\.\d+)%\)', output)
        blocked_match = re.search(r'CRP_BLOCKED:\s+(\d+)\s+\((\d+\.\d+)%\)', output)
        crash_match = re.search(r'CRASH:\s+(\d+)\s+\((\d+\.\d+)%\)', output)

        if success_match and blocked_match and crash_match:
            success = int(success_match.group(1))
            success_pct = float(success_match.group(2))
            blocked = int(blocked_match.group(1))
            crash = int(crash_match.group(1))

            results.append({
                'pause': pause,
                'success': success,
                'success_pct': success_pct,
                'blocked': blocked,
                'crash': crash
            })

            print(f"  SUCCESS: {success} ({success_pct}%), BLOCKED: {blocked}, CRASH: {crash}")
        else:
            print(f"  Failed to parse output")
            results.append({
                'pause': pause,
                'success': 0,
                'success_pct': 0,
                'blocked': 0,
                'crash': 0,
                'error': 'parse_failed'
            })

    except subprocess.TimeoutExpired:
        print(f"  TIMEOUT")
        results.append({
            'pause': pause,
            'success': 0,
            'success_pct': 0,
            'blocked': 0,
            'crash': 0,
            'error': 'timeout'
        })
    except Exception as e:
        print(f"  ERROR: {e}")
        results.append({
            'pause': pause,
            'success': 0,
            'success_pct': 0,
            'blocked': 0,
            'crash': 0,
            'error': str(e)
        })

# Summary
print("\n\n=== FINE TIMING RESULTS ===")
print(f"{'Pause':>8} {'Success':>8} {'Rate':>8} {'Blocked':>8} {'Crash':>8}")
print("-" * 52)

# Sort by success rate
results.sort(key=lambda x: x.get('success_pct', 0), reverse=True)

for r in results:
    print(f"{r['pause']:>8} {r['success']:>8} {r['success_pct']:>7.2f}% {r['blocked']:>8} {r['crash']:>8}")

# Best result
if results and results[0].get('success_pct', 0) > 0:
    best = results[0]
    print(f"\n*** BEST: pause={best['pause']} -> {best['success_pct']:.2f}% success ***")
