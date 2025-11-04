#!/usr/bin/env python3
"""
LPC2468 Boot ROM Timing Analysis

Calculates precise timing for boot sequence based on ARM7TDMI cycle counts.
"""

def calculate_timing(clock_mhz):
    """Calculate boot timing for a given clock frequency"""

    ns_per_cycle = 1000 / clock_mhz

    # Phase 1: Reset Entry (ARM mode at 0x7FFFE000)
    phase1_instructions = [
        ("0x7FFFE000", "ldr r4, [pc, #24]", 3, "Load address 0x3FFF8000 (SCB)"),
        ("0x7FFFE004", "ldr r5, [pc, #16]", 3, "Load mask 0xFFFFBFFF"),
        ("0x7FFFE008", "ldr r6, [r4]", 3, "Read SCB register"),
        ("0x7FFFE00C", "and r6, r5, r6", 1, "Apply mask"),
        ("0x7FFFE010", "str r6, [r4]", 2, "Write back"),
        ("0x7FFFE014", "ldr pc, [pc, #-4]", 3, "Jump to 0x7FFFE040"),
    ]

    # Phase 2: CRP Check (ARM mode at 0x7FFFE040)
    phase2_instructions = [
        ("0x7FFFE040", "ldr r2, [pc, #52]", 3, "Load register address"),
        ("0x7FFFE044", "ldr r3, [pc, #60]", 3, "Load flash address 0x1FC"),
        ("0x7FFFE048", "ldr r5, [pc, #64]", 3, "Load CRP3 value"),
        ("0x7FFFE04C", "ldr r6, [pc, #64]", 3, "Load CRP1 value"),
        ("0x7FFFE050", "ldr r4, [r3]", 3, "***READ CRP FROM FLASH***"),
        ("0x7FFFE054", "cmp r4, r5", 1, "Compare with CRP3"),
        ("0x7FFFE058", "cmpne r4, r6", 1, "Compare with CRP1"),
        ("0x7FFFE05C", "bne 0x7FFFE064", 1, "Branch if not CRP1/3"),
        ("0x7FFFE064", "str r4, [r2]", 2, "Store CRP value"),
        ("0x7FFFE068", "ldr r3, [pc, #16]", 3, "Load SP address"),
        ("0x7FFFE06C", "ldr r2, [r3]", 3, "Read SP value"),
        ("0x7FFFE070", "sub sp, r2, #31", 1, "Initialize stack"),
        ("0x7FFFE074", "ldr r2, [pc, #8]", 3, "Load target 0x7FFFE327"),
        ("0x7FFFE078", "bx r2", 3, "Branch to Thumb mode"),
    ]

    # Calculate phase cycles
    phase1_cycles = sum(cyc for _, _, cyc, _ in phase1_instructions)
    phase2_cycles = sum(cyc for _, _, cyc, _ in phase2_instructions)
    phase3_cycles = 150  # Estimated boot initialization
    phase4_cycles = 58   # Checksum validation

    # Calculate times
    phase1_us = phase1_cycles / clock_mhz
    phase2_us = phase2_cycles / clock_mhz
    phase3_us = phase3_cycles / clock_mhz
    phase4_us = phase4_cycles / clock_mhz

    total_cycles = phase1_cycles + phase2_cycles + phase3_cycles + phase4_cycles
    total_us = total_cycles / clock_mhz

    # Critical windows
    crp_cycle = phase1_cycles + phase2_cycles - 14  # CRP read instruction
    crp_us = crp_cycle / clock_mhz
    crp_width_ns = 3 * ns_per_cycle

    checksum_start = phase1_cycles + phase2_cycles + phase3_cycles
    checksum_end = total_cycles
    checksum_start_us = checksum_start / clock_mhz
    checksum_end_us = checksum_end / clock_mhz
    checksum_width_us = (checksum_end - checksum_start) / clock_mhz

    branch_cycle = total_cycles
    branch_us = branch_cycle / clock_mhz
    branch_width_ns = 1 * ns_per_cycle

    return {
        'clock_mhz': clock_mhz,
        'ns_per_cycle': ns_per_cycle,
        'phases': {
            'phase1': {'cycles': phase1_cycles, 'us': phase1_us, 'instructions': phase1_instructions},
            'phase2': {'cycles': phase2_cycles, 'us': phase2_us, 'instructions': phase2_instructions},
            'phase3': {'cycles': phase3_cycles, 'us': phase3_us},
            'phase4': {'cycles': phase4_cycles, 'us': phase4_us},
        },
        'total': {'cycles': total_cycles, 'us': total_us},
        'glitch_windows': {
            'crp_read': {
                'cycle': crp_cycle,
                'us': crp_us,
                'width_ns': crp_width_ns,
                'address': '0x7FFFE050'
            },
            'checksum_loop': {
                'start_cycle': checksum_start,
                'end_cycle': checksum_end,
                'start_us': checksum_start_us,
                'end_us': checksum_end_us,
                'width_us': checksum_width_us
            },
            'branch_decision': {
                'cycle': branch_cycle,
                'us': branch_us,
                'width_ns': branch_width_ns
            }
        }
    }


def print_analysis(timing):
    """Print formatted timing analysis"""

    print("=" * 70)
    print(f"LPC2468 BOOT ROM TIMING ANALYSIS @ {timing['clock_mhz']} MHz")
    print("=" * 70)
    print()
    print(f"Clock Period: {timing['ns_per_cycle']:.2f} ns/cycle")
    print()

    # Phase breakdown
    print("BOOT SEQUENCE PHASES")
    print("-" * 70)
    for phase_name, phase_data in timing['phases'].items():
        if 'instructions' in phase_data:
            print(f"\n{phase_name.upper()}:")
            for addr, instr, cycles, desc in phase_data['instructions']:
                time_ns = cycles * timing['ns_per_cycle']
                print(f"  {addr}  {instr:25s} {cycles:2d} cyc ({time_ns:6.1f} ns)  // {desc}")
        print(f"\n  Total: {phase_data['cycles']} cycles ({phase_data['us']:.3f} µs)")

    print()
    print("=" * 70)
    print("TIMING SUMMARY")
    print("=" * 70)
    print(f"Total boot time: {timing['total']['cycles']} cycles ({timing['total']['us']:.3f} µs)")
    print()

    # Glitch windows
    print("=" * 70)
    print("CRITICAL GLITCH WINDOWS")
    print("=" * 70)
    print()

    crp = timing['glitch_windows']['crp_read']
    print(f"1. CRP Value Read (Primary Target)")
    print(f"   Cycle:   {crp['cycle']} from reset")
    print(f"   Time:    {crp['us']:.3f} µs from reset")
    print(f"   Width:   {crp['width_ns']:.0f} ns")
    print(f"   Address: {crp['address']}")
    print()

    cksum = timing['glitch_windows']['checksum_loop']
    print(f"2. Checksum Validation Loop")
    print(f"   Cycles:  {cksum['start_cycle']}-{cksum['end_cycle']} from reset")
    print(f"   Time:    {cksum['start_us']:.3f}-{cksum['end_us']:.3f} µs from reset")
    print(f"   Width:   {cksum['width_us']:.3f} µs")
    print()

    branch = timing['glitch_windows']['branch_decision']
    print(f"3. Branch Decision")
    print(f"   Cycle:   {branch['cycle']} from reset")
    print(f"   Time:    {branch['us']:.3f} µs from reset")
    print(f"   Width:   {branch['width_ns']:.0f} ns")
    print()


def compare_clocks(clocks):
    """Compare timing across different clock speeds"""

    timings = [calculate_timing(clk) for clk in clocks]

    print("=" * 70)
    print("CLOCK FREQUENCY COMPARISON")
    print("=" * 70)
    print()

    print(f"{'Clock':<10} {'Boot Time':<12} {'CRP Read':<12} {'CRP Window':<12}")
    print("-" * 70)
    for t in timings:
        clk = t['clock_mhz']
        boot = t['total']['us']
        crp = t['glitch_windows']['crp_read']['us']
        window = t['glitch_windows']['crp_read']['width_ns']
        print(f"{clk:>4} MHz    {boot:>6.2f} µs    {crp:>6.3f} µs    {window:>6.0f} ns")
    print()


def main():
    import argparse

    parser = argparse.ArgumentParser(description='LPC2468 Boot ROM Timing Analysis')
    parser.add_argument('--clock', type=float, default=12.0,
                      help='Clock frequency in MHz (default: 12.0)')
    parser.add_argument('--compare', action='store_true',
                      help='Compare multiple clock speeds')

    args = parser.parse_args()

    if args.compare:
        compare_clocks([4, 12, 72])
    else:
        timing = calculate_timing(args.clock)
        print_analysis(timing)


if __name__ == '__main__':
    main()
