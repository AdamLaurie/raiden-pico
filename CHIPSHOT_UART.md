# ChipShouter UART Integration

## Overview

The Raiden Pico can communicate directly with the ChipShouter via UART, allowing full remote configuration and status monitoring without a PC connection to the ChipShouter.

## Hardware Connection

### Wiring
```
Raiden Pico GP8 (TX) -----> ChipShouter RX
Raiden Pico GP9 (RX) -----> ChipShouter TX
Raiden Pico GND ---------> ChipShouter GND
```

### UART Parameters
- **Baud Rate**: 115200
- **Data Bits**: 8
- **Parity**: None
- **Stop Bits**: 1
- **Flow Control**: None

### Custom Pins
If GP8/GP9 conflict with your setup:
```
CHIPSHOT UARTPINS 12 13    # Use GP12(TX)/GP13(RX) instead
```

## Available Commands

### 1. Status Query
**Command**: `CHIPSHOT`

**Purpose**: Query ChipShouter status

**Example**:
```
> CHIPSHOT
ChipShouter Status:
  Armed: TRUE
  Voltage: 250V
  Temperature: 35C
  Capacitor: Charged
```

### 2. Voltage Control
**Query**: `CHIPSHOT VOLTAGE`

**Set**: `CHIPSHOT VOLTAGE <volts>`

**Purpose**: Query or set ChipShouter output voltage

**Examples**:
```
> CHIPSHOT VOLTAGE
ChipShouter Voltage: 250V

> CHIPSHOT VOLTAGE 300
OK: ChipShouter voltage set to 300V
```

**Notes**:
- Setting via UART also updates local `PLATFORM VOLTAGE` config
- Range: 0-500V (ChipShouter dependent)

### 3. Arm/Disarm

**Arm**: `CHIPSHOT ARM`

**Disarm**: `CHIPSHOT DISARM`

**Purpose**: Arm or disarm ChipShouter via UART

**Examples**:
```
> CHIPSHOT ARM
OK: ChipShouter armed via UART

> CHIPSHOT DISARM
OK: ChipShouter disarmed via UART
```

**Notes**:
- This is independent of Raiden's `ARM ON/OFF` command
- Use `CHIPSHOT ARM` to arm ChipShouter hardware
- Use `ARM ON` to arm Raiden's glitching system
- For full operation, both must be armed

### 4. Reset
**Command**: `CHIPSHOT RESET`

**Purpose**: Reset ChipShouter to default state

**Example**:
```
> CHIPSHOT RESET
OK: ChipShouter reset
```

### 5. Settings Sync
**Command**: `CHIPSHOT SYNC`

**Purpose**: Read ChipShouter settings and update Raiden to match

**Example**:
```
> CHIPSHOT SYNC
OK: Synced voltage from ChipShouter: 275V
```

**Use Case**: If ChipShouter voltage was changed manually or by another controller, sync Raiden's settings to match.

### 6. UART Pin Configuration
**Command**: `CHIPSHOT UARTPINS <tx> <rx>`

**Purpose**: Change UART pins (default GP8/GP9)

**Example**:
```
> CHIPSHOT UARTPINS 12 13
OK: ChipShouter UART on GP12(TX)/GP13(RX)
```

**Constraints**:
- Pins must be in range GP7-GP29
- Cannot use reserved pins (GP0-GP6)

### 7. Raw Command
**Command**: `CHIPSHOT CMD <raw_command>`

**Purpose**: Send arbitrary command to ChipShouter

**Examples**:
```
> CHIPSHOT CMD help
ChipShouter: Available commands: voltage, arm, disarm, status, reset...

> CHIPSHOT CMD temperature
ChipShouter: Temperature: 37C

> CHIPSHOT CMD version
ChipShouter: Firmware v2.1.0
```

**Use Case**: Access ChipShouter-specific features not directly supported by Raiden commands.

## Typical Workflows

### Workflow 1: Full Remote Control
Control ChipShouter entirely from Raiden Pico (no PC needed):

```
# 1. Configure platform
PLATFORM SET CHIPSHOUTER
PLATFORM HVPIN 20
PLATFORM VPIN 21
PLATFORM CHARGE 5000

# 2. Configure ChipShouter via UART
CHIPSHOT VOLTAGE 250
CHIPSHOT ARM

# 3. Configure trigger/output
IN 10
OUT 15
TRIGGER GPIO
SET PAUSE 1000
SET WIDTH 50
SET COUNT 1

# 4. Arm Raiden and glitch
ARM ON
GLITCH

# 5. Disarm everything
ARM OFF
CHIPSHOT DISARM
```

### Workflow 2: Query Status During Operation
Monitor ChipShouter health during glitching campaign:

```python
#!/usr/bin/env python3
import serial
import time

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)

def send(cmd):
    ser.write((cmd + '\r\n').encode())
    time.sleep(0.1)
    return ser.read(ser.in_waiting).decode()

# Configure system
send('PLATFORM SET CHIPSHOUTER')
send('CHIPSHOT VOLTAGE 250')
send('IN 10')
send('OUT 15')
send('SET PAUSE 500')
send('SET WIDTH 50')
send('ARM ON')

# Glitching loop with status monitoring
for i in range(100):
    # Execute glitch
    send('GLITCH')
    time.sleep(0.5)

    # Check ChipShouter every 10 glitches
    if i % 10 == 0:
        response = send('CHIPSHOT')
        print(f"Glitch {i}: {response}")

        # Check temperature
        temp_resp = send('CHIPSHOT CMD temperature')
        if 'Temperature' in temp_resp:
            temp = int(temp_resp.split(':')[1].split('C')[0])
            if temp > 50:
                print("WARNING: ChipShouter overheating!")
                send('ARM OFF')
                send('CHIPSHOT DISARM')
                break

    send('RESET')

# Cleanup
send('ARM OFF')
send('CHIPSHOT DISARM')
ser.close()
```

### Workflow 3: Hybrid Control
Use both GPIO and UART control simultaneously:

```
# Setup
PLATFORM SET CHIPSHOUTER
PLATFORM HVPIN 20           # GPIO HV enable (PIO-controlled)
PLATFORM VPIN 21            # GPIO voltage control (PIO PWM)
PLATFORM CHARGE 5000

# Configure ChipShouter voltage via UART
CHIPSHOT VOLTAGE 250

# Arm via both methods
ARM ON                      # Arms Raiden + enables HV via GPIO
CHIPSHOT ARM                # Arms ChipShouter via UART

# Glitch
GLITCH

# Status
STATUS                      # Raiden status
CHIPSHOT                    # ChipShouter status

# Disarm
ARM OFF
CHIPSHOT DISARM
```

## Control Methods Comparison

| Feature | GPIO Control | UART Control | Combined |
|---------|-------------|--------------|----------|
| **HV Enable** | ✅ PIO SM5 | ✅ UART command | ✅ Both |
| **Voltage Set** | ✅ PIO PWM SM4 | ✅ UART command | ✅ Both |
| **Status Query** | ❌ No | ✅ Full status | ✅ UART |
| **Arm/Disarm** | ✅ HV pin | ✅ UART command | ✅ Both |
| **Latency** | <1µs (hardware) | ~50ms (UART) | Depends on method |
| **Reliability** | ✅ Hardware | ⚠️ UART may timeout | ✅ GPIO primary |

### Recommendation
- **Use GPIO** for time-critical controls (HV enable, voltage)
- **Use UART** for queries, configuration, diagnostics
- **Combine both** for maximum flexibility and monitoring

## Troubleshooting

### No Response from ChipShouter
**Symptom**: `ERROR: No response from ChipShouter`

**Causes**:
- UART not connected
- Wrong TX/RX pin configuration
- ChipShouter not powered
- Incorrect baud rate (should be 115200)

**Solutions**:
```
# Check UART pins
CHIPSHOT UARTPINS 8 9

# Try reinitializing platform
PLATFORM SET CHIPSHOUTER

# Send simple command
CHIPSHOT CMD help
```

### Voltage Mismatch
**Symptom**: Raiden shows 250V but ChipShouter shows 200V

**Solution**:
```
# Sync from ChipShouter to Raiden
CHIPSHOT SYNC
```

Or set explicitly:
```
CHIPSHOT VOLTAGE 250        # Set ChipShouter via UART
PLATFORM VOLTAGE 250        # Set Raiden local config
```

### UART Conflicts
**Symptom**: GPIO pins GP8/GP9 already in use

**Solution**:
```
# Use different pins before initializing platform
CHIPSHOT UARTPINS 12 13
PLATFORM SET CHIPSHOUTER
```

### Timeout Errors
**Symptom**: `ERROR: Failed to arm ChipShouter` (timeout)

**Causes**:
- ChipShouter busy charging
- UART buffer full
- ChipShouter in error state

**Solutions**:
```
# Reset ChipShouter
CHIPSHOT RESET

# Check status
CHIPSHOT

# Try again
CHIPSHOT ARM
```

## Advanced Usage

### Custom ChipShouter Commands
Access ChipShouter-specific features:

```
# Query firmware version
CHIPSHOT CMD version

# Check error log
CHIPSHOT CMD errors

# Set pulse count (if supported)
CHIPSHOT CMD pulsecount 5

# Calibration (if supported)
CHIPSHOT CMD calibrate
```

### Automated Voltage Sweeps
Sweep ChipShouter voltage via UART:

```python
for voltage in range(150, 351, 10):
    send(f'CHIPSHOT VOLTAGE {voltage}')
    time.sleep(0.1)  # Wait for ChipShouter to adjust

    # Verify setting
    response = send('CHIPSHOT VOLTAGE')
    print(f"Set {voltage}V: {response}")

    # Execute glitch
    send('GLITCH')
    time.sleep(0.5)
    send('RESET')
```

### Health Monitoring
Continuous ChipShouter health check:

```python
def monitor_chipshot():
    """Background thread to monitor ChipShouter health"""
    while True:
        # Query temperature
        temp_resp = send('CHIPSHOT CMD temperature')

        # Query status
        status = send('CHIPSHOT')

        # Check for warnings
        if 'ERROR' in status or 'WARNING' in status:
            print(f"ChipShouter alert: {status}")
            # Trigger alarm/disarm

        time.sleep(5)  # Check every 5 seconds

# Start monitoring in background
threading.Thread(target=monitor_chipshot, daemon=True).start()
```

## Protocol Details

### ChipShouter UART Protocol
**Command Format**: `<command> [parameters]\n`

**Response Format**: `<data>\n` or `OK\n` or `ERROR: <message>\n`

**Common Commands** (ChipShouter-specific, may vary by model):
- `voltage` - Query voltage
- `voltage <V>` - Set voltage
- `arm` - Arm system
- `disarm` - Disarm system
- `status` - Full status
- `reset` - Reset
- `help` - Show available commands
- `version` - Firmware version
- `temperature` - Current temperature
- `errors` - Error log

### Raiden UART Wrapper
Raiden wraps ChipShouter UART with:
- Automatic timeout handling (500ms default)
- Response parsing
- Error detection
- Local config synchronization

## Pin Allocation

### Default ChipShouter Interface
| Pin | Function | Direction | Protocol |
|-----|----------|-----------|----------|
| GP8 | UART TX | Output | ChipShouter UART |
| GP9 | UART RX | Input | ChipShouter UART |
| GP15 | Trigger | Output | Pulse (to ChipShouter TRIGGER IN) |
| GP20 | HV Enable | Output | GPIO (to ChipShouter HV_EN) |
| GP21 | Voltage Control | Output | PWM (to ChipShouter VOLTAGE_CTRL) |

### Reserved Pins
- GP0, GP1: Raiden CLI UART
- GP2-GP6: Flag outputs and clock
- GP7-29: User-configurable

## Performance

### UART Communication
- **Baud Rate**: 115200 bps
- **Command Latency**: 10-50ms (depends on ChipShouter response)
- **Max Throughput**: ~20 commands/second
- **Timeout**: 500ms default (configurable in code)

### GPIO vs UART
| Operation | GPIO | UART |
|-----------|------|------|
| Voltage change | <1µs (PIO) | ~50ms (UART round-trip) |
| HV enable | <1µs (PIO) | ~50ms (UART round-trip) |
| Status query | N/A | ~50ms (UART round-trip) |

**Recommendation**: Use GPIO for real-time control, UART for configuration and monitoring.

## Further Reading

- [ChipShouter User Manual](https://www.newae.com/chipShouter) - Official ChipShouter documentation
- [UART Protocol Specification](https://en.wikipedia.org/wiki/Universal_asynchronous_receiver-transmitter)
- [MicroPython UART API](https://docs.micropython.org/en/latest/library/machine.UART.html)
