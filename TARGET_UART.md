# Target UART Integration

## Overview

The Raiden Pico includes a third UART interface specifically for communicating with target device bootloaders and firmware. This PIO-based UART provides flexible pin assignment and configurable baud rates, enabling you to send commands, read responses, and interact with target devices during glitching campaigns.

## Hardware Connection

### Wiring
```
Raiden Pico GP16 (TX) -----> Target Device RX
Raiden Pico GP17 (RX) -----> Target Device TX
Raiden Pico GND -----------> Target Device GND
```

### UART Parameters (Default)
- **Baud Rate**: 115200 (configurable 300-1000000)
- **Data Bits**: 8
- **Parity**: None
- **Stop Bits**: 1
- **Flow Control**: None

### Custom Pins
If GP16/GP17 conflict with your setup:
```
TARGET PINS 18 19    # Use GP18(TX)/GP19(RX) instead
```

## Implementation Details

### Hardware UART
The target UART uses **hardware UART1** (GP4/GP5) for reliable communication:

- **UART1 TX** (GP4) - Transmit to target
- **UART1 RX** (GP5) - Receive from target
- **PIO0 SM2** - Parallel monitoring of GP5 for byte-matching triggers only

Benefits:
- Hardware UART provides reliable, efficient communication
- Fixed pins (GP4/GP5) for UART1
- Any baud rate (300-1000000)
- PIO monitoring enables byte-matching triggers without interfering with UART data flow

### State Machine Allocation
```
PIO0:
  SM0: Trigger detection
  SM1: Pulse generation
  SM2: Flag outputs
  SM3: Clock generation
  SM4: Voltage PWM
  SM5: HV enable
  SM6: Status monitor
  SM7: Target UART TX ✓

PIO1:
  SM0: Target UART RX ✓
  SM1-3: Available for future use
```

## Available Commands

### 1. Status Query
**Command**: `TARGET`

**Purpose**: Show target UART configuration and status

**Example**:
```
> TARGET
Target UART:
  TX Pin: GP16
  RX Pin: GP17
  Baud Rate: 115200
  Status: Active
  TX FIFO: 0/8
  RX FIFO: 0/8
```

### 2. Initialize UART
**Command**: `TARGET INIT`

**Purpose**: Initialize PIO-based UART with current settings

**Example**:
```
> TARGET INIT
OK: Target UART initialized on GP16/GP17 @ 115200 baud
```

**Notes**:
- Automatically called when pins or baud rate changed
- Safe to call multiple times (reinitializes)

### 3. Configure Baud Rate
**Command**: `TARGET BAUD <rate>`

**Purpose**: Set UART baud rate (300-1000000)

**Examples**:
```
> TARGET BAUD 9600
OK: Target UART baud rate = 9600

> TARGET BAUD 115200
OK: Target UART baud rate = 115200

> TARGET BAUD 921600
OK: Target UART baud rate = 921600
```

**Notes**:
- Automatically reinitializes UART if already active
- Common rates: 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600

### 4. Configure UART Pins
**Command**: `TARGET PINS <tx> <rx>`

**Purpose**: Set custom TX and RX pins (default GP16/GP17)

**Example**:
```
> TARGET PINS 18 19
OK: Target UART on GP18(TX)/GP19(RX)
```

**Constraints**:
- Pins must be in range GP7-GP29
- Cannot use reserved pins (GP0-GP6)

### 5. Send Data
**Command**: `TARGET SEND <data>`

**Purpose**: Send data to target device (newline added automatically)

**Examples**:
```
> TARGET SEND help
OK: Sent 5 bytes to target

> TARGET SEND reboot
OK: Sent 7 bytes to target

> TARGET SEND read_flash 0x1000
OK: Sent 18 bytes to target
```

**Notes**:
- Newline (`\n`) is automatically appended
- Multi-word commands are supported (spaces preserved)

### 6. Read Data (Raw)
**Command**: `TARGET READ [timeout_ms]`

**Purpose**: Read raw data from target (hex display)

**Examples**:
```
> TARGET READ 100
TARGET RX (12 bytes): 48656C6C6F20576F726C640A
  ASCII: Hello World

> TARGET READ 500
TARGET RX: (no data)
```

**Notes**:
- Default timeout: 100ms
- Displays both hex and ASCII (if printable)
- Returns all available bytes up to 256 bytes max

### 7. Read Line
**Command**: `TARGET READLINE [timeout_ms]`

**Purpose**: Read data until newline (`\n` or `\r`)

**Examples**:
```
> TARGET READLINE 1000
TARGET RX: OK: Ready

> TARGET READLINE 2000
TARGET RX: (timeout)
```

**Notes**:
- Default timeout: 1000ms
- Stops at first newline character
- Returns partial line on timeout if any data received

### 8. Send Command and Read Response
**Command**: `TARGET CMD <command>`

**Purpose**: Send command, flush RX buffer, and wait for response

**Examples**:
```
> TARGET CMD version
TARGET: v1.2.3

> TARGET CMD status
TARGET: UNLOCKED

> TARGET CMD help
TARGET: Available commands: help, version, status, reboot, unlock
```

**Notes**:
- Flushes RX buffer before sending
- Waits up to 1000ms for response
- Most convenient for interactive bootloaders

### 9. Flush RX Buffer
**Command**: `TARGET FLUSH`

**Purpose**: Clear RX FIFO (discard pending data)

**Example**:
```
> TARGET FLUSH
OK: Target UART RX buffer flushed
```

**Use Case**: Clear stale data before capturing new response

---

## Typical Workflows

### Workflow 1: Bootloader Interaction
Interact with target bootloader to verify glitch success:

```
# 1. Initialize target UART
TARGET INIT

# 2. Configure glitching
PLATFORM SET CHIPSHOUTER
PLATFORM VOLTAGE 200
PLATFORM HVPIN 20
PLATFORM VPIN 21
IN 10
OUT 15
TRIGGER GPIO
SET PAUSE 1000
SET WIDTH 50
SET COUNT 1

# 3. Arm system
ARM ON

# 4. Send command to target (trigger bootloader prompt)
TARGET SEND help

# 5. Execute glitch
GLITCH

# 6. Wait for glitch to complete, then check response
TARGET READLINE 1000

# 7. Check if glitch succeeded (e.g., unlocked bootloader)
TARGET CMD status
```

### Workflow 2: Automated Glitching with Response Capture
Python script for automated glitching with bootloader interaction:

```python
#!/usr/bin/env python3
import serial
import time

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)

def send(cmd):
    ser.write((cmd + '\r\n').encode())
    time.sleep(0.1)
    return ser.read(ser.in_waiting).decode()

# Configure Raiden
send('PLATFORM SET CHIPSHOUTER')
send('PLATFORM VOLTAGE 250')
send('PLATFORM HVPIN 20')
send('PLATFORM VPIN 21')
send('IN 10')
send('OUT 15')
send('TRIGGER GPIO')
send('SET PAUSE 500')
send('SET WIDTH 50')
send('ARM ON')

# Configure target UART
send('TARGET INIT')
send('TARGET BAUD 115200')

# Glitching loop
for i in range(100):
    print(f"Attempt {i+1}/100")

    # Reset target (via external control)
    # ... (GPIO toggle, power cycle, etc.)
    time.sleep(0.5)

    # Send bootloader unlock command
    send('TARGET SEND unlock secret_key')
    time.sleep(0.1)

    # Execute glitch
    send('GLITCH')
    time.sleep(0.5)

    # Check bootloader response
    response = send('TARGET READLINE 1000')
    print(f"  Response: {response}")

    # Check for success
    if 'UNLOCKED' in response or 'SUCCESS' in response:
        print("*** BOOTLOADER UNLOCKED! ***")
        break

    # Reset for next attempt
    send('RESET')

# Cleanup
send('ARM OFF')
ser.close()
```

### Workflow 3: Multi-UART Glitching
Use all three UARTs simultaneously:

```
# UART0 (GP0/GP1): CLI commands from PC
# UART1 (GP8/GP9): ChipShouter control
# PIO UART (GP16/GP17): Target bootloader

# Configure ChipShouter via UART
CHIPSHOT VOLTAGE 250
CHIPSHOT ARM

# Configure target UART
TARGET INIT
TARGET BAUD 9600

# Send command to target
TARGET SEND start_secure_boot

# Execute glitch during secure boot
GLITCH

# Query ChipShouter status
CHIPSHOT

# Read target response
TARGET READLINE 2000

# Disarm everything
ARM OFF
CHIPSHOT DISARM
```

---

## UART Triggering with Bootloader Communication

### Important: Shared RX Pin Requirement

**When combining UART triggering with target bootloader communication, you MUST use the same GPIO pin for both the bootloader RX (Target TX → Pico RX) and the UART trigger input.**

This is because:
- UART triggering (SM0) monitors incoming bytes on the `IN` pin
- Target UART RX (PIO1 SM0) receives data from the target on the RX pin
- **Both need to monitor the same data stream from the bootloader**

### Workflow: UART-Triggered Glitching with Bootloader

Trigger glitches based on specific bootloader messages:

```
# 1. Configure target UART with shared RX pin
TARGET PINS 16 17       # GP16=TX (Pico→Target), GP17=RX (Target→Pico)
TARGET BAUD 115200
TARGET INIT

# 2. Configure UART trigger on the SAME pin as target RX
IN 17                   # MUST match TARGET RX pin (GP17)
HEX 0x3E                # Trigger on '>' character (bootloader prompt)
TRIGGER UART            # Enable UART byte matching on GP17

# 3. Configure glitch output
OUT 15
SET PAUSE 100           # 100µs after seeing '>'
SET WIDTH 50            # 50µs glitch pulse
SET COUNT 1

# 4. Configure platform
PLATFORM SET CHIPSHOUTER
PLATFORM VOLTAGE 250
PLATFORM HVPIN 20
PLATFORM VPIN 21

# 5. Arm and wait
ARM ON
RUN                     # Start monitoring

# Now when bootloader sends '>' character:
#   - UART trigger (SM0) detects 0x3E and fires glitch
#   - Target UART RX (PIO1 SM0) also receives the byte
#   - Both operate on the same GPIO pin (GP17)
```

### Example: Trigger on Bootloader Prompt

Many bootloaders send a prompt character (like `>` or `$`) when ready. You can trigger glitches precisely when this appears:

```
# Configure for '>' (0x3E) bootloader prompt
TARGET PINS 16 17
TARGET BAUD 115200
TARGET INIT

IN 17                   # Same as TARGET RX pin!
HEX 0x3E                # ASCII '>' character
TRIGGER UART

OUT 15
SET PAUSE 50            # Glitch 50µs after prompt
SET WIDTH 100
SET COUNT 1

ARM ON
RUN

# Reset target device to trigger bootloader
TARGET RESET

# The bootloader will send '>...' and:
# 1. UART trigger fires glitch at '>'
# 2. Target UART captures the full response
# 3. Read the result after glitch
TARGET READLINE 1000
```

### Python Automation with UART Triggering

```python
#!/usr/bin/env python3
import serial
import time

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)

def send(cmd):
    ser.write((cmd + '\r\n').encode())
    time.sleep(0.05)
    return ser.read(ser.in_waiting).decode()

# Configure target UART (GP17 = RX from bootloader)
send('TARGET PINS 16 17')
send('TARGET BAUD 115200')
send('TARGET INIT')

# Configure UART trigger on SAME pin as target RX
send('IN 17')                    # MUST match TARGET RX!
send('HEX 0x3E')                 # Trigger on '>' prompt
send('TRIGGER UART')

# Configure glitch timing
send('OUT 15')
send('SET PAUSE 100')
send('SET WIDTH 50')
send('SET COUNT 1')

# Configure platform
send('PLATFORM SET CHIPSHOUTER')
send('PLATFORM VOLTAGE 250')
send('PLATFORM HVPIN 20')
send('PLATFORM VPIN 21')
send('ARM ON')

# Configure target reset
send('TARGET RESET PIN 18 PERIOD 100')

# Glitching loop
for i in range(100):
    print(f"Attempt {i+1}/100")

    # Reset target to trigger bootloader
    send('TARGET RESET')
    time.sleep(0.1)

    # Start monitoring for UART trigger
    send('RUN')

    # Wait for glitch to complete
    time.sleep(0.5)

    # Read bootloader response
    response = send('TARGET READLINE 1000')
    print(f"  Bootloader: {response}")

    # Check for success indicators
    if 'UNLOCKED' in response or 'DEBUG' in response:
        print("*** GLITCH SUCCESS! ***")
        break

    # Reset for next attempt
    send('RESET')

send('ARM OFF')
ser.close()
```

### Pin Configuration Summary

**Correct configuration (shared RX pin):**
```
TARGET PINS 16 17    # GP16=TX to target, GP17=RX from target
IN 17                # UART trigger watches GP17 (same as TARGET RX)
TRIGGER UART         # Both SM0 and PIO1 SM0 monitor GP17
```

**Incorrect configuration (different pins):**
```
TARGET PINS 16 17    # GP17=RX from target
IN 10                # WRONG! Trigger won't see bootloader data
TRIGGER UART         # SM0 watches GP10, PIO1 SM0 watches GP17 (disconnected)
```

### How It Works Internally

When configured correctly:

1. **Bootloader sends data** → Target TX pin → **Raiden GP17** (shared)
2. **PIO SM0 (UART trigger)** monitors GP17 for hex byte match (e.g., 0x3E)
3. **PIO1 SM0 (Target UART RX)** also monitors GP17, captures all bytes
4. **When trigger byte detected** → SM0 fires IRQ → starts pulse output
5. **Target UART RX continues** receiving data independently
6. **Result**: Glitch fires at exact bootloader message, and full response is captured

### State Machine Sharing

Both state machines can safely monitor the same pin:
- **SM0 (Trigger)**: Rising edge + byte matching, fires once, then stops
- **PIO1 SM0 (Target RX)**: Continuous byte reception, always active
- **No conflict**: Both are read-only operations on the input pin

---

## Integration with RESPONSE Capture

The TARGET UART can be used alongside the existing RESPONSE capture functionality:

### Option 1: Use TARGET commands directly
```
TARGET SEND read_memory 0x1000
TARGET READLINE 500
```

### Option 2: Use RESPONSE CAPTURE
```
# Send command via TARGET UART
TARGET SEND read_memory 0x1000

# Wait a bit for target to respond
# (Response goes to CLI UART0, not target UART)
RESPONSE CAPTURE 500
RESPONSE
```

**Note**: The `RESPONSE CAPTURE` command captures data from **UART0 (CLI)**, not the target UART. Use `TARGET READ` or `TARGET READLINE` to capture from the target device.

---

## Performance

### Timing
- **Baud Rate Range**: 300-1000000 bps
- **Command Latency**: 10-50ms (PIO processing + UART transfer)
- **Max Throughput**: ~10,000 characters/second @ 115200 baud

### PIO UART vs Hardware UART
| Feature | Hardware UART | PIO UART (Target) |
|---------|---------------|-------------------|
| **Speed** | Up to 921600 baud | Up to 1000000 baud |
| **Pin Flexibility** | Fixed pins | Any GP7-29 |
| **CPU Overhead** | Very low (DMA) | Low (PIO handles bits) |
| **Accuracy** | High | High (PIO clock) |
| **Use Case** | CLI, ChipShouter | Target devices |

### Practical Limits
- **Max baud rate**: 1Mbps (higher rates may have timing errors)
- **Buffer size**: 256 bytes (can be increased if needed)
- **FIFO depth**: 8 bytes (PIO limitation)

---

## Troubleshooting

### No Response from Target
**Symptom**: `TARGET READLINE` returns `(timeout)`

**Causes**:
- Target not powered or reset
- Wrong TX/RX connections (swap TX/RX)
- Incorrect baud rate
- Target not sending newline character

**Solutions**:
```
# Check baud rate
TARGET BAUD 9600

# Try raw read instead of readline
TARGET READ 1000

# Flush and retry
TARGET FLUSH
TARGET SEND help
TARGET READ 500

# Check wiring (TX to RX, RX to TX)
TARGET PINS 16 17
```

### Garbled Data
**Symptom**: `TARGET RX: ��������`

**Causes**:
- Baud rate mismatch
- Wrong data format (expecting 8N1)
- Electrical noise or bad connection

**Solutions**:
```
# Try common baud rates
TARGET BAUD 9600
TARGET BAUD 115200
TARGET BAUD 921600

# Check target device documentation for correct baud rate
```

### Pin Conflicts
**Symptom**: `ERROR: Cannot use reserved pins`

**Solution**:
```
# Avoid GP0-GP6 (reserved for CLI, flags, clock)
TARGET PINS 18 19

# Check current pin assignments
PINS
```

### PIO State Machine Not Initializing
**Symptom**: `ERROR: Failed to initialize target UART`

**Causes**:
- PIO state machines already in use
- Invalid pin configuration

**Solutions**:
```
# Try different pins
TARGET PINS 20 21

# Reinitialize
TARGET INIT
```

---

## Advanced Usage

### Custom Bootloader Commands
Access vendor-specific bootloader features:

```
# STM32 bootloader example
TARGET BAUD 115200
TARGET SEND 0x7F        # STM32 bootloader sync byte
TARGET READLINE 100

# Send bootloader command
TARGET CMD 0x00 0xFF    # Get version command
TARGET READ 200

# Read memory
TARGET CMD 0x11 0xEE    # Read memory command
TARGET READ 500
```

### Automated Firmware Extraction
Glitch past authentication, then dump firmware:

```python
# After successful glitch
send('TARGET CMD read_flash 0x0000 256')
firmware = send('TARGET READ 2000')

# Parse hex response and save
with open('firmware_dump.bin', 'wb') as f:
    # ... (parse hex and write bytes)
```

### Multi-Stage Glitching
Use target UART to control multi-stage attacks:

```
# Stage 1: Bypass bootloader lock
TARGET SEND unlock
GLITCH
TARGET READLINE 1000

# Stage 2: Enable debug mode
TARGET SEND enable_debug
GLITCH
TARGET READLINE 1000

# Stage 3: Extract secret
TARGET SEND dump_key
TARGET READ 2000
```

---

## Pin Allocation Summary

### Default Target UART Interface
| Pin | Function | Direction | Protocol |
|-----|----------|-----------|----------|
| GP16 | UART TX | Output | Target UART (PIO SM7) |
| GP17 | UART RX | Input | Target UART (PIO1 SM0) |

### Reserved Pins
- **GP0, GP1**: Raiden CLI UART (hardware UART0)
- **GP2-GP6**: Flag outputs and clock
- **GP7-29**: User-configurable (including target UART)
- **GP8, GP9**: ChipShouter UART (default, hardware UART1)

### Example Full Configuration
```
GP0  = CLI TX (fixed)
GP1  = CLI RX (fixed)
GP2  = ERROR flag (fixed)
GP3  = RUNNING flag (fixed)
GP4  = TRIGGERED flag (fixed)
GP5  = FINISHED flag (fixed)
GP6  = CLOCK output (fixed)
GP8  = ChipShouter UART TX (default)
GP9  = ChipShouter UART RX (default)
GP10 = Trigger input (glitch detection)
GP15 = Pulse output (glitch trigger)
GP16 = Target UART TX (default)
GP17 = Target UART RX (default)
GP20 = ChipShouter HV enable
GP21 = ChipShouter voltage control
```

---

## Protocol Details

### PIO UART Format
**Frame**: 8N1 (8 data bits, no parity, 1 stop bit)

**Timing**:
- Start bit: 1 bit period (LOW)
- Data bits: 8 bits (LSB first)
- Stop bit: 1 bit period (HIGH)

**PIO Clock**: `baudrate * 8` (8 PIO cycles per bit)

**Example for 115200 baud**:
- PIO clock: 115200 * 8 = 921600 Hz
- Bit period: ~8.68 µs
- Byte period: ~86.8 µs (including start/stop)

### Transmit (TX) Timing
```
PIO SM7 @ 921600 Hz (for 115200 baud):
  1. pull() - Get byte from FIFO
  2. Start bit (LOW) - 8 cycles
  3. Data bits (8x) - 8 cycles each
  4. Stop bit (HIGH) - 8 cycles
Total: ~70 cycles = ~76 µs per byte
```

### Receive (RX) Timing
```
PIO1 SM0 @ 921600 Hz (for 115200 baud):
  1. wait(0, pin, 0) - Wait for start bit
  2. Delay to center of first data bit - 12 cycles
  3. Sample data bits (8x) - 8 cycles each
  4. Check stop bit - 1 cycle
  5. push() - Push byte to FIFO
Total: ~72 cycles = ~78 µs per byte
```

---

## Further Reading

- [MicroPython PIO Documentation](https://docs.micropython.org/en/latest/library/rp2.html)
- [RP2350 PIO User Guide](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
- [UART Protocol Specification](https://en.wikipedia.org/wiki/Universal_asynchronous_receiver-transmitter)
- [Bootloader Attack Techniques](https://www.riscure.com/blog/bootloader-fault-injection)
