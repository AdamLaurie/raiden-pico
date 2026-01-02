#!/usr/bin/env python3
"""
Parameter optimization for CRP glitch attack.
Tests different voltage/width combinations to find optimal success rate.
"""

import subprocess
import sys
import re
from datetime import datetime

# Base parameters (from successful campaign)
BASE_PAUSE = 6273
ITERATIONS = 200  # Quick test per parameter set

# Parameter ranges to test
VOLTAGES = [200, 205, 210, 215, 220, 225]
WIDTHS = [60, 70, 76, 80, 90, 100]

results = []

print(f"=== Parameter Optimization ===")
print(f"Base pause: {BASE_PAUSE}")
print(f"Iterations per test: {ITERATIONS}")
print(f"Voltages: {VOLTAGES}")
print(f"Widths: {WIDTHS}")
print(f"Total tests: {len(VOLTAGES) * len(WIDTHS)}")
print()

for voltage in VOLTAGES:
    for width in WIDTHS:
        print(f"\n--- Testing V={voltage}, W={width} ---")

        cmd = [
            'python3', '-u', 'scripts/crp3_fast_glitch.py',
            str(voltage), str(BASE_PAUSE), str(width), str(ITERATIONS),
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
                    'voltage': voltage,
                    'width': width,
                    'success': success,
                    'success_pct': success_pct,
                    'blocked': blocked,
                    'crash': crash
                })

                print(f"  SUCCESS: {success} ({success_pct}%), BLOCKED: {blocked}, CRASH: {crash}")
            else:
                print(f"  Failed to parse output")
                results.append({
                    'voltage': voltage,
                    'width': width,
                    'success': 0,
                    'success_pct': 0,
                    'blocked': 0,
                    'crash': 0,
                    'error': 'parse_failed'
                })

        except subprocess.TimeoutExpired:
            print(f"  TIMEOUT")
            results.append({
                'voltage': voltage,
                'width': width,
                'success': 0,
                'success_pct': 0,
                'blocked': 0,
                'crash': 0,
                'error': 'timeout'
            })
        except Exception as e:
            print(f"  ERROR: {e}")
            results.append({
                'voltage': voltage,
                'width': width,
                'success': 0,
                'success_pct': 0,
                'blocked': 0,
                'crash': 0,
                'error': str(e)
            })

# Summary
print("\n\n=== OPTIMIZATION RESULTS ===")
print(f"{'Voltage':>8} {'Width':>6} {'Success':>8} {'Rate':>8} {'Blocked':>8} {'Crash':>8}")
print("-" * 56)

# Sort by success rate
results.sort(key=lambda x: x.get('success_pct', 0), reverse=True)

for r in results:
    print(f"{r['voltage']:>8} {r['width']:>6} {r['success']:>8} {r['success_pct']:>7.2f}% {r['blocked']:>8} {r['crash']:>8}")

# Best result
if results and results[0].get('success_pct', 0) > 0:
    best = results[0]
    print(f"\n*** BEST: V={best['voltage']}, W={best['width']} -> {best['success_pct']:.2f}% success ***")
