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
# Single glitch test with default parameters (V=350, Pulse=8000 cycles)
./scripts/test_chipshouter_lpc_glitch.py

# Run multiple iterations
./scripts/test_chipshouter_lpc_glitch.py -n 10

# Custom voltage and pulse width
./scripts/test_chipshouter_lpc_glitch.py -v 320 -p 10000

# Custom voltage reduction step (default: 10V)
./scripts/test_chipshouter_lpc_glitch.py --voltage-step 20

# Quiet mode (less verbose output)
./scripts/test_chipshouter_lpc_glitch.py -q

# Multiple iterations with custom parameters
./scripts/test_chipshouter_lpc_glitch.py -n 100 -v 350 -p 8000
```

**Parameters:**

- `-n, --num-iterations`: Number of test iterations (default: 1)
- `-v, --voltage`: ChipSHOUTER voltage in volts (default: 350)
- `-p, --pulse-width`: Pico glitch pulse width in cycles (default: 8000)
- `--voltage-step`: Voltage reduction step on no-response failures (default: 10)
- `-q, --quiet`: Suppress verbose output

**Test Sequence:**

1. Reboots the Pico for clean state
2. Resets ChipSHOUTER
3. Sets target to LPC
4. Syncs with LPC bootloader (115200 baud, 12MHz crystal, 10ms reset delay)
5. Configures ChipSHOUTER voltage and Pico pulse width
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

## Notes

- All scripts use `/dev/ttyACM0` for Pico serial communication
- Default baud rate: 115200
- Scripts automatically handle Pico reboot and reconnection
- For best results, ensure ChipSHOUTER is properly configured with appropriate voltage, pulse width, and positioning
