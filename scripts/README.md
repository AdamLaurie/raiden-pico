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
# Single glitch test
./scripts/test_chipshouter_lpc_glitch.py

# Run multiple iterations
./scripts/test_chipshouter_lpc_glitch.py -n 10

# Quiet mode (less verbose output)
./scripts/test_chipshouter_lpc_glitch.py -q

# Multiple iterations with summary
./scripts/test_chipshouter_lpc_glitch.py -n 100
```

**Test Sequence:**

1. Reboots the Pico for clean state
2. Sets target to LPC
3. Syncs with LPC bootloader (115200 baud, 12MHz crystal, 10ms reset delay)
4. Configures UART trigger on byte 0x0d (carriage return)
5. Sets ChipSHOUTER to hardware trigger HIGH
6. Arms ChipSHOUTER
7. Arms Pico trigger system
8. Sends "R 0 516096" read command to target
9. Analyzes response:
   - Response "19" = Glitch FAILED (normal error response)
   - Response "0" = Glitch SUCCESS (security bypass)
   - No response or unexpected = May indicate successful glitch (target hung/crashed)

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
