#!/usr/bin/env python3
"""
STM32F1 dual trace: measure glitch window on real bootloader with median averaging.

Talks to the real STM32F1 system bootloader (not an SRAM payload) and captures
ADC shunt traces triggered on TX 0xEE (Read Memory command byte) and RX 0x{RX_BYTE}
(ACK after rdp_check). Multiple runs with median averaging reject periodic
noise (watchdog resets, etc.) while preserving the true power signature.

The target is power-cycled between each run for identical conditions.

Usage:
    python3 stm32_dual_trace.py [--port /dev/ttyACM0] [--runs 15] [--samples 4096]
"""
import serial
import time
import sys
import argparse
from datetime import datetime

parser = argparse.ArgumentParser(description='STM32F1 dual trace with median averaging')
parser.add_argument('--port', default='/dev/ttyACM0', help='Raiden serial port')
parser.add_argument('--runs', type=int, default=15, help='Runs per trigger type')
parser.add_argument('--samples', type=int, default=4096, help='ADC samples per trace')
parser.add_argument('--pre', type=int, default=50, help='Pre-trigger percentage')
parser.add_argument('--output', default=None, help='Output PNG path (default: /tmp/...)')
parser.add_argument('--rx-byte', default='79', help='RX trigger byte hex (79=ACK, 1F=NACK for RDP1)')
args = parser.parse_args()

RUNS = args.runs
TRACE_SAMPLES = args.samples
PRE_PCT = args.pre
RX_BYTE = args.rx_byte.upper()

ser = serial.Serial(args.port, 115200, timeout=2)
time.sleep(1)
ser.reset_input_buffer()


def cmd(c, t=1.0, verbose=True):
    ser.reset_input_buffer()
    ser.write((c + '\r\n').encode())
    time.sleep(t)
    r = ser.read(ser.in_waiting or 1)
    while True:
        time.sleep(0.1)
        more = ser.read(ser.in_waiting or 0)
        if not more:
            break
        r += more
    text = r.decode('utf-8', errors='replace').strip()
    if verbose:
        print(f'> {c}')
        for line in text.split('\n'):
            line = line.strip()
            if line and line != c:
                print(f'  {line}')
        print(flush=True)
    return text


def read_trace_dump():
    """Read trace dump and parse ADC samples."""
    ser.reset_input_buffer()
    ser.write(b'trace dump\r\n')
    time.sleep(0.5)
    chunks = []
    deadline = time.time() + 15
    while time.time() < deadline:
        data = ser.read(ser.in_waiting or 1)
        if data:
            chunks.append(data)
            text = data.decode('utf-8', errors='replace')
            if 'TRACE COMPLETE' in text or 'RAW_END' in text or 'ERROR' in text:
                time.sleep(0.3)
                more = ser.read(ser.in_waiting or 0)
                if more:
                    chunks.append(more)
                break
            deadline = time.time() + 3
        else:
            time.sleep(0.1)

    trace_output = b''.join(chunks).decode('utf-8', errors='replace')
    samples = []
    pre_count = 0
    in_raw = False
    for line in trace_output.split('\n'):
        line = line.strip()
        if line.startswith('RAW_START'):
            parts = line.split()
            if len(parts) >= 4:
                pre_count = int(parts[3])
            in_raw = True
            continue
        if line.startswith('RAW_END'):
            in_raw = False
            continue
        if in_raw and ':' in line:
            hex_part = line.split(':', 1)[1].strip()
            bytes_list = hex_part.split()
            i = 0
            while i + 1 < len(bytes_list):
                try:
                    lo = int(bytes_list[i], 16)
                    hi = int(bytes_list[i + 1], 16)
                    samples.append((hi << 8) | lo)
                    i += 2
                except ValueError:
                    i += 1
    return samples, pre_count


def power_cycle_and_sync():
    """Power cycle the target and sync with bootloader."""
    cmd('target power off', t=0.5, verbose=False)
    time.sleep(0.3)
    cmd('target power on', t=0.5, verbose=False)
    time.sleep(0.5)
    cmd('target stm32f1', t=0.3, verbose=False)
    r = cmd('target sync', t=2, verbose=False)
    if '0x79' not in r.lower() and 'ok' not in r.lower() and 'ack' not in r.lower():
        # Retry once
        cmd('target send FF', t=0.5, verbose=False)
        r = cmd('target sync', t=2, verbose=False)
    return r


def do_single_trace(trigger_cmd):
    """Run one trace capture. Returns (samples, pre_count) or (None, 0)."""
    cmd(trigger_cmd, verbose=False)
    cmd('set count 1', verbose=False)
    cmd(f'trace {TRACE_SAMPLES} {PRE_PCT}', verbose=False)
    cmd('trace arm', verbose=False)

    # Send Read Memory command to trigger rdp_check
    cmd('target send 11EE', t=1, verbose=False)

    # Wait for trace completion
    for _ in range(20):
        time.sleep(0.5)
        r = cmd('trace status', verbose=False)
        if 'COMPLETE' in r:
            break
    else:
        r = cmd('trace status', verbose=False)
        if 'COMPLETE' not in r:
            cmd('arm off', verbose=False)
            cmd('trace reset', verbose=False)
            return None, 0

    cmd('arm off', verbose=False)
    samples, pre_count = read_trace_dump()
    cmd('trace reset', verbose=False)
    return samples, pre_count


def collect_runs(trigger_cmd, label, n_runs):
    """Collect n_runs traces, power cycling between each. Returns list of (samples, pre_count)."""
    results = []
    for i in range(n_runs):
        print(f'  {label} run {i + 1}/{n_runs}...', end=' ', flush=True)
        power_cycle_and_sync()
        samples, pre_count = do_single_trace(trigger_cmd)
        if samples and len(samples) > 0:
            results.append((samples, pre_count))
            print(f'OK ({len(samples)} samples, pre={pre_count})')
        else:
            print('FAILED')
    return results


# === Main ===
print(f'STM32F1 dual trace: {RUNS} runs per trigger, {TRACE_SAMPLES} samples')
print()

# Initial setup
cmd('target stm32f1')

print(f'=== Collecting {RUNS} TX 0xEE traces ===')
tx_runs = collect_runs('trigger uart EE TX', 'TX', RUNS)

print(f'\n=== Collecting {RUNS} RX 0x{RX_BYTE} traces ===')
rx_runs = collect_runs(f'trigger uart {RX_BYTE} RX', 'RX', RUNS)

# Power off target when done
cmd('target power off', t=0.3, verbose=False)
ser.close()

if len(tx_runs) < 3 or len(rx_runs) < 3:
    print(f'\nInsufficient data: {len(tx_runs)} TX, {len(rx_runs)} RX runs')
    sys.exit(1)

print(f'\nGood runs: {len(tx_runs)} TX, {len(rx_runs)} RX')

# === Compute medians ===
import numpy as np
import matplotlib.pyplot as plt

# Align all runs to their trigger index and compute element-wise median
def compute_median(runs):
    """Align traces by trigger index and compute median."""
    min_len = min(len(s) for s, _ in runs)
    # Truncate all to same length, aligned by trigger
    aligned = []
    pres = []
    for samples, pre_count in runs:
        arr = np.array(samples[:min_len], dtype=np.float64)
        aligned.append(arr)
        pres.append(pre_count)
    stacked = np.vstack(aligned)
    median = np.median(stacked, axis=0)
    median_pre = int(np.median(pres))
    return median, median_pre

tx_median, tx_pre = compute_median(tx_runs)
rx_median, rx_pre = compute_median(rx_runs)

us_per_sample = 2.0
RAIDEN_MHZ = 150

tx_v = tx_median * 3.3 / 4095
rx_v = rx_median * 3.3 / 4095
tx_t = (np.arange(len(tx_v)) - tx_pre) * us_per_sample
rx_t = (np.arange(len(rx_v)) - rx_pre) * us_per_sample

fig, axes = plt.subplots(3, 1, figsize=(16, 10), gridspec_kw={'height_ratios': [2, 2, 2]})

# Plot 1: TX median
ax = axes[0]
ax.plot(tx_t, tx_v, linewidth=0.5, color='#2196F3', label=f'TX median ({len(tx_runs)} runs)')
ax.axvline(x=0, color='red', linestyle='--', linewidth=1.5, label='TX 0xEE')
ax.set_ylabel('Shunt V')
ax.set_title(f'TX 0xEE trigger \u2014 {len(tx_runs)} runs, median')
ax.legend()
ax.grid(True, alpha=0.3)

# Plot 2: RX median
ax = axes[1]
ax.plot(rx_t, rx_v, linewidth=0.5, color='#4CAF50', label=f'RX median ({len(rx_runs)} runs)')
ax.axvline(x=0, color='green', linestyle='--', linewidth=1.5, label=f'RX 0x{RX_BYTE}')
ax.set_ylabel('Shunt V')
ax.set_title(f'RX 0x{RX_BYTE} trigger \u2014 {len(rx_runs)} runs, median')
ax.legend()
ax.grid(True, alpha=0.3)

# Plot 3: Overlay — align by cross-correlation of median traces
ax = axes[2]

corr = np.correlate(tx_v - tx_v.mean(), rx_v - rx_v.mean(), mode='full')
zero_lag_idx = len(rx_v) - 1
corr_positive = corr.copy()
corr_positive[:zero_lag_idx] = -np.inf
lag_samples = np.argmax(corr_positive) - zero_lag_idx
lag_us = lag_samples * us_per_sample

rx_t_aligned = rx_t + lag_us
rx_trigger_us = lag_us
rx_trigger_cycles = rx_trigger_us * RAIDEN_MHZ

margin = 1500
window_start = -margin
window_end = max(rx_trigger_us, 0) + margin

tx_mask = (tx_t >= window_start) & (tx_t <= window_end)
rx_mask = (rx_t_aligned >= window_start) & (rx_t_aligned <= window_end)

ax.plot(tx_t[tx_mask], tx_v[tx_mask], linewidth=0.8, color='#2196F3', alpha=0.7, label='TX median')
ax.plot(rx_t_aligned[rx_mask], rx_v[rx_mask], linewidth=0.8, color='#4CAF50', alpha=0.7, label='RX median (shifted)')
ax.axvline(x=0, color='red', linestyle='--', linewidth=1.5, alpha=0.8, label='TX 0xEE')
ax.axvline(x=rx_trigger_us, color='green', linestyle='--', linewidth=1.5, alpha=0.8,
           label=f'RX 0x{RX_BYTE} (+{rx_trigger_us:.0f}us)')

if rx_trigger_us > 0:
    ax.axvspan(0, rx_trigger_us, alpha=0.1, color='orange',
               label=f'Window ({rx_trigger_us:.0f}us = {rx_trigger_cycles:.0f} cycles)')

ax.set_xlabel('Time (us) relative to TX 0xEE')
ax.set_ylabel('Shunt V')
ax.set_title('Median overlay \u2014 real bootloader glitch window')
ax.legend(loc='upper right', fontsize=8)
ax.grid(True, alpha=0.3)

ax2 = ax.secondary_xaxis('top', functions=(
    lambda us: us * RAIDEN_MHZ,
    lambda cy: cy / RAIDEN_MHZ
))
ax2.set_xlabel('Raiden cycles (150MHz)')

ymin, ymax = ax.get_ylim()
ax.annotate(f'PAUSE = {rx_trigger_cycles:.0f}\n({rx_trigger_us:.0f}us)',
            xy=(rx_trigger_us, ymax),
            xytext=(rx_trigger_us + 30, ymax - (ymax - ymin) * 0.05),
            fontsize=9, fontweight='bold', color='#2E7D32',
            ha='left', va='top',
            arrowprops=dict(arrowstyle='->', color='#2E7D32', lw=1.5))

print(f'Cross-correlation lag: {lag_samples} samples = {lag_us:.0f}us')
print(f'Glitch window: TX 0xEE \u2192 RX 0x{RX_BYTE} = {rx_trigger_us:.0f}us = {rx_trigger_cycles:.0f} Raiden cycles')

plt.tight_layout()
out = args.output or f'/tmp/real_bl_median_{datetime.now().strftime("%Y%m%d_%H%M%S")}.png'
plt.savefig(out, dpi=600)
print(f'\nPlot saved to {out}')
