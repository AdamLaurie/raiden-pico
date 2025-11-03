#!/usr/bin/env python3
"""
OpenOCD J-Link trace script to count instructions until first CRP compare

Uses OpenOCD with J-Link Pro to single-step through the LPC bootloader
and count instructions until it reaches the first CRP protection check.

This helps determine the precise timing for glitch attacks.
"""

import subprocess
import time
import re
import sys

# OpenOCD configuration
OPENOCD_CMD = "openocd"
JLINK_INTERFACE = "interface/jlink.cfg"
TARGET_CONFIG = "target/lpc2478.cfg"  # LPC2478 configuration (compatible with LPC2468)

# Known CRP addresses for LPC2000 series
# CRP is stored at 0x000001FC in flash for LPC2xxx
CRP_ADDRESS = 0x000001FC
CRP_VALUES = {
    0x12345678: "NO_ISP",     # CRP1 - Prevents ISP entry
    0x87654321: "CRP1",       # CRP2 - Disables most debug
    0x43218765: "CRP2",       # CRP3 - Full protection
    0x4E697370: "CRP3"        # NO_ISP string constant
}

class OpenOCDSession:
    """Manage OpenOCD session via telnet interface"""

    def __init__(self):
        self.process = None
        self.telnet_port = 4444

    def start(self):
        """Start OpenOCD with J-Link interface"""
        print("Starting OpenOCD with J-Link interface...")

        # Start OpenOCD in background
        cmd = [
            OPENOCD_CMD,
            "-f", JLINK_INTERFACE,
            "-f", TARGET_CONFIG,
            "-c", "init",
            "-c", "halt"
        ]

        try:
            self.process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True
            )

            # Wait for OpenOCD to start
            time.sleep(2)

            if self.process.poll() is not None:
                stdout, stderr = self.process.communicate()
                print(f"ERROR: OpenOCD failed to start")
                print(f"stdout: {stdout}")
                print(f"stderr: {stderr}")
                return False

            print("✓ OpenOCD started successfully")
            return True

        except FileNotFoundError:
            print(f"ERROR: OpenOCD not found. Install with: sudo apt install openocd")
            return False
        except Exception as e:
            print(f"ERROR: Failed to start OpenOCD: {e}")
            return False

    def send_command(self, cmd):
        """Send command to OpenOCD via telnet"""
        try:
            result = subprocess.run(
                ["echo", cmd, "|", "nc", "localhost", str(self.telnet_port)],
                shell=True,
                capture_output=True,
                text=True,
                timeout=5
            )
            return result.stdout
        except subprocess.TimeoutExpired:
            print(f"WARNING: Command timeout: {cmd}")
            return ""
        except Exception as e:
            print(f"ERROR: Failed to send command: {e}")
            return ""

    def stop(self):
        """Stop OpenOCD"""
        if self.process:
            print("\nStopping OpenOCD...")
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
            print("✓ OpenOCD stopped")

def send_ocd_command(cmd):
    """Send command to OpenOCD telnet interface"""
    try:
        # Use subprocess to send command via netcat
        result = subprocess.run(
            f'echo "{cmd}" | nc localhost 4444',
            shell=True,
            capture_output=True,
            text=True,
            timeout=2
        )
        return result.stdout
    except Exception as e:
        print(f"ERROR: Command failed: {e}")
        return ""

def read_register(reg_name):
    """Read ARM register value"""
    output = send_ocd_command(f"reg {reg_name}")
    # Parse output like: "pc (/32): 0x00001234"
    match = re.search(r'0x([0-9a-fA-F]+)', output)
    if match:
        return int(match.group(1), 16)
    return None

def read_memory(address, size=4):
    """Read memory at address (size in bytes)"""
    output = send_ocd_command(f"mdw 0x{address:08x} 1")
    # Parse output like: "0x00000000: 12345678"
    match = re.search(r'0x[0-9a-fA-F]+:\s+([0-9a-fA-F]+)', output)
    if match:
        return int(match.group(1), 16)
    return None

def single_step():
    """Execute single instruction"""
    send_ocd_command("step")
    time.sleep(0.01)  # Small delay for stability

def reset_and_halt():
    """Reset target and halt at first instruction"""
    print("Resetting target and halting...")
    send_ocd_command("reset halt")
    time.sleep(0.5)
    pc = read_register("pc")
    print(f"✓ Halted at PC: 0x{pc:08x}")
    return pc

def disassemble_instruction(address):
    """Disassemble instruction at address"""
    output = send_ocd_command(f"arm disassemble 0x{address:08x} 1")
    # Parse output to get instruction
    lines = output.strip().split('\n')
    for line in lines:
        if '0x' in line and ':' in line:
            # Format: "0x00001234: instruction"
            return line.split(':', 1)[1].strip()
    return ""

def is_crp_compare(instruction, pc, prev_pc):
    """
    Detect if instruction is related to CRP comparison

    The bootloader typically:
    1. Loads CRP value from 0x2FC
    2. Compares against known CRP constants
    3. Branches based on result
    """
    instruction_lower = instruction.lower()

    # Check for memory loads from CRP address area
    if 'ldr' in instruction_lower:
        # Check if loading from/near CRP address
        if f'0x{CRP_ADDRESS:x}' in instruction_lower or '0x1fc' in instruction_lower:
            return True, "Loading CRP value from flash"

    # Check for comparison instructions
    if any(x in instruction_lower for x in ['cmp', 'cmn', 'tst', 'teq']):
        return True, "Comparison instruction (potential CRP check)"

    # Check for conditional branches after comparison
    if any(x in instruction_lower for x in ['beq', 'bne', 'bcs', 'bcc', 'bmi', 'bpl']):
        return True, "Conditional branch (potential CRP decision)"

    return False, ""

def trace_to_crp_check(max_instructions=1000):
    """
    Single-step through bootloader until CRP check is found

    Returns:
        tuple: (instruction_count, pc_at_crp_check, instruction)
    """
    print("\n" + "="*70)
    print("Starting instruction trace to find CRP check...")
    print("="*70)

    # Reset and halt at bootloader entry
    start_pc = reset_and_halt()

    instruction_count = 0
    prev_pc = start_pc
    crp_related_instructions = []

    print(f"\nSingle-stepping from 0x{start_pc:08x}...\n")

    for i in range(max_instructions):
        # Read current PC
        pc = read_register("pc")
        if pc is None:
            print("ERROR: Failed to read PC")
            break

        # Disassemble current instruction
        instruction = disassemble_instruction(pc)

        # Check if this is CRP-related
        is_crp, reason = is_crp_compare(instruction, pc, prev_pc)

        # Print every 10th instruction, or CRP-related ones
        if i % 10 == 0 or is_crp:
            print(f"[{instruction_count:4d}] 0x{pc:08x}: {instruction:30s}", end="")
            if is_crp:
                print(f" <- {reason}", end="")
            print()

        if is_crp:
            crp_related_instructions.append({
                'count': instruction_count,
                'pc': pc,
                'instruction': instruction,
                'reason': reason
            })

            # If we've found a few CRP-related instructions, the first one is likely the check
            if len(crp_related_instructions) >= 3:
                print("\n" + "="*70)
                print("FOUND CRP CHECK SEQUENCE!")
                print("="*70)
                print(f"\nCRP-related instructions:")
                for crp_instr in crp_related_instructions:
                    print(f"  [{crp_instr['count']:4d}] 0x{crp_instr['pc']:08x}: {crp_instr['instruction']:30s} <- {crp_instr['reason']}")

                first_crp = crp_related_instructions[0]
                print(f"\n✓ First CRP check at instruction #{first_crp['count']}")
                print(f"  PC: 0x{first_crp['pc']:08x}")
                print(f"  Instruction: {first_crp['instruction']}")

                return first_crp['count'], first_crp['pc'], first_crp['instruction']

        # Single step
        single_step()
        instruction_count += 1
        prev_pc = pc

        # Check for infinite loops (PC not changing)
        if i > 10 and pc == prev_pc:
            print(f"\nWARNING: PC stuck at 0x{pc:08x}, possible infinite loop")
            break

    print(f"\n✗ No clear CRP check found in first {instruction_count} instructions")

    if crp_related_instructions:
        print(f"\nFound {len(crp_related_instructions)} potentially CRP-related instructions:")
        for crp_instr in crp_related_instructions:
            print(f"  [{crp_instr['count']:4d}] 0x{crp_instr['pc']:08x}: {crp_instr['instruction']:30s}")

    return None, None, None

def main():
    """Main entry point"""
    print("LPC Bootloader CRP Trace Tool")
    print("Using OpenOCD + J-Link to count instructions until CRP check\n")

    # Start OpenOCD
    ocd = OpenOCDSession()
    if not ocd.start():
        return 1

    try:
        # Read and display CRP value
        print("\nReading CRP value from flash...")
        crp_value = read_memory(CRP_ADDRESS)
        if crp_value is not None:
            crp_name = CRP_VALUES.get(crp_value, "UNKNOWN")
            print(f"✓ CRP at 0x{CRP_ADDRESS:08x}: 0x{crp_value:08x} ({crp_name})")
        else:
            print("✗ Failed to read CRP value")

        # Trace to CRP check
        count, pc, instruction = trace_to_crp_check(max_instructions=2000)

        if count is not None:
            print("\n" + "="*70)
            print("SUMMARY")
            print("="*70)
            print(f"Instructions until CRP check: {count}")
            print(f"CRP check at PC: 0x{pc:08x}")
            print(f"Instruction: {instruction}")
            print(f"\nFor glitch timing:")
            print(f"  - Target instruction count: ~{count}")
            print(f"  - Consider glitching a few instructions before ({count-5} to {count})")
            print("="*70)

    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
    finally:
        ocd.stop()

    return 0

if __name__ == "__main__":
    sys.exit(main())
