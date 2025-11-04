# LPC2468 Boot ROM Analysis

This directory contains complete analysis of the LPC2468 boot ROM, including timing analysis for voltage glitching research.

## Contents

- **lpc2468_boot_timing_analysis.md** - Complete boot ROM timing analysis with cycle-accurate measurements
- **bootrom/** - Boot ROM dumps and disassembly
- **scripts/** - Analysis and extraction tools

## Quick Reference

### Boot Timing @ 12 MHz

| Phase | Description | Cycles | Time | Cumulative |
|-------|-------------|--------|------|------------|
| 1 | Reset Entry (ARM) | 15 | 1.25 µs | 1.25 µs |
| 2 | CRP Check (ARM) | 33 | 2.75 µs | 4.00 µs |
| 3 | Boot Init (Thumb) | 150 | 12.50 µs | 16.50 µs |
| 4 | Checksum (Thumb) | 58 | 4.83 µs | 21.33 µs |
| **Total** | | **256** | **21.33 µs** | |

### Critical Glitch Windows

1. **CRP Read (Primary Target)**
   - Time: 4.000 µs from reset
   - Window: 250 ns (3 cycles)
   - Address: 0x7FFFE050

2. **Checksum Loop**
   - Time: 16.5-21.3 µs from reset
   - Window: 4.833 µs (58 cycles)

3. **Branch Decision**
   - Time: 21.333 µs from reset
   - Window: 83 ns (1 cycle)

## Usage

### Dump Boot ROM

```bash
cd scripts
./dump_bootrom.sh
```

This will:
- Connect via JTAG using OpenOCD
- Dump 72KB boot ROM from 0x7FFEE000
- Generate ARM and Thumb disassembly

### Analyze Timing

```bash
cd scripts
python3 analyze_boot_timing.py --clock 12.0
```

Calculate timing for different clock frequencies:

```bash
python3 analyze_boot_timing.py --compare
```

### Extract Boot Sequence

```bash
cd scripts
python3 extract_boot_sequence.py ../bootrom/bootrom_arm_disasm.txt
```

## Boot ROM Structure

| Address Range | Size | Contents |
|--------------|------|----------|
| 0x7FFEE000-0x7FFFDFFF | 64KB | Boot code and data |
| 0x7FFFE000-0x7FFFE0FF | 256B | Reset entry and CRP check (ARM mode) |
| 0x7FFFE100-0x7FFFFFFF | ~7.75KB | Main boot code (Thumb mode) |

## Key Addresses

| Address | Mode | Function |
|---------|------|----------|
| 0x7FFFE000 | ARM | Reset entry point |
| 0x7FFFE040 | ARM | CRP check routine |
| 0x7FFFE050 | ARM | **CRP read instruction** (glitch target) |
| 0x7FFFE326 | Thumb | Main boot initialization |
| 0x7FFFE20E | Thumb | Normal boot path (good checksum) |
| 0x7FFFE2C4 | Thumb | ISP mode path (bad checksum) |

## CRP Values

| Name | Value | Effect |
|------|-------|--------|
| CRP0 | 0xDEADBEEF | No protection |
| CRP1 | 0x12345678 | JTAG disabled, limited ISP |
| CRP2 | 0x87654321 | JTAG disabled, chip erase only |
| CRP3 | 0x43218765 | Maximum protection |
| NO_ISP | 0x4E697370 | ISP disabled, JTAG enabled |

## Files

### Boot ROM Dumps

- `bootrom/lpc2468_bootrom_full.bin` - Complete 72KB boot ROM binary
- `bootrom/bootrom_arm_disasm.txt` - ARM mode disassembly
- `bootrom/bootrom_thumb_disasm.txt` - Thumb mode disassembly

### Scripts

- `scripts/dump_bootrom.sh` - Dump boot ROM via JTAG
- `scripts/analyze_boot_timing.py` - Calculate precise boot timing
- `scripts/extract_boot_sequence.py` - Extract boot sequence from disassembly

## Related Files

The following files in the parent directories are related to LPC2468 CRP testing:

- `../openocd/lpc2478_no_checksum.cfg` - OpenOCD config without auto-checksum
- `../images/crp*_test.{bin,hex}` - CRP test images for all protection levels
- `../scripts/create_crp_image.py` - Tool to generate bootable CRP images

## Methodology

This analysis was performed by:

1. **Dumping** the boot ROM via JTAG using OpenOCD
2. **Disassembling** with GNU ARM toolchain (arm-none-eabi-objdump)
3. **Tracing** execution flow from reset vector
4. **Counting** instruction cycles per ARM7TDMI datasheet
5. **Verifying** with JTAG halt and register inspection

## References

- ARM7TDMI Technical Reference Manual
- LPC24XX User Manual (UM10237)
- NXP LPC2468 Datasheet
- ARM Architecture Reference Manual

## License

This analysis is provided for educational and security research purposes.
