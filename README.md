# Raiden Pico

A versatile glitching control platform built on the Raspberry Pi Pico 2 (RP2350), designed for hardware security research and fault injection experiments, written entirely by AI.

## Overview

Raiden Pico is a high-precision glitching tool that leverages the RP2350's PIO (Programmable I/O) state machines to generate precise glitch pulses for fault injection attacks. It supports multiple glitching methodologies including direct voltage glitching, ChipSHOUTER electromagnetic fault injection (EMFI), and clock glitching.

It was originally created as an experiment to see how useful AI could be in hardware hacking, but the goal has since evolved into making it the cheapest and most efficient glitching controller in order that it can be hardwired into test platforms to allow more reliable setups without having to waste or duplicate more expensive FPGA based controllers. 

All the code and test scripts in this project were written and tested by [Claude](https://www.claude.com/product/claude-code)*

The inspiration for the project came from discussions here: [Prompt||GTFO](https://www.knostic.ai/blog/prompt-gtfo-season-1).

\* Claude had full control of the Pico and a ChipShouter and was able to flash it and run against a real target. The only manual intervention required was validating timings etc. against an oscilloscope. I have not read a single line of code produced, only ran and tested it, so I cannot speak for the quality of the coding! YMMV!

### Key Features

- **Precise timing control**: PIO-based glitch generation with 6.67ns resolution (150 MHz system clock)
- **8 x Hardware UART**: Alternate configs are leveraged to share UART0/1 across multiple pinouts
- **Multiple trigger modes**: GPIO, UART byte matching, or software triggers
- **Platform support**: Manual, ChipSHOUTER EMFI, generic EMFI, and crowbar voltage glitching
- **Target support**: Built-in bootloader entry for LPC and STM32 microcontrollers
- **Command-line interface**: USB CDC serial interface with command shortcuts
- **GRBL XYZ support**: Direct UART control of GRBL CNC platform
- **Real-time monitoring**: UART snooping for target debugging

## Hardware Requirements

- **Raspberry Pi Pico 2** (RP2350)
- Target microcontroller
- Optional: ChipSHOUTER or voltage glitching hardware

## Installation

### Building from Source

```bash
# Set Pico SDK path
export PICO_SDK_PATH=/path/to/pico-sdk

# Build
./build.sh

# Flash
# 1. Hold BOOTSEL button while plugging in USB
# 2. Copy UF2 file:
cp build/raiden_pico.uf2 /media/$USER/RP2350/
```

Once flashed, for future updates the FLASH target can be used with make:

```
make FLASH
```

### Quick Flash via CLI

Connect to the Pico and reboot to bootloader:
```bash
# Connect to CLI
screen /dev/ttyACM0 115200

# Reboot to bootloader
REBOOT BL

# Then copy the UF2 file to the mounted drive
```

## Usage

### Connecting

```bash
screen /dev/ttyACM0 115200
# or
minicom -D /dev/ttyACM0 -b 115200
```

### Command Reference

All commands support non-ambiguous shortcuts (e.g., `STAT` for `STATUS`, `GL` for `GLITCH`).

#### System Commands

**`HELP`** - Display command reference
- Shows all available commands with usage examples

**`STATUS`** - Show system status
- Displays chip variant, armed state, glitch parameters, trigger mode, and pin configuration

**`PINS`** - Show pin configuration
- Lists current GPIO assignments for glitch output, triggers, and platform control

**`RESET`** - Reset system to default state
- Clears all configuration and returns to power-on defaults

**`REBOOT [BL]`** - Reboot the Pico
- `REBOOT` - Soft reboot (restart firmware)
- `REBOOT BL` - Reboot to bootloader mode for firmware updates

**`DEBUG [ON|OFF]`** - Toggle target UART debug display
- When enabled, displays all data received from target UART in real-time
- Useful for monitoring target behavior during glitching

#### Glitch Configuration

**`SET PAUSE <cycles>`** - Set glitch pause time
- Delay before glitch pulse in system clock cycles @ 150MHz
- Each cycle = 6.67ns
- Example: `SET PAUSE 1000` = 6.67µs delay

**`SET WIDTH <cycles>`** - Set glitch pulse width
- Duration of glitch pulse in clock cycles
- Example: `SET WIDTH 150` = 1µs pulse

**`SET GAP <cycles>`** - Set gap between glitches
- Delay between pulses when COUNT > 1
- Example: `SET GAP 100` = 667ns gap

**`SET COUNT <n>`** - Set number of glitches per trigger
- How many pulses to generate on each trigger event
- Useful for multi-pulse fault injection

**`GET PAUSE|WIDTH|GAP|COUNT`** - Read current parameter values
- Query individual glitch parameters
- Example: `GET WIDTH`

**`OUT <pin>`** - Set glitch output pin
- Configure which GPIO pin outputs the glitch pulse
- Default: GPIO 2
- Example: `OUT 5`

#### Trigger Configuration

**`TRIGGER NONE`** - Disable all triggers
- Glitches can only be manually fired with `GLITCH` command

**`TRIGGER GPIO <pin> <RISING|FALLING>`** - Configure GPIO trigger
- Trigger on edge detection of specified pin
- Example: `TRIGGER GPIO 10 RISING`
- Use case: Trigger from external signal or target GPIO

**`TRIGGER UART <byte>`** - Configure UART byte trigger (on TARGET UART)
- Trigger when specific byte is received from target UART
- Byte value in decimal (0-255)
- Example: `TRIGGER UART 0D` (trigger on '\r')
  - `TARGET SEND "R 0 4"`
- Use case: Trigger at specific point in target's execution

#### Platform Control

Raiden Pico supports multiple glitching platforms with different control requirements.

**`PLATFORM SET <MANUAL|CHIPSHOUTER|EMFI|CROWBAR>`**
- `MANUAL` - Direct control, no automatic platform management
- `CHIPSHOUTER` - NewAE ChipSHOUTER EMFI tool (UART control)
- `EMFI` - Generic EMFI platform with HV enable control
- `CROWBAR` - Voltage glitching with crowbar circuit

**`PLATFORM VOLTAGE <mv>`** - Set platform voltage
- For voltage glitching platforms
- Value in millivolts
- Example: `PLATFORM VOLTAGE 3300` (3.3V)

**`PLATFORM CHARGE <ms>`** - Set charge time
- For EMFI platforms that require capacitor charging
- Value in milliseconds
- Example: `PLATFORM CHARGE 100`

**`PLATFORM HVPIN <pin>`** - Set HV enable pin
- GPIO pin that controls high voltage enable
- Example: `PLATFORM HVPIN 15`

**`PLATFORM VPIN <pin>`** - Set voltage control pin
- GPIO pin for voltage control (PWM or analog)
- Example: `PLATFORM VPIN 14`

#### Glitch Execution

**`ARM ON|OFF`** - Arm/disarm glitch system
- Must be armed before glitches will fire on triggers
- Safety feature to prevent accidental glitching
- Example: `ARM ON`

**`GLITCH`** - Manually fire a single glitch
- Immediately generates glitch pulse(s)
- Works regardless of trigger configuration
- Useful for testing glitch parameters

#### Target Control

Raiden Pico includes built-in support for entering bootloader mode on common microcontrollers.

**`TARGET <LPC|STM32>`** - Set target microcontroller type
- Configures bootloader entry protocol
- `LPC` - NXP LPC series (ISP protocol)
- `STM32` - STMicroelectronics STM32 series

**`TARGET BOOTLOADER [baud] [crystal_khz]`** - Enter bootloader (alias: `TARGET BOOT`)
- Initialize target UART and enter bootloader mode
- Defaults: 115200 baud, 12000 kHz crystal
- LPC example: `TARGET BOOT 115200 12000`
- STM32 example: `TARGET BOOT 115200`

**`TARGET SYNC [baud] [crystal_khz] [reset_delay_ms]`** - Reset and enter bootloader
- Performs target reset, waits, then enters bootloader
- Includes automatic retry logic for reliability
- Defaults: 115200 baud, 12000 kHz crystal, 300ms reset delay
- Example: `TARGET SYNC 115200 12000 500`

**`TARGET SEND <hex|"text">`** - Send data to target
- Send hex bytes or quoted text to target UART
- Hex: `TARGET SEND 3F` (sends 0x3F)
- Text: `TARGET SEND "hello"` (automatically appends \r)

**`TARGET RESPONSE`** - Display target response buffer
- Shows data received from target UART
- Useful after sending bootloader commands

**`TARGET RESET [PERIOD <ms>] [PIN <n>] [HIGH]`** - Reset target
- Pulses reset pin to restart target
- Defaults: 300ms pulse, GPIO 15, active low
- Example: `TARGET RESET PERIOD 500 PIN 16 HIGH`

#### ChipSHOUTER Control

Built-in UART control for NewAE ChipSHOUTER EMFI tool.

**`CS ARM`** - Arm ChipSHOUTER
- Prepares ChipSHOUTER for firing
- Required before pulses can be generated

**`CS DISARM`** - Disarm ChipSHOUTER
- Safety disarm ChipSHOUTER
- Prevents accidental pulses

**`CS FIRE`** - Manually fire ChipSHOUTER
- Triggers single electromagnetic pulse
- ChipSHOUTER must be armed first

**`CS STATUS`** - Get ChipSHOUTER status
- Queries voltage, armed state, and configuration
- Returns UART response from ChipSHOUTER

**`CS VOLTAGE <V>`** - Set ChipSHOUTER voltage
- Configure pulse voltage (device-specific range)
- Example: `CS VOLTAGE 250`

**`CS PULSE <us>`** - Set ChipSHOUTER pulse width
- Configure pulse duration in microseconds
- Example: `CS PULSE 50`

**`CS TRIGGER HW <HIGH|LOW>`** - Set hardware trigger mode
- Configures ChipSHOUTER to trigger on glitch output pin
- `HIGH` - Active high trigger (pin idles low with pull-down)
- `LOW` - Active low trigger (pin idles high with pull-up)
- Example: `CS TRIGGER HW HIGH`

**`CS TRIGGER SW`** - Set software trigger mode
- ChipSHOUTER fires when interrupt routine calls `CS FIRE`
- Disables hardware trigger input

**`CS RESET`** - Reset ChipSHOUTER
- Clears errors and reinitializes ChipSHOUTER
- Verifies error state is cleared

#### GRBL XY Platform Control

Built-in UART control for GRBL-based XY positioning platforms (CNC routers, laser cutters, etc.) useful for automated EMFI probe positioning.

**Note**: The system uses UART1 for both Target (GP4/GP5) and GRBL (GP8/GP9). Only one can be active at a time - switching between them reconfigures the UART automatically.

**`GRBL SEND <gcode>`** - Send raw G-code command
- Send any G-code directly to GRBL controller
- Example: `GRBL SEND G0 X10 Y10`

**`GRBL UNLOCK`** - Unlock alarm state
- Sends `$X` to clear GRBL alarm
- Required after homing errors or emergency stops

**`GRBL SET HOME`** - Set current position as home
- Sets current XY position as origin (0,0,0)
- Sends `G92 X0 Y0 Z0`

**`GRBL HOME`** - Move to home position
- Rapid move to (0,0) coordinates
- Example: `GRBL HOME`

**`GRBL AUTOHOME`** - Auto-home with limit switches
- Sends `$H` to trigger GRBL's homing cycle
- Requires limit switches configured on GRBL controller

**`GRBL MOVE <X> <Y> [F]`** - Move to absolute position
- Move to specified XY coordinates
- Optional feed rate in mm/min (default: 300)
- Example: `GRBL MOVE 15 20` or `GRBL MOVE 15 20 500`

**`GRBL STEP <DX> <DY> [F]`** - Move relative distance
- Move by specified offset from current position
- Optional feed rate in mm/min (default: 300)
- Example: `GRBL STEP 5 -3` (move +5mm X, -3mm Y)

**`GRBL POS`** - Get current position
- Query current XYZ coordinates from GRBL
- Returns position in machine coordinates

**`GRBL RESET`** - Soft reset GRBL
- Sends Ctrl-X (0x18) to reset GRBL controller
- Use to recover from error states

## Typical Workflow

### 1. Basic Voltage Glitching

```bash
# Configure glitch parameters
SET PAUSE 1000      # 6.67µs before glitch
SET WIDTH 150       # 1µs glitch pulse
SET COUNT 1         # Single pulse
OUT 2               # Output on GPIO 2

# Set up trigger
TRIGGER GPIO 10 RISING

# Configure platform
PLATFORM SET CROWBAR
PLATFORM VOLTAGE 3300

# Arm and test
ARM ON
STATUS              # Verify configuration
```

### 2. UART-Triggered Glitching

```bash
# Set target type
TARGET LPC

# Enter bootloader
TARGET SYNC 115200 12000 300

# Configure UART trigger
TRIGGER UART 0D     # Trigger on '\r' (13)

# Configure glitch
SET PAUSE 500
SET WIDTH 100
ARM ON
TARGET SEND "R 0 516096"
```

### 3. ChipSHOUTER EMFI

```bash
# Configure ChipSHOUTER
CS VOLTAGE 250
CS PULSE 50
CS ARM

# Set up hardware trigger
CS TRIGGER HW HIGH

# Configure Raiden timing
SET PAUSE 2000
SET WIDTH 200
OUT 2

# Trigger from GPIO
TRIGGER GPIO RISING
ARM ON
```

## Pin Configuration

### Default Pinout

- **GPIO 2** - Glitch output (default)
- **GPIO 4** - Target UART TX
- **GPIO 5** - Target UART RX (also PIO monitored for UART triggers)
- **GPIO 15** - Target reset (active low)

### ChipSHOUTER Connection

- **GPIO 0** - ChipSHOUTER UART TX
- **GPIO 1** - ChipSHOUTER UART RX
- **GPIO 2** - Hardware trigger output (connects to ChipSHOUTER trigger input)

### GRBL XY Platform Connection

- **GPIO 8** - GRBL UART TX (UART1 alternate function)
- **GPIO 9** - GRBL UART RX (UART1 alternate function)

**Note**: GRBL uses UART1 which is shared with Target UART (GP4/GP5). Only one can be active at a time - commands auto-switch as needed.

## Architecture

### PIO State Machines

Raiden Pico uses the RP2350's PIO for precise timing-critical operations only:

- **PIO0 SM0** - GPIO edge detection for trigger input
- **PIO0 SM1** - Glitch pulse generation with cycle-accurate timing
- **PIO0 SM2** - UART RX decoder for byte-matching triggers (monitors GP5)
- **PIO0 SM3** - Clock generator for manual glitch triggering

### Hardware Peripherals

Non-timing-critical communications use standard hardware peripherals which can be shared by using alternate pin configs, 
effictively increasing the hardware UART capability from 2 to 8:

- **UART0** (GP0/GP1) - ChipSHOUTER communication (115200 baud)
- **UART1** (GP4/GP5) - Target device communication (configurable baud)
- **UART1** (GP8/GP9) - GRBL XYZ platform control

### Interrupt-Driven Design

- Target UART triggers are detected by PIO and generate IRQs
- Glitch execution happens in interrupt context for minimal latency
- Non-blocking UART reads prevent missed triggers

## Troubleshooting

### Target bootloader sync fails

LPC ISP sync typically requires 1-2 retries. Use `TARGET SYNC` which includes automatic retry logic:
```bash
TARGET LPC
TARGET SYNC 115200 12000 300
```

### No glitches generated

Check the following:
1. Is system armed? (`ARM ON`)
2. Is trigger configured? (`STATUS` to verify)
3. For GPIO triggers, is signal connected?
4. For UART triggers, is target sending data? (`DEBUG ON` to monitor)

### ChipSHOUTER not responding

1. Verify UART connection (GPIO 0/1)
2. Check baud rate (should be 115200)
3. Use `CS RESET` to reinitialize
4. Check with `CS STATUS`

## Technical Details

- **System Clock**: 150 MHz
- **Timing Resolution**: 6.67 ns (1 clock cycle)
- **UART Baud Rates**: Up to 921600 (configurable)
- **Glitch Width Range**: 1 cycle to 2^32 cycles (6.67ns to 28.6 seconds)
- **Trigger Latency**: < 1µs (PIO to glitch output)

## Safety Considerations

⚠️ **WARNING**: Fault injection can permanently damage hardware.

- Always verify glitch parameters before arming
- Start with conservative settings (longer pause, shorter width)
- Use `GLITCH` command to test before enabling triggers
- Keep target voltage within safe operating range
- ChipSHOUTER can generate high electromagnetic fields - follow device safety guidelines

## Development

### Building

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
mkdir build && cd build
cmake ..
make -j4
```

### Project Structure

```
raiden-pico/
├── src/           - C source files
├── include/       - Header files
├── build/         - Build artifacts
├── CMakeLists.txt - Build configuration
└── *.md          - Documentation
```

## License

This project is for hardware security research and educational purposes. Use responsibly and only on hardware you own or have permission to test.

## References

- [RP2350 Datasheet](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
- [ChipSHOUTER Documentation](https://chipshouter.readthedocs.io/)
- [Pico SDK Documentation](https://www.raspberrypi.com/documentation/pico-sdk/)

## Contributing

Contributions welcome! This project is maintained at https://github.com/AdamLaurie/raiden-pico

---

**Raiden Pico** - Precision fault injection for hardware security research
