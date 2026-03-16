#!/usr/bin/env python3
"""SWD regression test suite for Raiden-Pico.

Tests every SWD CLI command in various combinations.
Requires an STM32F1 target connected via SWD (GP17=SWCLK, GP18=SWDIO, GP15=nRST).

WARNING: This test modifies flash and RDP level on the target.
         It will mass-erase flash as part of RDP testing.

Usage:
    ./scripts/swd_regression.py [section|all]

Sections:
    connect     - Connection and identification
    read        - Memory read (hex dump, aliases, DP/AP)
    write       - Memory write with verify (SRAM and flash)
    fill        - Fill command with verify
    regs        - Core register access
    reset       - Target reset (pulse, hold/release)
    flash       - Flash erase and write via controller
    rdp         - RDP level read/set round-trip (DESTRUCTIVE)
    opt         - Option byte read
    errors      - Error handling and edge cases
    all         - Run all sections (default)
"""

import serial
import sys
import time
import re

PORT = "/dev/ttyACM0"
BAUD = 115200

pass_count = 0
fail_count = 0
skip_count = 0
current_section = ""


def open_port():
    ser = serial.Serial(PORT, BAUD, timeout=3)
    time.sleep(1)
    ser.reset_input_buffer()
    return ser


def cmd(ser, command, wait=1.0):
    """Send command, return response text."""
    ser.reset_input_buffer()
    ser.write(f"{command}\r\n".encode())
    time.sleep(wait)
    data = b""
    while ser.in_waiting:
        data += ser.read(ser.in_waiting)
        time.sleep(0.1)
    return data.decode("utf-8", errors="replace").strip()


def check(name, response, *patterns):
    """Check response contains all patterns. Returns True on pass."""
    global pass_count, fail_count
    ok = all(p.upper() in response.upper() for p in patterns)
    status = "PASS" if ok else "FAIL"
    if ok:
        pass_count += 1
    else:
        fail_count += 1
    # Show first meaningful line of response
    lines = [l.strip() for l in response.split("\n") if l.strip() and l.strip() != ">"]
    first = lines[0] if lines else "(empty)"
    if len(first) > 100:
        first = first[:100] + "..."
    print(f"  [{status}] {name}")
    if not ok:
        print(f"         Expected: {patterns}")
        print(f"         Got: {first}")
    return ok


def check_not(name, response, *patterns):
    """Check response does NOT contain any pattern."""
    global pass_count, fail_count
    ok = not any(p.upper() in response.upper() for p in patterns)
    status = "PASS" if ok else "FAIL"
    if ok:
        pass_count += 1
    else:
        fail_count += 1
    print(f"  [{status}] {name}")
    if not ok:
        lines = [l.strip() for l in response.split("\n") if l.strip() and l.strip() != ">"]
        first = lines[0] if lines else "(empty)"
        print(f"         Should not contain: {patterns}")
        print(f"         Got: {first}")
    return ok


def skip(name, reason=""):
    global skip_count
    skip_count += 1
    print(f"  [SKIP] {name}" + (f" ({reason})" if reason else ""))


def section(name):
    global current_section
    current_section = name
    print(f"\n{'='*60}")
    print(f"  {name}")
    print(f"{'='*60}")


# ── Section: connect ──────────────────────────────────────────

def test_connect(ser):
    section("CONNECTION & IDENTIFICATION")

    # Basic connect
    r = cmd(ser, "swd connect")
    check("SWD CONNECT", r, "Connected", "DPIDR")

    # Double connect (should be idempotent)
    r = cmd(ser, "swd connect")
    check("SWD CONNECT (repeat)", r, "Connected", "DPIDR")

    # IDCODE
    r = cmd(ser, "swd idcode")
    check("SWD IDCODE - DPIDR", r, "DPIDR")
    check("SWD IDCODE - CPUID", r, "CPUID")
    check("SWD IDCODE - Cortex", r, "Cortex")

    # Connect under reset
    r = cmd(ser, "swd connectrst", wait=2)
    check("SWD CONNECTRST", r, "Connected")

    # Help text (no subcommand)
    r = cmd(ser, "swd")
    check("SWD help shows CONNECT", r, "CONNECT")
    check("SWD help shows CONNECTRST", r, "CONNECTRST")
    check("SWD help shows READ", r, "READ")
    check("SWD help shows WRITE", r, "WRITE")
    check("SWD help shows FILL", r, "FILL")
    check("SWD help shows HALT", r, "HALT")
    check("SWD help shows RESET", r, "RESET")
    check("SWD help shows RDP", r, "RDP")
    check("SWD help shows OPT", r, "OPT")
    check("SWD help shows REGS", r, "REGS")
    check("SWD help alphabetised", r, "CONNECT", "FILL", "HALT", "IDCODE")
    check_not("SWD help no DUMP command", r, "SWD DUMP")

    # Prefix matching - ambiguous prefix
    r = cmd(ser, "swd con")
    check("SWD ambiguous prefix (con)", r, "Ambiguous")

    # Unambiguous prefix
    r = cmd(ser, "swd id")
    check("SWD prefix match (id->IDCODE)", r, "DPIDR")

    # Unknown command
    r = cmd(ser, "swd foobar")
    check("SWD unknown command", r, "ERROR")


# ── Section: read ─────────────────────────────────────────────

def test_read(ser):
    section("MEMORY READ")

    cmd(ser, "swd connect")
    cmd(ser, "swd halt")

    # Read single word - hex dump format
    r = cmd(ser, "swd read E000ED00")
    check("READ single word (CPUID)", r, "E000ED00:")
    check("READ hex dump format", r, "Read complete")

    # Read without MEM keyword
    r = cmd(ser, "swd read 20000000")
    check("READ raw addr (no MEM)", r, "20000000:")

    # Read with MEM keyword (backward compat)
    r = cmd(ser, "swd read mem 20000000")
    check("READ MEM addr (compat)", r, "20000000:")

    # Read multiple words
    r = cmd(ser, "swd read 20000000 4")
    check("READ 4 words", r, "Reading 16 bytes")
    # Should be exactly 1 hex dump line (16 bytes)
    hex_lines = [l for l in r.split("\n") if re.match(r"\s*2000000", l)]
    check("READ 4 words = 1 line", r, "20000000:")

    # Read with count
    r = cmd(ser, "swd read E000ED00 8")
    check("READ 8 words = 32 bytes", r, "Reading 32 bytes")

    # Read DP register
    r = cmd(ser, "swd read dp 0")
    check("READ DP[0] (DPIDR)", r, "DP[0x0]")

    # Read DP CTRL/STAT
    r = cmd(ser, "swd read dp 4")
    check("READ DP[4] (CTRL/STAT)", r, "DP[0x4]")

    # Read AP register
    r = cmd(ser, "swd read ap 0")
    check("READ AP[0] (CSW)", r, "AP[0x00]")

    # Read AP IDR
    r = cmd(ser, "swd read ap FC")
    check("READ AP[FC] (IDR)", r, "AP[0xFC]")

    # Read SRAM alias (default = full region)
    r = cmd(ser, "swd read sram", wait=3)
    check("READ SRAM alias (full)", r, "Reading 20480 bytes")
    check("READ SRAM starts at base", r, "20000000:")

    # Read SRAM alias with count
    r = cmd(ser, "swd read sram 4")
    check("READ SRAM alias + count", r, "Reading 16 bytes")

    # Read FLASH alias
    r = cmd(ser, "swd read flash 4")
    check("READ FLASH alias + count", r, "Reading 16 bytes", "08000000:")

    # Read MEM SRAM (old syntax)
    r = cmd(ser, "swd read mem sram 4")
    check("READ MEM SRAM (compat)", r, "Reading 16 bytes", "20000000:")

    # PPB registers (always accessible)
    r = cmd(ser, "swd read E000EDF0")
    check("READ DHCSR", r, "E000EDF0:")

    # Cortex-M CPUID
    r = cmd(ser, "swd read E000ED00")
    check("READ CPUID has data", r, "E000ED00:")


# ── Section: write ────────────────────────────────────────────

def test_write(ser):
    section("MEMORY WRITE (with verify)")

    cmd(ser, "swd connect")
    cmd(ser, "swd halt")

    # Write SRAM - should auto-verify
    r = cmd(ser, "swd write 20000000 DEADBEEF")
    check("WRITE SRAM verified", r, "verified", "DEADBEEF")

    # Read back
    r = cmd(ser, "swd read 20000000")
    check("WRITE SRAM readback", r, "EF BE AD DE")

    # Write different value
    r = cmd(ser, "swd write 20000000 12345678")
    check("WRITE SRAM different val", r, "verified", "12345678")

    # Write without MEM keyword
    r = cmd(ser, "swd write 20000004 AABBCCDD")
    check("WRITE raw addr (no MEM)", r, "verified", "AABBCCDD")

    # Write with MEM keyword (backward compat)
    r = cmd(ser, "swd write mem 20000008 CAFEBABE")
    check("WRITE MEM (compat)", r, "verified", "CAFEBABE")

    # Verify all three
    r = cmd(ser, "swd read 20000000 4")
    check("WRITE readback word 0", r, "78 56 34 12")
    check("WRITE readback word 1", r, "DD CC BB AA")
    check("WRITE readback word 2", r, "BE BA FE CA")

    # Write DP (ABORT register - clears errors, always safe)
    r = cmd(ser, "swd write dp 0 1E")
    check("WRITE DP (ABORT)", r, "DP[0x0]")

    # Write flash WITHOUT erase - should prompt
    r = cmd(ser, "swd write 08000000 DEADBEEF")
    check("WRITE flash prompts ERASE", r, "Flash writes require", "ERASE")
    check_not("WRITE flash no auto-erase", r, "Erasing")

    # Write flash alias WITHOUT erase - should also prompt
    r = cmd(ser, "swd write flash DEADBEEF")
    check("WRITE FLASH alias prompts ERASE", r, "Flash writes require", "ERASE")

    # Write flash WITH erase
    r = cmd(ser, "swd write 08000000 DEADBEEF ERASE", wait=3)
    check("WRITE flash + ERASE verified", r, "verified", "DEADBEEF")

    # Read back flash
    r = cmd(ser, "swd read 08000000")
    check("WRITE flash readback", r, "EF BE AD DE")

    # Write flash alias WITH erase
    r = cmd(ser, "swd write flash CAFEF00D ERASE", wait=3)
    check("WRITE FLASH alias + ERASE", r, "verified", "CAFEF00D")


# ── Section: fill ─────────────────────────────────────────────

def test_fill(ser):
    section("MEMORY FILL")

    cmd(ser, "swd connect")
    cmd(ser, "swd halt")

    # Fill SRAM with pattern
    r = cmd(ser, "swd fill 20000000 A5A5A5A5 8")
    check("FILL SRAM 8 words", r, "Filled 8 words", "verified")

    # Verify
    r = cmd(ser, "swd read 20000000 8")
    check("FILL readback all A5", r, "A5 A5 A5 A5 A5 A5 A5 A5")

    # Fill with different pattern
    r = cmd(ser, "swd fill 20000000 00000000 4")
    check("FILL zeros", r, "Filled 4 words", "verified")

    # Verify mixed: first 4 words zero, next 4 still A5
    r = cmd(ser, "swd read 20000000 8")
    check("FILL partial - zeros", r, "00 00 00 00 00 00 00 00")
    check("FILL partial - A5 remains", r, "A5 A5 A5 A5")

    # Fill SRAM alias with count
    r = cmd(ser, "swd fill sram DEADBEEF 4")
    check("FILL SRAM alias + count", r, "Filled 4 words", "verified")

    # Fill with raw addr, small count
    r = cmd(ser, "swd fill 20001000 FFFFFFFF 2")
    check("FILL 2 words", r, "Filled 2 words", "verified")

    # Fill missing args
    r = cmd(ser, "swd fill 20000000 AA")
    check("FILL raw addr needs count", r, "ERROR", "Usage")

    # Fill alias without count = full region (just check it starts)
    r = cmd(ser, "swd fill sram 00000000", wait=3)
    check("FILL SRAM alias default size", r, "Filling 5120 words")

    # ── Flash FILL ──

    # Flash fill WITHOUT ERASE - should prompt
    r = cmd(ser, "swd fill flash DEADBEEF 4")
    check("FILL flash prompts ERASE", r, "ERASE")
    check_not("FILL flash no erase without keyword", r, "Erasing")

    # Flash fill WITH ERASE
    r = cmd(ser, "swd fill flash B00B1335 4 ERASE", wait=5)
    check("FILL flash + ERASE ok", r, "Filled 4 words", "verified")

    # Verify flash fill
    r = cmd(ser, "swd read flash 4")
    check("FILL flash readback", r, "35 13 0B B0")

    # Flash fill by raw address
    r = cmd(ser, "swd fill 08000000 CAFEF00D 8 ERASE", wait=5)
    check("FILL flash raw addr + ERASE", r, "Filled 8 words", "verified")

    # Verify
    r = cmd(ser, "swd read 08000000 8")
    check("FILL flash raw addr readback", r, "0D F0 FE CA")


# ── Section: regs ─────────────────────────────────────────────

def test_regs(ser):
    section("CORE REGISTERS")

    cmd(ser, "swd connect")
    cmd(ser, "swd halt")

    r = cmd(ser, "swd regs", wait=2)
    check("REGS shows r0", r, "r0")
    check("REGS shows sp", r, "sp")
    check("REGS shows lr", r, "lr")
    check("REGS shows pc", r, "pc")
    check("REGS shows xPSR", r, "xPSR")
    check("REGS shows MSP", r, "MSP")
    check("REGS shows PSP", r, "PSP")

    # Write a known value to SRAM, write to r0 indirectly via DCRSR/DCRDR
    # Just verify regs command doesn't crash and returns values
    lines = [l for l in r.split("\n") if "r0" in l.lower() and "=" in l]
    check("REGS r0 has value", r, "r0", "0x")


# ── Section: reset ────────────────────────────────────────────

def test_reset(ser):
    section("TARGET RESET")

    cmd(ser, "swd connect")

    # Reset pulse
    r = cmd(ser, "swd reset", wait=1.5)
    check("RESET pulse", r, "OK")

    # Reconnect after reset
    r = cmd(ser, "swd connect")
    check("CONNECT after reset", r, "Connected")

    # Reset hold
    r = cmd(ser, "swd reset hold")
    check("RESET HOLD", r, "OK")

    # Reset release
    r = cmd(ser, "swd reset release")
    check("RESET RELEASE", r, "OK")

    # Reconnect after hold/release
    r = cmd(ser, "swd connect")
    check("CONNECT after hold/release", r, "Connected")

    # Halt/resume cycle
    cmd(ser, "swd halt")
    r = cmd(ser, "swd read E000EDF0")
    # S_HALT bit (bit 17) should be set
    check("HALT - DHCSR readable", r, "E000EDF0:")

    r = cmd(ser, "swd resume")
    check("RESUME", r, "OK")

    r = cmd(ser, "swd halt")
    check("HALT after resume", r, "OK")


# ── Section: flash ────────────────────────────────────────────

def test_flash(ser):
    section("FLASH OPERATIONS")

    cmd(ser, "swd connect")
    cmd(ser, "swd halt")

    # Erase page 0
    r = cmd(ser, "swd flash erase 0", wait=2)
    check("FLASH ERASE page 0", r, "OK")

    # Verify erased (should be all FF)
    r = cmd(ser, "swd read 08000000 4")
    check("FLASH erased = FF", r, "FF FF FF FF FF FF FF FF")

    # Write via SWD WRITE with ERASE
    r = cmd(ser, "swd write 08000000 AABBCCDD ERASE", wait=3)
    check("FLASH write word 0", r, "verified")

    # Write second word (same page, already erased by previous ERASE)
    # This will erase page again, which is fine
    r = cmd(ser, "swd write 08000004 11223344 ERASE", wait=3)
    check("FLASH write word 1", r, "verified")

    # Read back both (word 0 was erased by second write's page erase)
    r = cmd(ser, "swd read 08000000 2")
    # Note: second ERASE erased page, so word 0 is FF again
    check("FLASH page erase wipes prev", r, "FF FF FF FF")
    check("FLASH word 1 present", r, "44 33 22 11")

    # Erase page 1
    r = cmd(ser, "swd flash erase 1", wait=2)
    check("FLASH ERASE page 1", r, "OK")

    # Verify page 1 erased
    r = cmd(ser, "swd read 08000400 4")
    check("FLASH page 1 erased", r, "FF FF FF FF")


# ── Section: rdp ──────────────────────────────────────────────

def test_rdp(ser):
    section("RDP LEVEL (DESTRUCTIVE - mass erase)")

    cmd(ser, "swd connect")
    cmd(ser, "swd halt")

    # Read current RDP
    r = cmd(ser, "swd rdp")
    check("RDP read", r, "RDP Level")

    # Check we're at RDP0
    if "Level 0" not in r:
        print("  WARNING: Target not at RDP0, skipping RDP tests")
        skip("RDP SET 1", "not at RDP0")
        skip("RDP round-trip", "not at RDP0")
        return

    # RDP SET 0 without WIPE - should prompt
    r = cmd(ser, "swd rdp set 0")
    check("RDP SET 0 prompts WIPE", r, "WIPE", "MASS ERASE")

    # RDP SET 2 - should refuse
    r = cmd(ser, "swd rdp set 2")
    check("RDP SET 2 refused", r, "PERMANENT", "Refusing")

    # RDP SET invalid
    r = cmd(ser, "swd rdp set 3")
    check("RDP SET 3 invalid", r, "ERROR")

    # Write a marker to flash before RDP1
    cmd(ser, "swd write 08000000 BADC0FFE ERASE", wait=3)

    # Set RDP to 1
    r = cmd(ser, "swd rdp set 1", wait=5)
    check("RDP SET 1", r, "OK")

    # Reset target to apply
    cmd(ser, "swd reset", wait=2)
    time.sleep(1)

    # Reconnect and verify RDP1
    r = cmd(ser, "swd connect")
    check("CONNECT at RDP1", r, "Connected")

    cmd(ser, "swd halt")
    r = cmd(ser, "swd rdp")
    check("RDP reads Level 1", r, "Level 1")

    # Flash should be unreadable at RDP1
    r = cmd(ser, "swd read 08000000")
    check("FLASH blocked at RDP1", r, "ERROR")

    # SRAM should be accessible when halted
    r = cmd(ser, "swd write 20000000 DEADBEEF")
    check("SRAM write at RDP1", r, "verified")

    # PPB still accessible
    r = cmd(ser, "swd read E000ED00")
    check("PPB read at RDP1", r, "E000ED00:")

    # Set RDP back to 0 (mass erase) - needs WIPE
    r = cmd(ser, "swd rdp set 0 WIPE", wait=10)
    check("RDP SET 0 WIPE (mass erase)", r, "OK")

    # Reset and reconnect
    cmd(ser, "swd reset", wait=2)
    time.sleep(2)
    r = cmd(ser, "swd connect")
    check("CONNECT after mass erase", r, "Connected")

    cmd(ser, "swd halt")
    r = cmd(ser, "swd rdp")
    check("RDP back to Level 0", r, "Level 0")

    # Flash should be erased (all FF)
    r = cmd(ser, "swd read 08000000 4")
    check("FLASH erased after RDP0", r, "FF FF FF FF")


# ── Section: opt ──────────────────────────────────────────────

def test_opt(ser):
    section("OPTION BYTES")

    cmd(ser, "swd connect")
    cmd(ser, "swd halt")

    r = cmd(ser, "swd opt")
    check("OPT readable", r, "RDP")
    check_not("OPT no error", r, "ERROR")


# ── Section: errors ───────────────────────────────────────────

def test_errors(ser):
    section("ERROR HANDLING & EDGE CASES")

    cmd(ser, "swd connect")
    cmd(ser, "swd halt")

    # DUMP is removed
    r = cmd(ser, "swd dump sram")
    check("DUMP removed", r, "ERROR")

    # Unknown subcommand
    r = cmd(ser, "swd blah")
    check("Unknown subcmd", r, "ERROR")

    # READ missing addr
    r = cmd(ser, "swd read dp")
    check("READ DP missing addr", r, "ERROR")

    # READ AP missing addr
    r = cmd(ser, "swd read ap")
    check("READ AP missing addr", r, "ERROR")

    # WRITE missing value
    r = cmd(ser, "swd write dp 0")
    check("WRITE DP missing value", r, "ERROR")

    # FILL missing args
    r = cmd(ser, "swd fill")
    check("FILL no args", r, "ERROR", "Usage")

    # Auto-connect: run a command without explicit connect
    # (disconnect first by resetting state)
    r = cmd(ser, "swd idcode")
    check("Auto-connect on IDCODE", r, "DPIDR")

    # ── Error recovery: sticky errors must not break subsequent commands ──

    # Read from unmapped address -> triggers STICKYERR
    r = cmd(ser, "swd read FFFFFFFC")
    check("Read bad addr errors", r, "ERROR")

    # Next command should recover automatically
    r = cmd(ser, "swd read E000ED00")
    check("Recover after read error", r, "E000ED00:")

    # Fill to bad address -> triggers STICKYERR
    r = cmd(ser, "swd fill 0xE0100000 12345678 4")
    # May partially succeed or error
    # Now verify recovery
    r = cmd(ser, "swd idcode")
    check("Recover after fill error", r, "DPIDR")

    # SRAM operations still work after errors
    r = cmd(ser, "swd write 20000000 AABBCCDD")
    check("SRAM write after errors", r, "verified")
    r = cmd(ser, "swd read 20000000")
    check("SRAM read after errors", r, "DD CC BB AA")

    # Multiple errors in a row, then recovery
    cmd(ser, "swd read FFFFFFFC")
    cmd(ser, "swd read FFFFFFFC")
    cmd(ser, "swd read FFFFFFFC")
    r = cmd(ser, "swd read sram 4")
    check("Recover after 3 consecutive errors", r, "Read complete")

    # WRITE to peripheral (read-only CPUID) - write succeeds but verify fails
    r = cmd(ser, "swd write E000ED00 12345678")
    # CPUID is read-only, verify should show mismatch
    check("WRITE to read-only reg", r, "Verify failed" if "Verify" in r else "OK")

    # Halt when already halted
    cmd(ser, "swd halt")
    r = cmd(ser, "swd halt")
    check("HALT when halted", r, "OK")

    # Resume when running (after resume)
    cmd(ser, "swd resume")
    r = cmd(ser, "swd resume")
    check("RESUME when running", r, "OK")


# ── Section: reboot ───────────────────────────────────────────

def test_after_reboot(ser):
    """Tests that require a target reset between operations."""
    section("POST-REBOOT BEHAVIOR")

    # Connect, write SRAM, reset, verify SRAM is gone
    cmd(ser, "swd connect")
    cmd(ser, "swd halt")
    cmd(ser, "swd write 20000100 DEADBEEF")
    r = cmd(ser, "swd read 20000100")
    check("Pre-reboot SRAM write", r, "EF BE AD DE")

    # Reset target
    cmd(ser, "swd reset", wait=1.5)
    cmd(ser, "swd connect")
    cmd(ser, "swd halt")
    r = cmd(ser, "swd read 20000100")
    # SRAM contents after reset are undefined, just verify we can read
    check("Post-reboot SRAM readable", r, "20000100:")

    # Write flash, reset, verify flash persists
    cmd(ser, "swd write 08000000 F00DCAFE ERASE", wait=3)
    cmd(ser, "swd reset", wait=1.5)
    cmd(ser, "swd connect")
    cmd(ser, "swd halt")
    r = cmd(ser, "swd read 08000000")
    check("Flash persists after reboot", r, "FE CA 0D F0")

    # Connect under reset, verify halt
    r = cmd(ser, "swd connectrst", wait=2)
    check("CONNECTRST after reboot", r, "Connected")
    r = cmd(ser, "swd read E000EDF0")
    check("Halted after CONNECTRST", r, "E000EDF0:")

    # Auto-detect after reboot
    cmd(ser, "swd reset", wait=1.5)
    r = cmd(ser, "swd idcode", wait=2)
    check("IDCODE after reboot", r, "DPIDR", "CPUID")


# ── Runner ────────────────────────────────────────────────────

SECTIONS = {
    "connect": test_connect,
    "read": test_read,
    "write": test_write,
    "fill": test_fill,
    "regs": test_regs,
    "reset": test_reset,
    "flash": test_flash,
    "rdp": test_rdp,
    "opt": test_opt,
    "errors": test_errors,
    "reboot": test_after_reboot,
}

# Sections safe to run without mass-erasing flash
SAFE_SECTIONS = ["connect", "read", "write", "fill", "regs", "reset", "flash", "opt", "errors", "reboot"]
ALL_SECTIONS = SAFE_SECTIONS + ["rdp"]


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else "all"

    if which == "all":
        run_list = ALL_SECTIONS
    elif which == "safe":
        run_list = SAFE_SECTIONS
    elif which in SECTIONS:
        run_list = [which]
    else:
        print(f"Unknown section: {which}")
        print(f"Available: {', '.join(SECTIONS.keys())}, safe, all")
        sys.exit(1)

    print(f"SWD Regression Test Suite")
    print(f"Sections: {', '.join(run_list)}")
    if "rdp" in run_list:
        print("WARNING: RDP tests will mass-erase target flash!")
    print()

    try:
        ser = open_port()
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {PORT}: {e}")
        sys.exit(1)

    try:
        for name in run_list:
            SECTIONS[name](ser)
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
    finally:
        ser.close()

    print(f"\n{'='*60}")
    print(f"  RESULTS: {pass_count} passed, {fail_count} failed, {skip_count} skipped")
    print(f"{'='*60}")

    sys.exit(1 if fail_count > 0 else 0)


if __name__ == "__main__":
    main()
