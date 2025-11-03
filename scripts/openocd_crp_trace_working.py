#!/usr/bin/env python3
"""
Simplified OpenOCD J-Link trace script to count instructions until first CRP compare

Uses OpenOCD command-line mode for reliability.
"""

import subprocess
import re
import sys

# OpenOCD configuration
JLINK_INTERFACE = "interface/jlink.cfg"
TARGET_CONFIG = "target/lpc2478.cfg"
CRP_ADDRESS = 0x000001FC
BOOTLOADER_ROM_ADDRESS = 0x7FFF0000  # LPC2xxx internal bootloader ROM

def run_openocd_command(commands):
    """Run OpenOCD with a series of commands"""
    cmd = ["openocd", "-f", JLINK_INTERFACE, "-f", TARGET_CONFIG]
    for c in commands:
        cmd.extend(["-c", c])
    cmd.extend(["-c", "exit"])

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        return result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    except Exception as e:
        return f"ERROR: {e}"

def read_crp_value():
    """Read CRP protection value from flash"""
    print("Reading CRP value...")
    output = run_openocd_command([
        "init",
        "halt",
        f"mdw 0x{CRP_ADDRESS:08x} 1"
    ])

    # Parse output for value
    match = re.search(r'0x[0-9a-fA-F]+:\s+([0-9a-fA-F]+)', output)
    if match:
        value = int(match.group(1), 16)
        crp_names = {
            0x12345678: "NO_ISP",
            0x87654321: "CRP1",
            0x43218765: "CRP2",
            0x4E697370: "CRP3"
        }
        name = crp_names.get(value, "UNKNOWN")
        print(f"✓ CRP value: 0x{value:08x} ({name})")
        return value
    else:
        print("✗ Could not read CRP value")
        return None

def trace_instructions(max_steps=100):
    """
    Single-step through bootloader and look for CRP-related operations

    Returns:
        tuple: (crp_candidates, total_instructions_traced)
    """
    print(f"\nSingle-stepping through first {max_steps} instructions...")
    print("Looking for CRP-related operations (LDR from 0x1FC, CMP, branches)\n")

    # Step 1: Reset to address 0x0 and collect PC values
    # Enable fast memory access to avoid timeout issues
    # The bootloader startup code is at 0x0 (flash), which includes CRP checking logic
    print("Resetting to flash start (0x00000000)...")
    print("This is where the bootloader startup code runs, including CRP checks")
    commands = [
        "init",
        # Using default speed (500 kHz from lpc2478.cfg) - faster than 100 kHz
        "arm7_9 fast_memory_access enable",  # Critical: prevents timeout issues
        "reset halt"  # Reset to 0x0
    ]

    for i in range(max_steps):
        commands.extend(["reg pc", "step"])

    print("Collecting PC values...")
    output = run_openocd_command(commands)

    # Parse PC values
    pcs = []
    for line in output.split('\n'):
        # Look for PC value: "pc (/32): 0x00000123"
        pc_match = re.search(r'pc \(/32\):\s+0x([0-9a-fA-F]+)', line)
        if pc_match:
            pc = int(pc_match.group(1), 16)
            pcs.append(pc)

    print(f"✓ Collected {len(pcs)} PC values")

    if len(pcs) == 0:
        print("✗ No PC values collected!")
        return []

    # Step 2: Disassemble unique addresses in batches
    print("Disassembling instructions...")
    unique_pcs = sorted(set(pcs))
    disassembly = {}

    batch_size = 50
    for i in range(0, len(unique_pcs), batch_size):
        batch = unique_pcs[i:i+batch_size]
        commands = ["init", "reset halt"]
        for pc in batch:
            commands.append(f"arm disassemble 0x{pc:x} 1")

        batch_output = run_openocd_command(commands)

        # Parse disassembly output: "0x00000000  4018 e59f	ldr	r4, [pc, #0x18]"
        for line in batch_output.split('\n'):
            match = re.match(r'0x([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+(.+)', line)
            if match:
                addr = int(match.group(1), 16)
                inst = match.group(2).strip()
                disassembly[addr] = inst

        if (i + batch_size) < len(unique_pcs):
            print(f"  Disassembled {i + batch_size}/{len(unique_pcs)} unique addresses...")

    print(f"✓ Disassembled {len(disassembly)} unique addresses\n")

    # Step 3: Build instructions list
    instructions = []
    for pc in pcs:
        inst = disassembly.get(pc, "???")
        instructions.append((pc, inst))

    print(f"✓ Traced {len(instructions)} instructions\n")

    # Analyze for CRP-related operations
    crp_candidates = []
    for idx, (pc, inst) in enumerate(instructions):
        inst_lower = inst.lower()

        # Check for CRP-related operations
        is_crp = False
        reason = ""

        if 'ldr' in inst_lower and ('0x1fc' in inst_lower or '1fc' in inst_lower):
            is_crp = True
            reason = "Loading from CRP address (0x1FC)"
        elif 'ldr' in inst_lower and idx < len(instructions) - 3:
            # Check if next few instructions do comparisons
            next_insts = ' '.join([instructions[idx+j][1].lower() for j in range(1, min(4, len(instructions)-idx))])
            if any(x in next_insts for x in ['cmp', 'tst', 'teq']):
                is_crp = True
                reason = "LDR followed by comparison"
        elif any(x in inst_lower for x in ['cmp', 'cmn', 'tst', 'teq']):
            is_crp = True
            reason = "Comparison instruction"
        elif any(x in inst_lower for x in ['beq', 'bne', 'bcs', 'bcc', 'bmi', 'bpl', 'bhi', 'bls']):
            is_crp = True
            reason = "Conditional branch"

        # Print all instructions (since we only have ~5 total)
        print(f"[{idx:4d}] 0x{pc:08x}: {inst:40s}", end="")
        if is_crp:
            print(f" <- {reason}", end="")
            crp_candidates.append((idx, pc, inst, reason))
        print()

    return crp_candidates, len(instructions)

def main():
    print("="*70)
    print("LPC Bootloader CRP Trace Tool")
    print("="*70)

    # Read CRP value
    crp_value = read_crp_value()

    # Trace instructions
    candidates, num_traced = trace_instructions(max_steps=500)

    if candidates:
        print("\n" + "="*70)
        print("CRP-RELATED INSTRUCTIONS FOUND:")
        print("="*70)
        for idx, pc, inst, reason in candidates:
            print(f"[{idx:4d}] 0x{pc:08x}: {inst:40s} <- {reason}")

        first = candidates[0]
        print("\n" + "="*70)
        print("FIRST CRP-RELATED INSTRUCTION:")
        print("="*70)
        print(f"  Instruction #{first[0]}")
        print(f"  PC: 0x{first[1]:08x}")
        print(f"  Instruction: {first[2]}")
        print(f"  Reason: {first[3]}")
        print("\nFor glitch timing, target around instruction #" + str(first[0]))
        print("="*70)
    else:
        print("\n" + "="*70)
        print("ANALYSIS:")
        print("="*70)
        print(f"No CRP-related instructions found in the {num_traced} instructions traced.")
        print("\nThe CRP check may occur later in the boot sequence.")
        print("Consider:")
        print("  1. Increasing max_steps in the script")
        print("  2. Looking for indirect CRP checks (function calls, branches)")
        print("  3. Static analysis of the flash image")
        print("="*70)

    return 0

if __name__ == "__main__":
    sys.exit(main())
