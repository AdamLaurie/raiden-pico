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

def run_openocd_command(commands):
    """Run OpenOCD with a series of commands"""
    cmd = ["openocd", "-f", JLINK_INTERFACE, "-f", TARGET_CONFIG]
    for c in commands:
        cmd.extend(["-c", c])
    cmd.extend(["-c", "exit"])

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
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

def trace_instructions(max_steps=500):
    """
    Single-step through bootloader and look for CRP-related operations

    This uses a simpler approach: step N times and dump register/disassembly
    """
    print(f"\nSingle-stepping through first {max_steps} instructions...")
    print("Looking for CRP-related operations (LDR from 0x1FC, CMP, branches)\n")

    # Build OpenOCD command sequence for stepping
    commands = [
        "init",
        "reset halt",
    ]

    # Add step + reg dump for each instruction
    for i in range(max_steps):
        commands.extend([
            f"arm disassemble 0x0 1",  # Will disassemble current PC
            "reg pc",
            "step"
        ])

    # Run all at once (faster than individual calls)
    print("Executing trace...")
    output = run_openocd_command(commands)

    # Parse output
    lines = output.split('\n')
    instructions = []
    current_pc = None
    current_inst = None

    for line in lines:
        # Look for PC value: "pc (/32): 0x00000123"
        pc_match = re.search(r'pc \(/32\):\s+0x([0-9a-fA-F]+)', line)
        if pc_match:
            current_pc = int(pc_match.group(1), 16)
            if current_inst and current_pc:
                instructions.append((current_pc, current_inst))
            current_inst = None

        # Look for disassembly
        inst_match = re.search(r'0x[0-9a-fA-F]+:\s+(.+)', line)
        if inst_match and not pc_match:  # Not the pc line itself
            current_inst = inst_match.group(1).strip()

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

        # Print every 50th instruction or CRP-related ones
        if idx % 50 == 0 or is_crp:
            print(f"[{idx:4d}] 0x{pc:08x}: {inst:40s}", end="")
            if is_crp:
                print(f" <- {reason}", end="")
                crp_candidates.append((idx, pc, inst, reason))
            print()

    return crp_candidates

def main():
    print("="*70)
    print("LPC Bootloader CRP Trace Tool (Simplified)")
    print("="*70)

    # Read CRP value
    crp_value = read_crp_value()

    # Trace instructions
    candidates = trace_instructions(max_steps=500)

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
        print("\n✗ No clear CRP-related instructions found in first 500 steps")

    return 0

if __name__ == "__main__":
    sys.exit(main())
