# GPIO Trigger Bug - RESOLVED

## Summary
The GPIO trigger was always firing on FALLING edge regardless of RISING/FALLING configuration.

## Status: ✅ RESOLVED

The bug has been fixed. Both RISING and FALLING edge detection now work correctly.

## Root Cause
**Unstable trigger signal causing false edge detection.**

The original edge detection programs lacked proper debouncing and signal stability verification. When the trigger signal had noise, glitches, or slow rise/fall times, the PIO state machine would detect spurious edges or misidentify the edge type. This was particularly problematic when:

- Trigger signals came from long wires or noisy sources
- Target reset pins had slow transitions
- Pull-up/pull-down resistors created non-ideal edge rates

The symptom appeared as "always triggering on FALLING edge" because the unstable signal would cause the RISING edge detector to see multiple transitions during a single edge event.

## Solution
The fix involved updating the edge detection programs in `src/glitch.pio` to include proper debouncing and one-shot trigger behavior:

### RISING Edge Detection (`gpio_edge_detect_rising`)
```
1. Wait for pin to be LOW
2. Debounce delay (~639ns total via three 32-cycle NOPs)
3. Verify pin is still LOW (stable state confirmation)
4. Wait for RISING edge (LOW->HIGH)
5. Trigger glitch via IRQ
6. Halt (one-shot behavior)
```

### FALLING Edge Detection (`gpio_edge_detect_falling`)
```
1. Wait for pin to be HIGH
2. Debounce delay (~639ns total via three 32-cycle NOPs)
3. Verify pin is still HIGH (stable state confirmation)
4. Wait for FALLING edge (HIGH->LOW)
5. Trigger glitch via IRQ
6. Halt (one-shot behavior)
```

Both programs include:
- **3-stage debouncing** (~639ns total @ 150MHz)
- **Stable state verification** before edge detection
- **One-shot triggering** (halts after firing to prevent multiple triggers)

## Verification
- ✅ UART trigger works correctly
- ✅ Pulse generation works correctly
- ✅ Timing is accurate
- ✅ Glitches fire as expected
- ✅ GPIO RISING edge detection works correctly
- ✅ GPIO FALLING edge detection works correctly

## Resolution Date
2025-11-06

## Files Modified
- `src/glitch.pio` - Updated edge detection programs with debouncing and stability checks
- `src/glitch.c` - Implemented proper program selection based on edge type (lines 146-163)

## Lessons Learned
- Always implement debouncing for real-world signal detection
- Verify signal stability before acting on edge detection
- PIO state machines need explicit handling of noisy/slow signals
- One-shot behavior prevents spurious re-triggering on signal bounce
