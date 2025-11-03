# LPC2468 CRP Trace Findings

## Summary
Used OpenOCD + J-Link Pro to trace LPC2468 bootloader startup to identify CRP check timing.

## Hardware Setup
- Target: LPC2468 microcontroller (ARM7TDMI-S)
- Debugger: J-Link Pro via JTAG
- JTAG Speed: 500 kHz (default from lpc2478.cfg)
- OpenOCD Version: 0.12.0

## Key OpenOCD Configuration
```bash
openocd -f interface/jlink.cfg -f target/lpc2478.cfg \
    -c "init" \
    -c "arm7_9 fast_memory_access enable" \  # Critical for preventing timeouts
    -c "reset halt"
```

## Boot Sequence Discovered

### Instructions 0-4: Flash Startup Code (0x00000000-0x00000010)
```
[0] 0x00000000: ldr r4, [pc, #0x18]     # Load 0x7fffe040 into r4
[1] 0x00000004: ldr r5, [pc, #0x10]     # Load 0xffffbfff into r5
[2] 0x00000008: ldr r6, [r4]            # Load value from [r4] into r6
[3] 0x0000000c: and r6, r5, r6          # AND r6 with r5
[4] 0x00000010: str r6, [r4]            # Store result back to [r4]
```

This appears to be setting up a hardware register (possibly pinsel/peripheral configuration).

### Instruction 5: Jump to Bootloader ROM (0x00000014)
```
[5] 0x00000014: ldr pc, [pc, #-4]       # Loads 0x7fffe040 into PC
```

**This jumps to the bootloader ROM at 0x7fffe040!**

### CRP Address and Value
- **CRP Location**: 0x000001FC (standard for LPC2xxx series)
- **CRP Value Read**: 0xe20330ff
  - This is NOT a standard CRP value (NO_ISP, CRP1, CRP2, CRP3)
  - Appears to be user application code or custom protection

## OpenOCD Limitation Discovered
- Single-stepping works fine through flash (0x0-0x00000014)
- **Timeout occurs when trying to step into bootloader ROM** (0x7FFF0000 region)
- Error: "timeout waiting for SYSCOMP & DBGACK"
- This prevents direct tracing of the CRP check code in ROM

## Critical Timing for Glitch Attack

### Target Window: Instructions 4-6
The CRP check happens **immediately after** jumping to bootloader ROM at instruction 5.

**Recommended glitch timing:**
- **Primary target**: Instruction 5 (0x00000014) - the jump to bootloader
- **Secondary targets**: Instructions 4 or 6 (around the transition)

### Glitch Strategy
1. Reset the target
2. Count 5 instruction cycles from reset
3. Apply glitch during/around instruction 5-6 window
4. Goal: Skip or corrupt the CRP check that happens in ROM

## Tool Created
`scripts/openocd_crp_trace_working.py` - Python script that:
- Connects via OpenOCD to J-Link
- Reads CRP value from flash
- Single-steps through startup code
- Disassembles and analyzes instructions
- Identifies CRP-related operations

## Next Steps for Glitch Attack
1. Configure glitch hardware to trigger at instruction count 4-6
2. Use instruction-synchronized glitching (not time-based)
3. Monitor UART for bootloader "Synchronized" response to detect successful bypass
4. Sweep glitch parameters (voltage, width, position) around this window

## Files
- `scripts/openocd_crp_trace_working.py` - Main trace tool
- `scripts/openocd_crp_trace_simple.py` - Simplified version
- `scripts/openocd_crp_trace.py` - Original telnet-based version (deprecated)
- `test_bootloader_entry.py` - Test script for bootloader UART sync
