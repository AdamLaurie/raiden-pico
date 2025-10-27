# Raiden Pico Version History

## v0.3 - Fixed UART Byte Matching
**Date:** 2025-10-21
**File:** raiden_pico_v0.3.uf2

### Changes from v0.2:
- ✅ **UART byte matching FIXED**: Now correctly triggers ONLY on specified byte
- ✅ **ISR-based comparison**: Rewrote PIO UART decoder to use ISR directly instead of autopush
- ✅ **Removed autopush**: Simplified PIO program for more reliable byte comparison
- ✅ **Verified with oscilloscope**: Confirmed glitch triggers only on matching bytes

### Technical Details:
- Changed from autopush+pull pattern to direct ISR comparison (`mov x, isr` then `jmp x!=y`)
- Added `mov isr, null` before each byte to clear residual data from previous bytes
- This eliminates timing issues with `pull noblock` and ensures reliable byte matching
- Trigger byte position correctly aligned in bits [24:31] after right shift

### Bug Fixes:
- Fixed UART byte matching - previously triggered on EVERY byte instead of just the target byte
- Fixed glitch timing - now triggers after complete byte reception, not during first bit

### Testing Results:
- ✅ Triggers correctly on 0x0D (carriage return)
- ✅ Triggers correctly on 0x52 ('R')
- ✅ Does NOT trigger on non-matching bytes
- ✅ Verified with SYNC and TARGET SEND commands
- ✅ Sub-microsecond latency maintained

---

## v0.2 - Cycle-Based Timing & ARM Fix
**Date:** 2025-10-21
**File:** raiden_pico_v0.2.uf2

### Changes from v0.1:
- ✅ **Cycle-based timing**: Changed from microseconds to system clock cycles (150MHz = 6.67ns per cycle)
- ✅ **Improved defaults**: Width and gap now default to 100 cycles (0.67us) instead of 15000 cycles
- ✅ **TARGET SYNC retries**: Automatically retries up to 5 times on sync failure
- ✅ **Fixed ARM after GLITCH**: Properly disarms PIO state machines after glitch execution
- ✅ **Enhanced STATUS/GET**: Now shows both cycles and microsecond equivalents
- ✅ **Updated HELP**: Documentation reflects new cycle-based timing

### Features:
- Ultra-low latency PIO-based UART triggering (sub-microsecond)
- Nanosecond-precision timing (6.67ns resolution)
- Triggers on ANY byte (for testing/debugging)
- Auto-disarm after trigger with proper cleanup
- Glitch count tracking
- RP2350B workaround (requires GP5→GP28 jumper wire)
- Configurable glitch parameters in clock cycles
- Manual, GPIO, and UART trigger modes
- Reliable TARGET SYNC with automatic retries

### Commands:
- `VERSION` - Show version info
- `STATUS` - Show system status with cycle and microsecond timing
- `ARM ON/OFF` - Arm/disarm system (now works reliably after GLITCH)
- `SET PAUSE/WIDTH/GAP <cycles>` - Configure glitch timing in cycles
- `SET COUNT <value>` - Set number of pulses
- `GET PAUSE/WIDTH/GAP/COUNT` - Get current values
- `TRIGGER NONE/GPIO/UART <byte>` - Set trigger type
- `GLITCH` - Manual trigger
- `TARGET SYNC` - Synchronize with target (up to 5 retries)
- `RESET` - Reset system
- `REBOOT BL` - Enter bootloader

### Architecture:
- PIO0 SM1: Pulse generator (waits for IRQ 0)
- PIO0 SM2: UART decoder (monitors GP28, triggers IRQ 0)
- Pure hardware triggering with no CPU involvement
- Proper state machine cleanup on disarm

### Bug Fixes:
- Fixed "Failed to arm system" error after GLITCH execution
- Proper PIO state machine cleanup in glitch_execute()

---

## v0.1 - Initial Working Version
**Date:** 2025-10-21
**File:** raiden_pico_v0.1.uf2

### Features:
- ✅ Ultra-low latency PIO-based UART triggering (sub-microsecond)
- ✅ Triggers on ANY byte (for testing/debugging)
- ✅ Auto-disarm after trigger
- ✅ Glitch count tracking
- ✅ RP2350B workaround (requires GP5→GP28 jumper wire)
- ✅ Configurable glitch parameters (pause, width, gap, count)
- ✅ Manual trigger mode
- ✅ GPIO trigger mode (rising/falling edge)
- ✅ TARGET SYNC with 200ms timing

### Known Issues:
- Triggers on ANY byte instead of specific byte (intentional for v0.1)
- Requires jumper wire GP5→GP28 for RP2350B chips

### Commands:
- `VERSION` - Show version info
- `STATUS` - Show system status
- `ARM ON/OFF` - Arm/disarm system
- `TRIGGER NONE/GPIO/UART <byte>` - Set trigger type
- `SET PAUSE/WIDTH/GAP/COUNT <value>` - Configure glitch
- `GLITCH` - Manual trigger
- `TARGET SYNC` - Synchronize with target
- `RESET` - Reset system
- `REBOOT BL` - Enter bootloader

### Architecture:
- PIO0 SM1: Pulse generator (waits for IRQ 0)
- PIO0 SM2: UART decoder (monitors GP28, triggers IRQ 0)
- Pure hardware triggering with no CPU involvement
- FIFO-based glitch detection for count tracking

### Testing:
- Confirmed working on oscilloscope
- Sub-microsecond latency from UART byte to glitch pulse
- Tested with LPC target synchronization
## Version 0.4 (2025-10-21)

**Changes from v0.3:**
- Added CLI command history with up/down arrow navigation
- ANSI escape sequence handling for terminal control
- History buffer stores last 10 commands
- Automatic line redrawing when browsing history

**Technical Details:**
- Same UART trigger implementation as v0.3
- Based on v0.3 codebase with CLI enhancements only
- No changes to PIO programs or glitch engine
- Internal looping pulse generator (from v0.3)
- Direct ISR comparison for byte matching (from v0.3)

**Files:**
- Binary: raiden_pico_v0.4.uf2
- Source: v0.4_source.tar.gz

**Known Issues:**
- Same as v0.3: Glitch timing during byte reception instead of after
- UART trigger works but timing needs refinement

