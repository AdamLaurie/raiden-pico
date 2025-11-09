# SUCCESS FOUND - LPC1114 Glitch Bypass

**Date:** 2025-10-28 10:07

## Success Parameters

**Working glitch parameters that bypass LPC1114 bootloader security:**

- **Voltage:** 290V
- **Pause:** 0 cycles (immediate glitch after trigger)
- **Width:** 150 cycles (1.00 μs)
- **Trigger:** UART byte 0x0D (carriage return '\r')

## Test Results at V=290, P=0, W=150

Testing 10 iterations:
- **SUCCESS:** 1/10 (10%)
- **NO_RESPONSE (crash):** 5/10 (50%)
- **ERROR19 (normal):** 4/10 (40%)

The bootloader response "0" was received instead of the expected "19" error code, indicating successful security bypass via fault injection.

## Discovery Process

### Phase 1: Find Crash Window
- Tested V=500, P=0-7500 in 500-cycle steps
- Found crash at P=0 (100% NO_RESPONSE)

### Phase 2a: Find Voltage Boundaries
- Tested V=500 down to V=275 in 25V steps at P=0
- V=500-300: 100% crashes
- V=275: 0% crashes (100% ERROR19)

### Phase 2b: Fine-Tune Voltage
- Tested V=280, 285, 290 in 5V steps
- V=280: 0% crashes, 0% success
- V=285: 0% crashes, 0% success
- **V=290: 50% crashes, 10% SUCCESS** ← OPTIMAL

## Conclusion

The adaptive voltage tuning strategy successfully found working glitch parameters:
- Started at high voltage (500V) with known crash window
- Reduced voltage until crashes stopped (275V)
- Fine-tuned upward in 5V steps to find the boundary where glitches affect code execution without complete crash
- V=290 is the "sweet spot" where the glitch disrupts security checks but allows bootloader to continue execution with bypassed security

## Next Steps

1. Test repeatability at V=290, P=0, W=150
2. Explore nearby parameter space (V=288-292, P=0-100, W=140-160)
3. Test different pause values at V=290 to find other success windows
4. Document reliable parameters for consistent bypass

## Log Files

- Full exploration log: `adaptive_explorer_20251028_100710.log`
