#!/usr/bin/env python3
"""
LPC2468 ISP Read Command Timing Calculator

Calculates precise timing from UART '\r' echo to CRP check.
"""

def calculate_isp_timing(clock_mhz, baud_rate=38400):
    """Calculate ISP command timing"""

    ns_per_cycle = 1000 / clock_mhz
    bit_time_us = 1_000_000 / baud_rate
    char_time_us = 10 * bit_time_us  # 8N1

    # CPU execution timeline (after '\r' TX starts)
    timeline = [
        ("Return from TX ISR", 10, "Interrupt overhead"),
        ("Check for '\\r' in buffer", 20, "Find command end"),
        ("Send '\\n' to UART", 10, "Queue linefeed"),
        ("Parse command 'R'", 30, "Switch on command"),
        ("Find first parameter", 50, "Skip whitespace"),
        ("Parse address (hex)", 200, "ASCII to hex"),
        ("Find second parameter", 30, "Skip whitespace"),
        ("Parse count (decimal)", 150, "ASCII to decimal"),
        ("Validate parameters", 40, "Range checks"),
        ("**LOAD CRP**", 3, "LDR from 0x1FC"),
        ("**COMPARE CRP**", 2, "CMP instruction"),
        ("**BRANCH ON CRP**", 3, "Conditional branch"),
    ]

    total_cycles = sum(cycles for _, cycles, _ in timeline)
    total_us = total_cycles / clock_mhz

    crp_check_cycles = 3 + 2 + 3  # Load + Compare + Branch
    crp_check_ns = crp_check_cycles * ns_per_cycle

    return {
        'clock_mhz': clock_mhz,
        'baud_rate': baud_rate,
        'ns_per_cycle': ns_per_cycle,
        'char_time_us': char_time_us,
        'timeline': timeline,
        'total_cycles': total_cycles,
        'total_us': total_us,
        'crp_check_cycles': crp_check_cycles,
        'crp_check_ns': crp_check_ns,
    }


def print_timing(timing):
    """Print formatted timing analysis"""

    print("=" * 70)
    print(f"LPC2468 ISP READ COMMAND TIMING @ {timing['clock_mhz']} MHz")
    print("=" * 70)
    print()

    print(f"CPU Clock:     {timing['clock_mhz']} MHz")
    print(f"UART Baud:     {timing['baud_rate']}")
    print(f"Cycle time:    {timing['ns_per_cycle']:.2f} ns")
    print(f"UART char time: {timing['char_time_us']:.2f} µs")
    print()

    print("=" * 70)
    print("TRIGGER: UART TX '\\r' Rising Edge")
    print("=" * 70)
    print()
    print("After '\r' TX starts, CPU execution is deterministic:")
    print("  - No more UART involvement")
    print("  - Synchronous CPU execution @ {} MHz".format(timing['clock_mhz']))
    print("  - Zero jitter")
    print()

    print("=" * 70)
    print("EXECUTION TIMELINE")
    print("=" * 70)
    print()

    print(f"{'Step':<35} {'Cycles':<8} {'Time (µs)':<12} {'Cumulative'}")
    print("-" * 70)

    cumulative = 0
    for step, cycles, desc in timing['timeline']:
        time_us = cycles / timing['clock_mhz']
        cumulative += cycles
        cumulative_us = cumulative / timing['clock_mhz']
        marker = " <<<" if "**" in step else ""
        print(f"{step:<35} {cycles:<8} {time_us:>8.2f}    {cumulative_us:>8.2f}{marker}")

    print()
    print(f"Total: {timing['total_cycles']} cycles = {timing['total_us']:.2f} µs")
    print()

    print("=" * 70)
    print("GLITCH PARAMETERS")
    print("=" * 70)
    print()
    print(f"Trigger source:  UART TX '\\r' rising edge")
    print(f"Trigger type:    External (logic analyzer)")
    print(f"Delay:           {timing['total_us']:.2f} µs ({timing['total_cycles']} cycles)")
    print(f"Window:          {timing['crp_check_ns']:.0f} ns ({timing['crp_check_cycles']} cycles)")
    print(f"Jitter:          0 ns (deterministic)")
    print()

    print("Advantages:")
    print(f"  ✓ Long delay ({timing['total_us']:.1f} µs) = easy timing")
    print(f"  ✓ External trigger (UART TX)")
    print(f"  ✓ Zero jitter (synchronous)")
    print(f"  ✓ Unlimited retries (no power cycle)")
    print()


def compare_methods():
    """Compare boot ROM vs ISP glitching"""

    print("=" * 70)
    print("BOOT ROM vs ISP GLITCHING COMPARISON")
    print("=" * 70)
    print()

    boot_delay = 4.0
    isp = calculate_isp_timing(12.0)

    print(f"{'Method':<15} {'Trigger':<20} {'Delay':<15} {'Window':<15} {'Jitter'}")
    print("-" * 70)
    print(f"{'Boot ROM':<15} {'Power-on':<20} {f'{boot_delay:.1f} µs':<15} {'250 ns':<15} {'0 ns'}")
    print(f"{'ISP Read':<15} {'UART TX':<20} {f'{isp[\"total_us\"]:.1f} µs':<15} {f'{isp[\"crp_check_ns\"]:.0f} ns':<15} {'0 ns'}")
    print()
    print(f"ISP delay is {isp['total_us']/boot_delay:.1f}x longer = easier timing!")
    print()


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description='LPC2468 ISP Read Command Timing Calculator'
    )
    parser.add_argument('--clock', type=float, default=12.0,
                       help='CPU clock in MHz (default: 12.0)')
    parser.add_argument('--baud', type=int, default=38400,
                       help='UART baud rate (default: 38400)')
    parser.add_argument('--compare', action='store_true',
                       help='Compare with boot ROM glitch')

    args = parser.parse_args()

    if args.compare:
        compare_methods()
    else:
        timing = calculate_isp_timing(args.clock, args.baud)
        print_timing(timing)


if __name__ == '__main__':
    main()
