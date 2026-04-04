#!/usr/bin/env python3
"""
STM32F1 single trace: capture one ADC shunt trace from the real bootloader.

Talks to the real STM32F1 system bootloader. Power cycles the target,
syncs with the bootloader, arms a UART trigger, sends a Read Memory
command (0x11 0xEE), and captures the ADC trace around the trigger event.

Usage:
    python3 stm32_single_trace.py [--port /dev/ttyACM0] [--trigger "79 RX"]
"""
import serial
import time
import sys
import argparse
from datetime import datetime

parser = argparse.ArgumentParser(description='STM32F1 single trace capture')
parser.add_argument('--port', default='/dev/ttyACM0', help='Raiden serial port')
parser.add_argument('--trigger', default='79 RX', help='Trigger byte and direction (e.g., "79 RX" or "EE TX")')
parser.add_argument('--samples', type=int, default=4096, help='ADC samples')
parser.add_argument('--pre', type=int, default=50, help='Pre-trigger percentage')
parser.add_argument('--output', default=None, help='Output PNG path')
args = parser.parse_args()

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


# === Main ===
print(f'STM32F1 single trace: trigger={args.trigger}, {args.samples} samples')

# Power cycle and sync
cmd('target stm32f1')
cmd('target power off', t=0.5)
time.sleep(0.3)
cmd('target power on', t=0.5)
time.sleep(0.5)
cmd('target sync', t=2)

# Arm trace
trigger_parts = args.trigger.split()
cmd(f'trigger uart {" ".join(trigger_parts)}')
cmd('set count 1')
cmd(f'trace {args.samples} {args.pre}')
cmd('trace arm')

# Send Read Memory command
cmd('target send 11EE', t=1)

# Wait for trace
print('Waiting for trace...')
for _ in range(20):
    time.sleep(0.5)
    r = cmd('trace status', verbose=False)
    if 'COMPLETE' in r:
        print('  Trace complete!')
        break
else:
    r = cmd('trace status', verbose=False)
    if 'COMPLETE' not in r:
        print('  WARNING: Trace did not complete')
        cmd('arm off')
        ser.close()
        sys.exit(1)

cmd('arm off')
samples, pre_count = read_trace_dump()
cmd('trace reset', verbose=False)
cmd('target power off', t=0.3, verbose=False)
ser.close()

if not samples:
    print('No ADC samples captured.')
    sys.exit(1)

print(f'Got {len(samples)} samples, pre_count={pre_count}')

# === Plot ===
try:
    import matplotlib.pyplot as plt
    import numpy as np

    v = np.array(samples) * 3.3 / 4095
    us_per_sample = 2.0
    t = (np.arange(len(v)) - pre_count) * us_per_sample

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(14, 8),
                                    gridspec_kw={'height_ratios': [3, 1]})

    ax1.plot(t, v, linewidth=0.5, color='#2196F3')
    ax1.axvline(x=0, color='green', linestyle='--', linewidth=1.5, label=f'Trigger ({args.trigger})')
    ax1.set_xlabel('Time (us)')
    ax1.set_ylabel('Shunt V')
    ax1.set_title(f'STM32F1 bootloader trace: trigger {args.trigger} ({len(samples)} samples)')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Zoom around trigger
    zoom_start, zoom_end = -200, 500
    mask = (t >= zoom_start) & (t <= zoom_end)
    if np.any(mask):
        ax2.plot(t[mask], v[mask], linewidth=0.8, color='#FF5722')
        ax2.axvline(x=0, color='green', linestyle='--', linewidth=1.5, label='Trigger')
        ax2.set_xlabel('Time (us)')
        ax2.set_ylabel('Shunt V')
        ax2.set_title('Zoomed around trigger')
        ax2.legend()
        ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    out = args.output or f'/tmp/stm32_trace_{datetime.now().strftime("%Y%m%d_%H%M%S")}.png'
    plt.savefig(out, dpi=150)
    print(f'Plot saved to {out}')
except ImportError:
    print('matplotlib not available - skipping plot')
