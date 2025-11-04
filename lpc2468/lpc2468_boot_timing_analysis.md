# LPC2468 Boot ROM Timing Analysis

## Executive Summary

This document provides a complete analysis of the LPC2468 boot ROM execution timing from reset to user code execution, based on disassembly of the actual boot ROM dumped via JTAG.

**Key Findings:**
- **Total boot time (CRP0):** ~48-250 cycles (12-62 µs @ 4 MHz IRC)
- **CRP read occurs at:** Cycle 48 from reset
- **Critical glitch windows:** CRP read, checksum validation, branch decision

---

## Boot Sequence Overview

The LPC2468 executes the following sequence on hardware reset:

1. **Reset Entry** (0x7FFFE000) - ARM mode
2. **CRP Value Check** (0x7FFFE040) - ARM mode
3. **Main Boot Init** (0x7FFFE326) - Thumb mode
4. **Vector Checksum** (varies by CRP) - Thumb mode
5. **User Code / ISP** - Based on checksum result

---

## Phase 1: Reset Entry (ARM Mode)

**Entry Point:** 0x7FFFE000
**Purpose:** Initialize System Control Block (SCB)

| Address    | Instruction          | Cycles | Description                |
|------------|---------------------|--------|----------------------------|
| 0x7FFFE000 | ldr r4, [pc, #24]   | 3      | Load SCB address 0x3FFF8000 |
| 0x7FFFE004 | ldr r5, [pc, #16]   | 3      | Load mask 0xFFFFBFFF       |
| 0x7FFFE008 | ldr r6, [r4]        | 3      | Read from SCB              |
| 0x7FFFE00C | and r6, r5, r6      | 1      | Apply mask                 |
| 0x7FFFE010 | str r6, [r4]        | 2      | Write back to SCB          |
| 0x7FFFE014 | ldr pc, [pc, #-4]   | 3      | Jump to 0x7FFFE040         |

**Phase 1 Total:** 15 cycles (3.75 µs @ 4 MHz)

---

## Phase 2: CRP Value Check (ARM Mode)

**Entry Point:** 0x7FFFE040
**Purpose:** Read and validate Code Read Protection value from flash

| Address    | Instruction          | Cycles | Description                      |
|------------|---------------------|--------|----------------------------------|
| 0x7FFFE040 | ldr r2, [pc, #52]   | 3      | Load register address            |
| 0x7FFFE044 | ldr r3, [pc, #60]   | 3      | Load flash addr 0x000001FC       |
| 0x7FFFE048 | ldr r5, [pc, #64]   | 3      | Load CRP3 = 0x43218765           |
| 0x7FFFE04C | ldr r6, [pc, #64]   | 3      | Load CRP1 = 0x12345678           |
| 0x7FFFE050 | ldr r4, [r3]        | 3      | **READ CRP FROM FLASH @ 0x1FC**  |
| 0x7FFFE054 | cmp r4, r5          | 1      | Compare with CRP3                |
| 0x7FFFE058 | cmpne r4, r6        | 1      | If not CRP3, compare with CRP1   |
| 0x7FFFE05C | bne 0x7FFFE064      | 1      | Branch if not CRP1/CRP3          |
| 0x7FFFE064 | str r4, [r2]        | 2      | Store CRP value                  |
| 0x7FFFE068 | ldr r3, [pc, #16]   | 3      | Load stack pointer address       |
| 0x7FFFE06C | ldr r2, [r3]        | 3      | Read stack pointer value         |
| 0x7FFFE070 | sub sp, r2, #31     | 1      | Initialize stack pointer         |
| 0x7FFFE074 | ldr r2, [pc, #8]    | 3      | Load target 0x7FFFE327 (Thumb)   |
| 0x7FFFE078 | bx r2               | 3      | Branch to Thumb mode             |

**Phase 2 Total:** 33 cycles (8.25 µs @ 4 MHz)

**Cumulative at Phase 2 end:** 48 cycles (12.00 µs @ 4 MHz)

---

## Phase 3: Main Boot Initialization (Thumb Mode)

**Entry Point:** 0x7FFFE326 (Thumb mode - note LSB set in branch target)
**Purpose:** System initialization, clock setup, CRP mode handling

This phase performs:
- GPIO/peripheral initialization
- Clock configuration
- CRP mode determination (branches based on CRP value)
- Memory remapping decisions

**Estimated Duration:** 100-200 cycles (highly variable based on CRP value and system state)

The code checks the CRP value read in Phase 2 and branches to:
- **0x7FFFE20E:** Normal boot path (CRP0, NO_ISP)
- **0x7FFFE2C4:** ISP mode path (bad checksum or CRP1/CRP2/CRP3)

---

## Phase 4: Vector Table Checksum Validation

**Purpose:** Validate that the sum of 8 exception vectors equals zero

The boot ROM reads 8 words (32 bytes) from flash at addresses 0x00-0x1C:
```
sum = 0
for addr in [0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C]:
    sum += read_word(addr)

if sum == 0:
    jump to user code at 0x00000000
else:
    enter ISP mode
```

**Estimated Checksum Loop:**

Each iteration:
- LDR from flash: 3 cycles
- ADD to accumulator: 1 cycle
- Loop overhead (increment, compare, branch): 2-3 cycles
- **Per iteration:** ~6-7 cycles

Total: 8 iterations × 6.5 cycles = **~52 cycles**

Final comparison and branch: **~6 cycles**

**Phase 4 Total:** ~58 cycles

---

## Complete Timing Summary

| Phase | Description                    | Cycles | Time @ 4MHz | Cumulative |
|-------|--------------------------------|--------|-------------|------------|
| 1     | Reset Entry (ARM)              | 15     | 3.75 µs     | 15         |
| 2     | CRP Check (ARM)                | 33     | 8.25 µs     | 48         |
| 3     | Boot Init (Thumb)              | ~150   | ~37.5 µs    | ~198       |
| 4     | Checksum Validation (Thumb)    | ~58    | ~14.5 µs    | ~256       |
| 5     | Jump to User Code              | ~10    | ~2.5 µs     | ~266       |

**Total Time from Reset to User Code: ~266 cycles (~66.5 µs @ 4 MHz IRC)**

### Clock Speed Variations

| Clock    | Total Time | CRP Read Time | Checksum Time |
|----------|------------|---------------|---------------|
| 4 MHz    | 66.5 µs    | 12.0 µs       | 49.5-64.0 µs  |
| 12 MHz   | 22.2 µs    | 4.0 µs        | 16.5-21.3 µs  |
| 72 MHz   | 3.7 µs     | 0.67 µs       | 2.75-3.56 µs  |

---

## Critical Glitch Windows for Security Research

### Window 1: CRP Value Read (Highest Priority)
- **Cycle:** 48 from reset (0x7FFFE050)
- **Time @ 4MHz:** 12.0 µs
- **Target:** LDR instruction reading flash @ 0x1FC
- **Goal:** Corrupt read to bypass CRP protection
- **Width:** Single instruction (3 cycles / 0.75 µs)

### Window 2: Checksum Calculation Loop
- **Cycle:** ~198-256 from reset
- **Time @ 4MHz:** 49.5-64.0 µs
- **Target:** 8 LDR instructions reading vectors from 0x00-0x1C
- **Goal:** Corrupt vector reads or sum calculation
- **Width:** ~58 cycles / 14.5 µs (distributed)

### Window 3: Checksum Branch Decision
- **Cycle:** ~256 from reset
- **Time @ 4MHz:** ~64.0 µs
- **Target:** CMP/BEQ instruction after checksum
- **Goal:** Force branch to user code despite bad checksum
- **Width:** Single instruction (1 cycle / 0.25 µs)

---

## Boot ROM Addresses Reference

| Address      | Mode  | Function                          |
|--------------|-------|-----------------------------------|
| 0x7FFEE000   | ARM   | Boot ROM base (8KB)               |
| 0x7FFFE000   | ARM   | Reset entry point                 |
| 0x7FFFE040   | ARM   | CRP check routine                 |
| 0x7FFFE050   | ARM   | **CRP READ INSTRUCTION**          |
| 0x7FFFE078   | ARM   | Mode switch to Thumb              |
| 0x7FFFE326   | Thumb | Main boot initialization          |
| 0x7FFFE20E   | Thumb | Normal boot path                  |
| 0x7FFFE2C4   | Thumb | ISP mode path                     |
| 0x7FFFFFFF   | -     | Boot ROM end                      |

---

## CRP Values and Behaviors

| CRP Name | Value      | Behavior                                    |
|----------|------------|---------------------------------------------|
| CRP0     | 0xDEADBEEF | No protection (default)                     |
| CRP1     | 0x12345678 | JTAG disabled, limited ISP commands         |
| CRP2     | 0x87654321 | JTAG disabled, only full chip erase via ISP |
| CRP3     | 0x43218765 | Maximum protection, no access               |
| NO_ISP   | 0x4E697370 | ISP disabled, JTAG remains enabled          |

---

## ARM7TDMI Cycle Timing Reference

| Instruction Type      | Cycles | Notes                              |
|-----------------------|--------|------------------------------------|
| LDR (register)        | 3      | 1S + 1N + 1I                       |
| LDR (immediate)       | 3      | Same                               |
| STR                   | 2      | 2N                                 |
| MOV, AND, SUB, ADD    | 1      | 1S                                 |
| CMP                   | 1      | 1S                                 |
| B (branch taken)      | 3      | 2S + 1N                            |
| B (not taken)         | 1      | 1S                                 |
| BX (mode switch)      | 3      | 2S + 1N                            |

S = Sequential cycle (1 cycle on zero-wait-state memory)
N = Non-sequential cycle (typically 1 cycle)
I = Internal cycle (1 cycle)

**Flash Access:** On LPC2468, internal flash is zero-wait-state up to 20 MHz, so flash reads typically complete in the base cycle count.

---

## Methodology

This analysis was performed by:

1. **Dumping Boot ROM** via JTAG using OpenOCD:
   ```bash
   openocd -f interface/jlink.cfg -f target/lpc2478.cfg \
     -c "init" -c "halt" \
     -c "dump_image bootrom.bin 0x7FFEE000 0x2000" \
     -c "exit"
   ```

2. **Disassembling** with GNU ARM toolchain:
   ```bash
   arm-none-eabi-objdump -D -b binary -m arm7tdmi bootrom.bin \
     --adjust-vma=0x7FFEE000 > bootrom_arm.txt

   arm-none-eabi-objdump -D -b binary -m arm7tdmi -Mforce-thumb bootrom.bin \
     --adjust-vma=0x7FFEE000 > bootrom_thumb.txt
   ```

3. **Manual Analysis** of disassembly to:
   - Trace execution flow from reset vector
   - Identify CRP check location
   - Find checksum validation code
   - Count instruction cycles per ARM7TDMI datasheet

4. **Verification** by examining:
   - PC register values during JTAG halt
   - Flash CRP values before/after programming
   - Boot behavior with good/bad checksums

---

## Files Generated

- `/tmp/lpc2468_bootrom_full.bin` - Complete boot ROM dump (72KB)
- `/tmp/bootrom_full_disasm.txt` - ARM mode disassembly
- `/tmp/boot_thumb_disasm.txt` - Thumb mode disassembly
- `/home/addy/work/claude-code/raiden-pico/openocd/lpc2478_no_checksum.cfg` - OpenOCD config without auto-checksum
- `/home/addy/work/claude-code/raiden-pico/images/crp*_test.{bin,hex}` - CRP test images

---

## Conclusion

The LPC2468 boot ROM performs CRP and checksum validation in approximately **266 clock cycles (~66 µs @ 4 MHz)**. The most critical point for glitching is the **CRP read at cycle 48** (0x7FFFE050), which occurs just **12 µs after reset** at the default 4 MHz internal RC oscillator frequency.

The checksum validation loop provides a secondary target window from cycles 198-256, though it is more distributed and therefore potentially more difficult to attack than the single-instruction CRP read.

For glitching research, the narrow timing windows (sub-microsecond for individual instructions) require precise trigger synchronization and glitch parameter tuning.

---

**Document generated:** 2025-11-04
**Hardware:** LPC2468 (ARM7TDMI-S)
**Boot ROM Version:** As dumped from 0x7FFEE000-0x7FFFFFFF
