#!/usr/bin/env python3
"""
Fine-grained timing optimization for CRP glitch attack.
Tests pause values ± a few cycles around the known good value.

At 150MHz, one cycle = 6.67ns, so timing is critical.
"""

import subprocess
import sys
import re

# Best parameters from voltage/width optimization
VOLTAGE = 210
WIDTH = 90
BASE_PAUSE = 6273
ITERATIONS = 200

# Test pause values: ±1, ±2, ±3, ±5, ±10 cycles around base
PAUSE_OFFSETS = [-10, -5, -3, -2, -1, 0, +1, +2, +3, +5, +10]
PAUSE_VALUES = [BASE_PAUSE + offset for offset in PAUSE_OFFSETS]

results = []

print(f"=== Timing Optimization ===")
print(f"Voltage: {VOLTAGE}V")
print(f"Width: {WIDTH} cycles")
print(f"Base pause: {BASE_PAUSE} cycles")
print(f"Testing offsets: {PAUSE_OFFSETS}")
print(f"Iterations per test: {ITERATIONS}")
print(f"Total tests: {len(PAUSE_VALUES)}")
print()

for pause in PAUSE_VALUES:
    offset = pause - BASE_PAUSE
    sign = "+" if offset >= 0 else ""
    print(f"\n--- Testing pause={pause} ({sign}{offset}) ---")

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
                'offset': offset,
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
                'offset': offset,
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
            'offset': offset,
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
            'offset': offset,
            'success': 0,
            'success_pct': 0,
            'blocked': 0,
            'crash': 0,
            'error': str(e)
        })

# Summary
print("\n\n=== TIMING OPTIMIZATION RESULTS ===")
print(f"{'Pause':>8} {'Offset':>8} {'Success':>8} {'Rate':>8} {'Blocked':>8} {'Crash':>8}")
print("-" * 64)

# Sort by success rate
results.sort(key=lambda x: x.get('success_pct', 0), reverse=True)

for r in results:
    sign = "+" if r['offset'] >= 0 else ""
    print(f"{r['pause']:>8} {sign}{r['offset']:>7} {r['success']:>8} {r['success_pct']:>7.2f}% {r['blocked']:>8} {r['crash']:>8}")

# Best result
if results and results[0].get('success_pct', 0) > 0:
    best = results[0]
    sign = "+" if best['offset'] >= 0 else ""
    print(f"\n*** BEST: pause={best['pause']} ({sign}{best['offset']}) -> {best['success_pct']:.2f}% success ***")
