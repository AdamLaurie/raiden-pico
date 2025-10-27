# Raiden Pico - Glitching Control Framework

A command-and-control framework for fault injection experiments using ChipShouter or similar EM glitching tools.

## Overview

The Raiden Pico provides a UART-based CLI for precise, hardware-triggered fault injection. It features:

- **Hardware triggering**: GPIO edge detection or UART byte matching
- **Precise timing**: Microsecond-resolution pulse generation using PIO state machines
- **Automated sweeps**: Parameter space exploration for glitch discovery
- **Response capture**: Target device response monitoring
- **External control**: Full automation support via serial commands

## Hardware Setup

### Pin Assignments (Fixed)
- **GP0**: UART TX (CLI output)
- **GP1**: UART RX (CLI input)
- **GP2**: ERROR flag output
- **GP3**: RUNNING flag output
- **GP4**: TRIGGERED flag output
- **GP5**: FINISHED flag output
- **GP6**: CLOCK output (optional target clock)

### User Pins (GP7-29)
- **IN pin**: Trigger input (GPIO or UART)
- **OUT pin**: Glitch pulse output (to ChipShouter trigger or MOSFET)

### Typical Connections
```
Target Device <---> Raiden Pico <---> ChipShouter
      |                |                    |
   TX/GPIO ---------> IN (GP7-29)           |
                      |                     |
                   OUT (GP7-29) -------> TRIGGER IN
                      |
                  FINISHED (GP5) -----> [Oscilloscope/Logic Analyzer]
```

## Basic Workflow

### 1. Configuration
```
# Configure trigger input
IN 10              # Use GP10 as trigger input (rising edge)
IN 10 LOW          # Use GP10 with falling edge trigger

# Configure glitch output
OUT 15             # Use GP15 for glitch pulses

# Set timing parameters (in microseconds @ 1MHz PIO clock)
SET PAUSE 1000     # Delay from trigger to first pulse (1ms)
SET COUNT 1        # Number of pulses to generate
SET WIDTH 50       # Pulse width (50µs)
SET GAP 100        # Gap between pulses (100µs)

# Configure trigger type
TRIGGER GPIO       # Trigger on GPIO edge
# or
HEX 0x41           # Watch for 'A' character
TRIGGER UART       # Trigger on UART byte match

# Verify configuration
GET                # Show all parameters
PINS               # Show pin assignments
```

### 2. Manual Glitching
```
# Arm the system
ARM ON             # Validates configuration and arms for glitching

# Execute single glitch
GLITCH             # System waits for trigger, then fires glitch pulse

# Check status
STATUS             # View all flags and glitch count

# Reset for next attempt
RESET              # Clear flags, ready for next glitch

# Disarm when done
ARM OFF
```

### 3. Automated Parameter Sweeps
```
# Arm system
ARM ON

# Start sweep: SWEEP START <pause_start> <pause_end> <pause_step> <width_start> <width_end> <width_step>
SWEEP START 500 2000 100 10 100 10
# This creates a 2D sweep:
# - PAUSE: 500 to 2000 µs in steps of 100 (16 values)
# - WIDTH: 10 to 100 µs in steps of 10 (10 values)
# - Total: 160 iterations

# Execute each iteration
SWEEP NEXT         # Runs next glitch with updated parameters
                   # Repeat until "SWEEP complete"

# Check progress
SWEEP              # Show current iteration and parameters

# Stop early if needed
SWEEP STOP

# Disarm when done
ARM OFF
```

### 4. Response Capture
```
# After a glitch, capture target's response
RESPONSE CAPTURE 100   # Capture for 100ms

# View captured data (hex format)
RESPONSE               # Shows: RESPONSE: 48656C6C6F... (if "Hello" was sent)

# Clear buffer for next capture
RESPONSE CLEAR
```

## Advanced Usage

### Clock Generation
```
# Generate target clock if needed
CLOCK 12000000     # 12 MHz clock on GP6
CLOCK OFF          # Turn off clock
```

### Conditional Start
```
# Wait for power-on or reset signal before starting
RUN 20             # Wait for GP20 to go HIGH, then start
RUN 20 LOW         # Wait for GP20 to go LOW, then start
```

### Flag Monitoring
All flags are output in real-time on dedicated GPIO pins for scope triggering:
- **RUNNING** (GP3): High when armed and waiting for trigger
- **TRIGGERED** (GP4): Pulses when trigger detected
- **FINISHED** (GP5): Pulses when glitch sequence completes
- **ERROR** (GP2): High if configuration error or glitch failed

The **FLAGS** bitfield combines all flags: `FLAGS = ERROR | (RUNNING<<1) | (TRIGGERED<<2) | (FINISHED<<3)`

## Example: ChipShouter Glitching Campaign

### Scenario
Bypass authentication on a microcontroller by glitching during password comparison.

### Setup
```python
#!/usr/bin/env python3
import serial
import time

# Open serial connection to Raiden Pico
ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)

def send_cmd(cmd):
    ser.write((cmd + '\r\n').encode())
    time.sleep(0.05)
    response = ser.read(ser.in_waiting).decode()
    print(response)
    return response

# Configure system
send_cmd('IN 10')           # Trigger from target's TX line
send_cmd('OUT 15')          # Output to ChipShouter trigger
send_cmd('SET COUNT 1')     # Single pulse per glitch
send_cmd('SET GAP 0')       # Not used for single pulse
send_cmd('HEX 0x50')        # Trigger on 'P' (password prompt)
send_cmd('TRIGGER UART')    # UART trigger mode
send_cmd('ARM ON')          # Arm the system

# Sweep timing parameters
pause_vals = range(100, 5000, 50)    # Delay from trigger: 100-5000µs
width_vals = range(10, 200, 10)      # Pulse width: 10-200µs

for pause in pause_vals:
    for width in width_vals:
        # Set parameters
        send_cmd(f'SET PAUSE {pause}')
        send_cmd(f'SET WIDTH {width}')

        # Reset target (via external GPIO or power cycling)
        # ...

        # Execute glitch
        send_cmd('GLITCH')

        # Wait for glitch to complete (monitor FINISHED flag or poll STATUS)
        time.sleep(0.5)

        # Capture target response
        send_cmd('RESPONSE CAPTURE 200')
        response = send_cmd('RESPONSE')

        # Check if glitch was successful
        if b'BYPASSED' in response.encode():
            print(f"SUCCESS! PAUSE={pause}, WIDTH={width}")
            break

        # Reset for next iteration
        send_cmd('RESET')
        send_cmd('RESPONSE CLEAR')

# Cleanup
send_cmd('ARM OFF')
ser.close()
```

## Timing Considerations

### PIO Clock: 1MHz (1µs resolution)
All timing parameters (PAUSE, WIDTH, GAP) are in microseconds:
- Minimum value: 1 (1µs)
- Maximum value: 4,294,967,295 (≈71 minutes)

### Timing Accuracy
- **Trigger to glitch**: PAUSE parameter (±1µs jitter)
- **Pulse width**: WIDTH parameter (±1µs jitter)
- **ChipShouter delay**: Add ~50-100ns hardware delay

### Calibration
Use an oscilloscope to measure:
1. Trigger event (IN pin)
2. Glitch output (OUT pin)
3. ChipShouter pulse (EM probe)

Adjust PAUSE to compensate for ChipShouter and target delays.

## Troubleshooting

### "ERROR: Cannot ARM"
- Verify OUT, IN, TRIGGER, PAUSE, COUNT, WIDTH are all configured
- Use `GET` to check all parameters

### Glitch doesn't fire
- Check RUNNING flag is HIGH (system is waiting)
- Verify trigger source is connected and active
- For UART trigger: confirm baud rate matches (115200)
- Use `STATUS` to check flags

### Inconsistent results
- Ensure stable power supply to Raiden Pico and target
- Check for EMI interference on trigger lines
- Use shielded cables for critical connections
- Monitor FINISHED flag to confirm glitch completion

### Response capture shows garbage
- Verify target UART baud rate (currently assumes 115200)
- Check target TX is connected to correct pin
- Increase RESPONSE CAPTURE timeout if target is slow

## Safety Considerations

⚠️ **WARNING**: Electromagnetic fault injection can damage hardware!

- **Start conservatively**: Begin with low power settings on ChipShouter
- **Monitor temperature**: Check target device for overheating
- **Limit pulse count**: Use COUNT=1 for initial experiments
- **ESD protection**: Use proper grounding and ESD precautions
- **Legal compliance**: Only test devices you own or have authorization to test

## Performance

- **Trigger latency**: <10µs from edge detection to pulse start
- **Timing jitter**: ±1µs (PIO hardware timing)
- **Max pulse rate**: ~10kHz (if PAUSE+WIDTH+GAP allows)
- **Sweep throughput**: Limited by target reset time (~1-10 glitches/second typical)

## Further Reading

- [ChipShouter User Guide](https://www.newae.com/chipShouter)
- [RP2350 PIO Documentation](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
- [Hardware Fault Injection Tutorial](https://github.com/newaetech/chipwhisperer-tutorials)

## Support

For questions or issues:
- Check STATUS and FLAGS outputs
- Use HELP command for syntax reference
- Monitor flag outputs with oscilloscope for debugging
