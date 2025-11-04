#!/usr/bin/env python3
"""
Write CRP protection value to LPC2468 flash via OpenOCD

Sets CRP protection level by writing to flash address 0x1FC.
Requires the target to be unlocked (no CRP active) before running.

Usage:
    ./write_crp_protection.py <crp_level>

    crp_level: NO_ISP, CRP1, CRP2, or CRP3
"""

import subprocess
import sys
import re

# CRP protection values (NXP LPC2xxx standard values)
CRP_VALUES = {
    "CRP0": 0xDEADBEEF,    # No protection (any value that's not CRP1/2/3/NO_ISP)
    "CRP1": 0x12345678,    # JTAG/SWD disabled, limited ISP (sector 0 cannot be erased/written)
    "CRP2": 0x87654321,    # JTAG/SWD disabled, only allows full chip erase via reduced ISP commands
    "CRP3": 0x43218765,    # Maximum protection, JTAG/SWD disabled, ISP entry disabled
    "NO_ISP": 0x4E697370,  # "Nisp" - ISP entry disabled, JTAG/SWD enabled, flash read allowed
}

CRP_ADDRESS = 0x000001FC

def run_openocd_commands(commands):
    """Run OpenOCD with a list of commands"""
    cmd = [
        "openocd",
        "-f", "/usr/share/openocd/scripts/interface/jlink.cfg",
        "-f", "/usr/share/openocd/scripts/target/lpc2478.cfg",
    ]

    for c in commands:
        cmd.extend(["-c", c])

    cmd.extend(["-c", "exit"])

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.stdout + result.stderr, result.returncode
    except subprocess.TimeoutExpired:
        return "TIMEOUT", -1
    except Exception as e:
        return f"ERROR: {e}", -1

def read_crp_value():
    """Read current CRP value from flash - tries multiple halt methods"""
    print(f"Reading current CRP value from 0x{CRP_ADDRESS:08x}...")

    halt_methods = ["soft_reset_halt", "halt", "reset init"]

    for idx, method in enumerate(halt_methods):
        if idx > 0:
            print(f"  Trying halt method: {method}")

        commands = [
            "init",
            "arm7_9 fast_memory_access enable",
            method,
            f"mdw 0x{CRP_ADDRESS:08x} 1"
        ]

        output, returncode = run_openocd_commands(commands)

        # Parse output for value
        match = re.search(r'0x[0-9a-fA-F]+:\s+([0-9a-fA-F]+)', output)
        if match:
            value = int(match.group(1), 16)

            # Find CRP name
            crp_name = "UNKNOWN"
            for name, val in CRP_VALUES.items():
                if val == value:
                    crp_name = name
                    break

            print(f"✓ Current CRP value: 0x{value:08x} ({crp_name})")
            return value, crp_name
        elif "data abort" not in output.lower() and idx < len(halt_methods) - 1:
            # If not a data abort, might be worth trying another method
            print(f"  Read failed, trying next halt method...")

    # All methods failed
    print(f"✗ Could not read CRP value")
    print(f"OpenOCD output:\n{output}")
    return None, None

def write_crp_value(crp_level):
    """Write CRP protection value to flash"""
    if crp_level not in CRP_VALUES:
        print(f"✗ Invalid CRP level: {crp_level}")
        print(f"Valid levels: {', '.join(CRP_VALUES.keys())}")
        return False

    crp_value = CRP_VALUES[crp_level]

    print(f"\nWriting CRP protection level: {crp_level}")
    print(f"Value: 0x{crp_value:08x}")
    print(f"Address: 0x{CRP_ADDRESS:08x}")
    print()

    # OpenOCD commands to write to flash
    commands = [
        "init",
        "soft_reset_halt",
        # Unlock flash
        "flash probe 0",
        # Write the CRP value (need to write full sector, so read-modify-write)
        # For LPC2xxx, we need to use flash write_image or modify memory
        # The safest way is to write the word directly
        f"mww 0x{CRP_ADDRESS:08x} 0x{crp_value:08x}",
        # Verify the write
        f"mdw 0x{CRP_ADDRESS:08x} 1"
    ]

    print("Executing OpenOCD commands...")
    output, returncode = run_openocd_commands(commands)

    # Check if write was successful by reading back
    match = re.search(r'0x[0-9a-fA-F]+:\s+([0-9a-fA-F]+)', output)
    if match:
        read_value = int(match.group(1), 16)

        if read_value == crp_value:
            print(f"✓ Successfully wrote CRP value: 0x{read_value:08x}")
            print(f"✓ CRP protection level: {crp_level}")
            print()
            print("WARNING: This is in RAM. To persist to flash, you need to:")
            print("  1. Erase and reprogram the flash sector containing 0x1FC")
            print("  2. Or use OpenOCD flash commands to write the entire sector")
            return True
        else:
            print(f"✗ Write verification failed!")
            print(f"  Expected: 0x{crp_value:08x}")
            print(f"  Read back: 0x{read_value:08x}")
            return False
    else:
        print(f"✗ Could not verify write")
        print(f"OpenOCD output:\n{output}")
        return False

def try_flash_write_with_halt_method(halt_method, dump_file):
    """Try erasing and writing flash with a specific halt method"""
    commands = [
        "init",
        "arm7_9 fast_memory_access enable",  # Enable fast memory access for ARM7
        halt_method,
        "flash probe 0",
        f"flash erase_address 0x0 0x1000",  # Erase first 4KB
        f"flash write_image {dump_file} 0x0",
        "verify_image " + dump_file + " 0x0",
    ]

    output, returncode = run_openocd_commands(commands)

    # Check for success - need BOTH write and verify
    has_write = "wrote" in output.lower()
    has_verify = "verified" in output.lower() and "bytes" in output.lower()

    if has_write and has_verify:
        return True, "success"
    elif "Halt timed out" in output or "halt timed out" in output.lower():
        return False, "halt_timeout"
    elif "failed erasing" in output.lower():
        return False, "erase_failed"
    elif "failed writing" in output.lower() or "failed flashing" in output.lower():
        return False, "write_failed"
    elif "verify failed" in output.lower():
        return False, "verify_failed"
    elif "error" in output.lower():
        return False, "error"
    else:
        return False, "unknown"

def write_crp_to_flash(crp_level):
    """Write CRP value to flash (persistent)"""
    if crp_level not in CRP_VALUES:
        print(f"✗ Invalid CRP level: {crp_level}")
        return False

    crp_value = CRP_VALUES[crp_level]

    print(f"\nWriting CRP {crp_level} (0x{crp_value:08x}) to FLASH at 0x{CRP_ADDRESS:08x}")
    print()
    print("This will:")
    print("  1. Read the current flash sector (first 8KB)")
    print("  2. Modify byte at offset 0x1FC with CRP value")
    print("  3. Erase the flash sector")
    print("  4. Reprogram with modified data")
    print()

    # First, dump the current flash sector - try multiple halt methods
    print("Step 1: Reading current flash sector...")
    dump_file = "/tmp/flash_sector0.bin"

    halt_methods = ["soft_reset_halt", "halt", "reset init"]
    dump_success = False

    for idx, method in enumerate(halt_methods):
        if idx > 0:
            print(f"  Trying halt method: {method}")

        commands = [
            "init",
            "arm7_9 fast_memory_access enable",
            method,
            f"dump_image {dump_file} 0x0 0x1000",  # First 4KB sector
        ]

        output, returncode = run_openocd_commands(commands)

        if "dumped" in output:
            print(f"✓ Dumped flash sector to {dump_file}")
            dump_success = True
            break
        elif "data abort" not in output.lower() and idx < len(halt_methods) - 1:
            print(f"  Dump failed, trying next halt method...")

    if not dump_success:
        print(f"✗ Failed to dump flash sector with all halt methods")
        print(f"Last output: {output}")
        return False

    # Modify the CRP location in the dumped file
    print(f"\nStep 2: Modifying CRP value in dumped image...")
    try:
        with open(dump_file, 'rb') as f:
            flash_data = bytearray(f.read())

        # Write CRP value at offset 0x1FC (little-endian)
        flash_data[0x1FC:0x200] = crp_value.to_bytes(4, byteorder='little')

        with open(dump_file, 'wb') as f:
            f.write(flash_data)

        print(f"✓ Modified CRP value to 0x{crp_value:08x}")
    except Exception as e:
        print(f"✗ Failed to modify image: {e}")
        return False

    # Try different halt methods for erase/write
    print(f"\nStep 3: Erasing and reprogramming flash...")
    print("WARNING: This will erase the flash! Make sure you have a backup!")
    print()

    halt_methods = [
        ("soft_reset_halt", "Soft reset + halt"),
        ("halt", "Direct halt"),
        ("reset init", "Reset init"),
    ]

    for idx, (method, description) in enumerate(halt_methods):
        if idx > 0:
            print(f"\nTrying alternative method #{idx+1}: {description}")
        else:
            print(f"Using OpenOCD command: {method}")

        success, result = try_flash_write_with_halt_method(method, dump_file)

        if success:
            print(f"✓ Successfully wrote CRP value to flash!")
            print(f"✓ CRP protection level: {crp_level}")
            print()
            print("IMPORTANT: Power cycle the target for CRP to take effect!")
            return True

        # If we got halt timeout or erase failed, try next method
        if result == "halt_timeout":
            print(f"✗ Method failed: Halt timeout")
            if idx < len(halt_methods) - 1:
                print(f"  -> Trying next method...")
        elif result == "erase_failed":
            print(f"✗ Method failed: Flash erase failed")
            if idx < len(halt_methods) - 1:
                print(f"  -> Trying next method...")
        else:
            print(f"✗ Method failed: {result}")
            if idx < len(halt_methods) - 1:
                print(f"  -> Trying next method...")

    # All methods failed - print last output for debugging
    print("\n✗ All halt methods failed")
    print("✗ Flash write/verify failed")
    print(f"\nLast OpenOCD output (for debugging):")
    print("=" * 70)
    # Get output from last attempt
    commands = [
        "init",
        "arm7_9 fast_memory_access enable",
        halt_methods[-1][0],
        "flash probe 0",
        f"flash erase_address 0x0 0x1000",
        f"flash write_image {dump_file} 0x0",
        "verify_image " + dump_file + " 0x0",
    ]
    debug_output, _ = run_openocd_commands(commands)
    print(debug_output[-2000:])  # Last 2000 chars
    print("=" * 70)
    return False

def main():
    if len(sys.argv) < 2:
        print("Usage: ./write_crp_protection.py <crp_level> [--flash]")
        print()
        print("CRP Levels:")
        for name, value in CRP_VALUES.items():
            print(f"  {name:8s} = 0x{value:08x}")
        print()
        print("Options:")
        print("  --flash    Write to flash (persistent, erases flash!)")
        print("  (default)  Write to RAM only (for testing)")
        return 1

    crp_level = sys.argv[1].upper()
    write_to_flash = "--flash" in sys.argv

    print("="*70)
    print("LPC2468 CRP Protection Writer")
    print("="*70)
    print()

    # Read current value
    current_value, current_name = read_crp_value()

    if current_value is None:
        print("\n✗ Failed to read current CRP value")
        print("Make sure the target is connected and unlocked!")
        return 1

    # Check if already at desired level
    if current_name == crp_level:
        print(f"\n⚠ CRP is already set to {crp_level}")
        return 0

    # Write new value
    if write_to_flash:
        success = write_crp_to_flash(crp_level)
    else:
        success = write_crp_value(crp_level)

    if not success:
        return 1

    # Verify
    print("\nVerifying...")
    new_value, new_name = read_crp_value()

    if new_name == crp_level:
        print(f"\n✓ SUCCESS! CRP protection set to {crp_level}")
        if not write_to_flash:
            print("\nNOTE: This is a RAM write only. To make it persistent:")
            print(f"  Run with --flash option: ./write_crp_protection.py {crp_level} --flash")
        return 0
    else:
        print(f"\n✗ FAILED! CRP is {new_name}, expected {crp_level}")
        return 1

if __name__ == "__main__":
    sys.exit(main())
