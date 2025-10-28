# Raiden Pico Glitch Testing Scripts

This directory contains Python scripts for automated glitch testing and analysis.

## Requirements

```bash
pip install pyserial
```

## Scripts

### test_chipshouter_lpc_glitch.py

Tests ChipSHOUTER glitching of the LPC bootloader by attempting to bypass security checks during the bootloader read command.

**Usage:**

```bash
# Single glitch test with default parameters (V=350, Pause=8000, Width=150 cycles)
./scripts/test_chipshouter_lpc_glitch.py

# Run multiple iterations
./scripts/test_chipshouter_lpc_glitch.py -n 10

# Custom voltage, pause, and pulse width
./scripts/test_chipshouter_lpc_glitch.py -v 320 --pause 10000 -p 200

# Custom voltage reduction step (default: 10V)
./scripts/test_chipshouter_lpc_glitch.py --voltage-step 20

# Quiet mode (less verbose output)
./scripts/test_chipshouter_lpc_glitch.py -q

# Multiple iterations with custom parameters
./scripts/test_chipshouter_lpc_glitch.py -n 100 -v 350 --pause 8000 -p 150
```

**Parameters:**

- `-n, --num-iterations`: Number of test iterations (default: 1)
- `-v, --voltage`: ChipSHOUTER voltage in volts (default: 350)
- `--pause`: Pico glitch pause in cycles (default: 8000)
- `-p, --pulse-width`: Pico glitch pulse width in cycles (default: 150)
- `--voltage-step`: Voltage reduction step on no-response failures (default: 10)
- `-q, --quiet`: Suppress verbose output

**Test Sequence:**

1. Reboots the Pico for clean state
2. Resets ChipSHOUTER
3. Sets target to LPC
4. Syncs with LPC bootloader (115200 baud, 12MHz crystal, 10ms reset delay)
5. Configures glitch parameters:
   - ChipSHOUTER voltage
   - Pico glitch pause (delay before glitch trigger)
   - Pico glitch pulse width
6. Configures UART trigger on byte 0x0d (carriage return)
7. Sets ChipSHOUTER to hardware trigger HIGH
8. Arms ChipSHOUTER
9. Arms Pico trigger system
10. Sends "R 0 516096" read command to target
11. Analyzes response:
    - Response "19" = Glitch FAILED (normal error response)
    - Response "0" = Glitch SUCCESS (security bypass)
    - No response = Target hung/crashed (retries with reduced voltage)
12. If no response: Automatically reduces ChipSHOUTER voltage by voltage_step and retries until getting error 19 or success

**Example Output:**

```
ChipSHOUTER LPC Bootloader Glitch Test
============================================================

Iteration 1/10
------------------------------------------------------------
Rebooting Pico...
Reconnected to Pico

Setting up LPC target...
LPC bootloader sync complete

Configuring glitch parameters...

Performing glitch test...

✓ GLITCH SUCCESS - Security bypass detected!
```

### glitch_marathon.py

Long-running parameter sweep designed to run for hours. Tests parameter space repeatedly to find rare glitch windows. Logs all results to timestamped CSV files.

**Usage:**

```bash
# Run for 8 hours (default: 12 hours)
./scripts/glitch_marathon.py --hours 8

# Run in background
nohup python3 scripts/glitch_marathon.py --hours 24 > marathon.log 2>&1 &

# Monitor progress
tail -f marathon.log

# Analyze results
python3 scripts/analyze_glitch_results.py glitch_results_*.csv
```

**Parameter Space:**
- Voltage: 300-500V (5 values)
- Pause: 0-8000 cycles (fine granularity near response timing)
- Width: 100-250 cycles (7 values)
- Total combinations: ~1540 per cycle

**Performance:**
- ~0.43 tests/second (2.3s per test)
- ~1500 tests/hour
- Logs all results to `glitch_results_YYYYMMDD_HHMMSS.csv`

### analyze_glitch_results.py

Analyzes glitch test results from CSV log files.

**Usage:**

```bash
# Analyze specific file
python3 scripts/analyze_glitch_results.py glitch_results_20251028_081812.csv

# Analyze latest results
python3 scripts/analyze_glitch_results.py glitch_results_*.csv
```

**Output:**
- Success rate statistics
- Successful parameter combinations
- Crash parameter ranges
- Timing statistics

## Notes

- All scripts use `/dev/ttyACM0` for Pico serial communication
- Default baud rate: 115200
- Scripts automatically handle Pico reboot and reconnection
- For best results, ensure ChipSHOUTER is properly configured with appropriate voltage, pulse width, and positioning
- **Marathon tests**: Run for hours (8-24h) to find narrow glitch windows
- **Results**: All results logged to timestamped CSV files for later analysis
