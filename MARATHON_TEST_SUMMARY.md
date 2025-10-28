# Marathon Test Summary

## Current Status: 8-Hour Test Running

**Started**: 2025-10-28 08:18:12
**Expected completion**: 2025-10-28 16:18:12 (approximately)
**Process ID**: 642668

## Quick Check Commands

```bash
# Monitor live progress
tail -f glitch_marathon_20251028_081812.log

# Check current results
python3 scripts/analyze_glitch_results.py glitch_results_20251028_081812.csv

# View latest results
tail -20 glitch_results_20251028_081812.csv

# Check process is still running
ps aux | grep glitch_marathon | grep -v grep
```

## Key Discovery: Crash Window Found

**Previous attempts (5 minutes, 312 tests)**: All ERROR19
**Reason**: Focused on wrong parameters (high voltage 400-500V, late timing 6500-8000 cycles)

**New approach (marathon testing)**: Found crash window immediately!

### Crash Parameters
- **Voltage**: 300V (lower than initially tested)
- **Pause**: 0-1000 cycles (early timing)
- **Width**: 100-250 cycles
- **Result**: Consistent target crashes (NO_RESPONSE)

### What This Means

✅ **Hardware Setup Confirmed Working**
- ChipSHOUTER is correctly positioned
- EMFI is successfully affecting the target
- Glitch trigger timing is correct
- Vulnerable windows exist in the bootloader

✅ **Next Goal**
Find the narrow success window where we get security bypass (response "0") instead of just crashes. This requires testing many thousands of parameter combinations, which is why the marathon test runs for hours.

## Marathon Test Parameters

### Parameter Ranges
- **Voltage**: 300-500V (5 values: 300, 350, 400, 450, 500)
- **Pause**: 0-8000 cycles
  - Broad coverage: 0-8000 step 500 (17 values)
  - Fine coverage: 6500-8000 step 50 (31 values)
  - **Total**: 44 unique pause values
- **Width**: 100-250 cycles step 25 (7 values)

### Coverage
- **Total combinations per cycle**: 1540 (5 × 44 × 7)
- **Test rate**: ~0.43 tests/second (2.3s per test)
- **Tests per hour**: ~1500
- **8-hour total**: ~12,000 tests
- **Cycles through full space**: ~7-8 times

### Why Hours of Testing?

The success window (security bypass) is likely:
1. **Very narrow**: May only be 10-50 cycles wide
2. **Timing-sensitive**: Requires precise synchronization
3. **Probabilistic**: May only succeed X% of the time even at perfect parameters
4. **Near crash parameters**: Often just slightly different timing/voltage from crash

By testing thousands of combinations repeatedly, we increase the probability of hitting the narrow success window.

## Expected Results

### Short-term (1-2 hours)
- Identify full crash parameter space
- Map voltage sensitivity (does crash window extend to higher voltages?)
- Identify pause ranges that crash vs don't crash

### Medium-term (4-6 hours)
- May find first success (security bypass)
- Identify patterns in success/crash boundaries
- Narrow parameter ranges for focused testing

### Long-term (8+ hours)
- Statistical confidence in success parameters
- Multiple successful attempts to confirm reproducibility
- Optimal parameter identification

## Result Files

All results logged to CSV with timestamps for later analysis:
- **glitch_results_20251028_081812.csv** - Current test results
- Each row: timestamp, voltage, pause, width, result, elapsed_time

### Result Types
- **SUCCESS**: Target returns "0" (security bypass - our goal!)
- **ERROR19**: Target returns "19" (normal operation, glitch had no effect)
- **NO_RESPONSE**: Target crashes/hangs (we're affecting it, but too much)
- **UNKNOWN**: Unexpected response
- **SYNC_FAIL**: Communication error (rare)

## Next Steps After Marathon Completes

1. **Analyze full results**
   ```bash
   python3 scripts/analyze_glitch_results.py glitch_results_20251028_081812.csv
   ```

2. **If SUCCESS found**:
   - Note exact parameters (V, Pause, Width)
   - Re-test those parameters with higher iteration count
   - Update default script parameters
   - Document success rate

3. **If only crashes found**:
   - Focus on boundary between crash and ERROR19
   - Test with finer granularity around crash window edges
   - Try slight variations in voltage/timing

4. **If neither (all ERROR19)**:
   - This would be surprising given we already found crashes
   - May indicate test setup issue
   - Check log file for errors

## Monitoring Tips

The script prints progress every 30 seconds:
```
[0.5h/8.0h] Tests: 758 (0.42/s) | Success: 0 | NO_RESP: 350 | ERROR19: 408 | Remaining: 7.5h
```

This tells you:
- How far through the test (0.5h of 8.0h)
- Total tests run (758)
- Test rate (0.42/second)
- Result breakdown (0 success, 350 crashes, 408 normal)
- Time remaining (7.5h)

If you see:
- **High NO_RESP**: Good! We're in the vulnerable window
- **High ERROR19**: May need different parameters
- **Any SUCCESS**: Excellent! Note the parameters
- **Test rate drops**: May indicate sync issues, check log

## Background Execution

The test runs in background with `nohup`, so:
- ✅ Can close terminal
- ✅ Survives SSH disconnects
- ✅ Logs to file (glitch_marathon_20251028_081812.log)
- ✅ Results saved continuously to CSV

To stop test early:
```bash
kill 642668
# or
pkill -f glitch_marathon
```

Results are saved continuously, so partial results are available even if stopped early.
