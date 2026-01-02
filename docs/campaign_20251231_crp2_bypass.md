# CRP2 Bypass Campaign - December 31, 2025

## Summary

Successfully bypassed CRP2 protection on LPC2468 target via EMFI voltage glitching.

## Timeline

- **Start:** 2025-12-31 12:35:41
- **Success:** 2025-12-31 23:44:34
- **Duration:** ~11 hours

## Winning Parameters

```
python3 scripts/crp3_fast_glitch.py 210 6273 76 100
```

| Parameter | Value |
|-----------|-------|
| Cell (X,Y) | (12, 14) |
| Voltage | 210V |
| Pause | 6273 cycles @ 150MHz |
| Width | 76 cycles |
| Attempt | 36 of 100 |

## Cells Tested

9 cells were tested before success (plus cell (10,14) from earlier failed run):

| # | Cell | Max Voltage | Result |
|---|------|-------------|--------|
| 1 | (10, 15) | 150V | 100% crash at all widths |
| 2 | (10, 16) | 287V | 100% blocked throughout |
| 3 | (11, 12) | 150V | 100% crash at all widths |
| 4 | (11, 13) | 150V | 100% blocked |
| 5 | (11, 14) | 218V | Transition from blocked to crash at ~190V |
| 6 | (11, 15) | 162V | 100% blocked |
| 7 | (11, 16) | 312V | 100% blocked |
| 8 | (12, 12) | 156V | 100% blocked |
| 9 | (12, 14) | 212V | **SUCCESS at 210V** |

## Flash Dump Details

- **File:** `crp3_flash_20251231_234142.uue` (not committed)
- **Size:** 516,096 bytes (504KB)
- **CRP Value at 0x1FC:** 0x87654321 (CRP2)
- **Firmware content:** ~480KB of actual code

## Campaign Configuration

From `targeted_campaign.py`:

```python
VOLTAGE_MIN = 150
VOLTAGE_STEP = 5
QUICK_SCAN_ATTEMPTS = 100
INTENSIVE_ATTEMPTS = 1000
PAUSE_CYCLES = 6273
WIDTH_CYCLES = 26  # Starting width, calibrated per cell
WIDTH_MIN = 10
WIDTH_MAX = 100
TARGET_CRASH_RATE = 0.50
```

## Observations

1. **Cell (10,14)** - Extremely sensitive, 100% crashes even at minimum width=10 across all voltages tested (150-325V). Pico disconnected during testing at higher voltages.

2. **Cell (12,14)** - Sweet spot found at 210V with width=76:
   - 195V: 100% blocked
   - 200V: 99% blocked, 1% crash
   - 205V: 99% blocked, 1% crash
   - 210V: **SUCCESS** (1 success, 34 blocked, 1 crash in 36 attempts)

3. **Width calibration** converged to 76 cycles for most cells that showed blocked behavior (vs 10 cycles for crash-prone cells).

## Scripts Used

- `targeted_campaign.py` - Campaign orchestration, XY positioning, voltage sweeping
- `scripts/crp3_fast_glitch.py` - Core glitching and flash dump logic

## Hardware Setup

- **Glitcher:** Raiden Pico (RP2350)
- **EMFI:** ChipSHOUTER
- **Target:** LPC2468 with CRP2 protection
- **Positioning:** GRBL-controlled XY platform

## CSV Data

Full campaign data saved to `campaign_20251231_123541.csv` (169 test runs logged).

---

## Follow-up Validation Test (January 1, 2026)

After the initial success, validation tests were run to measure the reproducible success rate.

### Test 1: --test-only mode (4-byte read)

| Metric | Value |
|--------|-------|
| Attempts | ~1458 (458 + 1000) |
| SUCCESS | 0 (0.00%) |
| CRP_BLOCKED | ~94% |
| CRASH | ~6% |

**Result:** Zero successes. The 4-byte read does not trigger the glitch vulnerability.

### Test 2: --no-save mode (full 516096-byte read request)

| Metric | Value |
|--------|-------|
| Attempts | 1000 |
| SUCCESS | 31 (3.10%) |
| CRP_BLOCKED | 840 (84.00%) |
| CRASH | 129 (12.90%) |

**Result:** 31 successes (3.1% rate), confirming the bypass is reproducible.

### Key Finding

The **full 516096-byte read request is required** for the glitch to work. The glitch timing is calibrated for the longer ISP read operation. A minimal 4-byte read completes too quickly and doesn't trigger the vulnerability.

### Script Flags Added

- `--test-only`: Quick 4-byte read, no save, runs all iterations (for testing, but doesn't trigger glitch)
- `--no-save`: Full 516096-byte read request, no data collection on success, runs all iterations (validates glitch without saving dump)

---

## Parameter Optimization (January 2, 2026)

Systematic parameter sweeps were run to optimize the success rate.

### Voltage/Width Optimization

Tested 36 combinations: 6 voltages x 6 widths, 200 iterations each.

**Top Results (200 iterations):**

| Voltage | Width | Success Rate | Crashes |
|---------|-------|--------------|---------|
| 210V | 90 | 4.5% | 12% |
| 215V | 60 | 4.5% | 11% |
| 210V | 80 | 4.0% | 15.5% |
| 210V | 76 | 3.5% | 16.5% |

**Findings:**
- 210V is the optimal voltage
- 220V+ causes too many crashes (0% success)
- 200-205V is too weak (0.5-1.5% success)

### Timing Optimization

Fine-tuned pause value ±1 to ±10 cycles around base 6273.

**Top Results (200 iterations):**

| Pause | Offset | Success Rate |
|-------|--------|--------------|
| 6275 | +2 | 5.5% |
| 6263 | -10 | 3.0% |
| 6271 | -2 | 3.0% |
| 6273 | 0 | 0.0% |

**Finding:** +2 cycles (13.3ns at 150MHz) improved success rate.

### 1000-Iteration Validation

| Parameter Set | Success Rate |
|---------------|--------------|
| Original (V=210, P=6273, W=76) | 3.1% (31/1000) |
| Width-optimized (V=210, P=6273, W=90) | 2.5% (25/1000) |
| Timing-optimized (V=210, P=6275, W=90) | 2.7% (27/1000) |

### Conclusions

1. **Small sample variance**: 200-iteration tests showed 4.5-5.5% success, but 1000-iteration validation converged to ~2.5-3%
2. **Optimal parameters**: V=210, pause=6273-6275, width=76-90
3. **True success rate**: ~2.5-3.1% with current setup
4. **Original parameters remain competitive**: The campaign-discovered parameters (V=210, P=6273, W=76) at 3.1% are within the optimal range
