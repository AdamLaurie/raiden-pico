#!/usr/bin/env python3
"""
Extract and analyze boot sequence from LPC2468 boot ROM disassembly
"""

import re
import sys


def parse_disassembly(filename):
    """Parse objdump disassembly output"""

    instructions = []

    with open(filename, 'r') as f:
        for line in f:
            # Match instruction lines
            match = re.match(r'\s*([0-9a-f]+):\s+([0-9a-f]+)\s+(.+)', line)
            if match:
                addr = int(match.group(1), 16)
                hex_code = match.group(2)
                asm = match.group(3).strip()

                instructions.append({
                    'addr': addr,
                    'hex': hex_code,
                    'asm': asm
                })

    return instructions


def find_boot_sequence(instructions, start_addr, end_addr):
    """Extract boot sequence between addresses"""

    sequence = []
    for instr in instructions:
        if start_addr <= instr['addr'] <= end_addr:
            sequence.append(instr)

    return sequence


def print_sequence(sequence, title):
    """Print formatted instruction sequence"""

    print(f"\n{title}")
    print("=" * 70)
    for instr in sequence:
        print(f"0x{instr['addr']:08x}:  {instr['hex']:8s}  {instr['asm']}")
    print()


def find_crp_check(instructions):
    """Find CRP check code that reads from 0x1FC"""

    # Look for load from address 0x1FC
    crp_sequence = []
    found_crp = False

    for i, instr in enumerate(instructions):
        if '0x1fc' in instr['asm'].lower() or '0x000001fc' in instr['asm'].lower():
            # Found reference to CRP location, get surrounding context
            start = max(0, i - 5)
            end = min(len(instructions), i + 20)
            crp_sequence = instructions[start:end]
            found_crp = True
            break

    return crp_sequence if found_crp else []


def find_checksum_code(instructions):
    """Find checksum validation code"""

    # Look for loops that read from low addresses (0x00-0x1C)
    # This is harder to detect automatically, so we look for patterns

    checksum_candidates = []

    for i, instr in enumerate(instructions):
        asm = instr['asm'].lower()

        # Look for loads from address 0
        if 'ldr' in asm and ('#0' in asm or ', #0' in asm or '[r' in asm):
            # Get context
            start = max(0, i - 10)
            end = min(len(instructions), i + 30)
            context = instructions[start:end]

            # Check if there's a loop (branch back)
            has_loop = any('b.' in inst['asm'].lower() for inst in context)
            has_add = any('add' in inst['asm'].lower() for inst in context)

            if has_loop and has_add:
                checksum_candidates.append({
                    'addr': instr['addr'],
                    'context': context
                })

    return checksum_candidates


def main():
    if len(sys.argv) < 2:
        print("Usage: extract_boot_sequence.py <disassembly_file>")
        print("\nExample:")
        print("  extract_boot_sequence.py ../bootrom/bootrom_arm_disasm.txt")
        sys.exit(1)

    filename = sys.argv[1]

    print("LPC2468 Boot ROM Sequence Extractor")
    print("=" * 70)
    print(f"Analyzing: {filename}")

    # Parse disassembly
    instructions = parse_disassembly(filename)
    print(f"Found {len(instructions)} instructions")

    # Extract reset entry (0x7FFFE000 - 0x7FFFE020)
    reset_entry = find_boot_sequence(instructions, 0x7FFFE000, 0x7FFFE020)
    print_sequence(reset_entry, "PHASE 1: Reset Entry (0x7FFFE000)")

    # Extract CRP check (0x7FFFE040 - 0x7FFFE080)
    crp_check = find_boot_sequence(instructions, 0x7FFFE040, 0x7FFFE080)
    print_sequence(crp_check, "PHASE 2: CRP Check (0x7FFFE040)")

    # Find CRP read instruction
    print("\nSearching for CRP read instruction...")
    crp_sequence = find_crp_check(instructions)
    if crp_sequence:
        print_sequence(crp_sequence, "CRP Check Code (detected)")

    # Find checksum validation
    print("\nSearching for checksum validation code...")
    checksum_candidates = find_checksum_code(instructions)

    if checksum_candidates:
        print(f"Found {len(checksum_candidates)} potential checksum loops:")
        for i, candidate in enumerate(checksum_candidates[:3]):  # Show first 3
            print(f"\nCandidate {i+1} at 0x{candidate['addr']:08x}:")
            for instr in candidate['context'][:15]:  # Show first 15 instructions
                print(f"  0x{instr['addr']:08x}:  {instr['asm']}")

    print("\n" + "=" * 70)
    print("Analysis complete!")


if __name__ == '__main__':
    main()
