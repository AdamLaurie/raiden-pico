# Raiden Pico - Glitcher Platform Architecture

## Overview

The Raiden Pico features a modular glitcher platform architecture that allows seamless integration with various fault injection tools. The Pico handles all timing, triggering, and control logic, providing a unified interface regardless of the glitching hardware.

## Supported Platforms

### 1. MANUAL (Default)
Basic trigger pulse output with no platform-specific control.

**Use case**: Custom glitching setups, testing, development

**Configuration**:
```
PLATFORM SET MANUAL
OUT 15                  # Trigger output pin
```

**Hardware**: OUT pin connects directly to your glitching circuit.

---

### 2. CHIPSHOUTER
NewAE ChipShouter electromagnetic fault injection platform.

**Use case**: EM fault injection (EMFI) on microcontrollers, secure elements

**Configuration**:
```
PLATFORM SET CHIPSHOUTER
PLATFORM HVPIN 20        # High voltage enable (to ChipShouter HV_EN)
PLATFORM VPIN 21         # Voltage control (to ChipShouter VOLTAGE_CTRL, 0-5V PWM)
PLATFORM VOLTAGE 250     # Set 250V output (typical range: 100-350V)
PLATFORM CHARGE 5000     # Wait 5ms for capacitor charge
OUT 15                   # Trigger pulse (to ChipShouter TRIGGER_IN)
```

**Hardware Connections**:
```
Raiden Pico GP20 (HV_EN) ----> ChipShouter HV ENABLE
Raiden Pico GP21 (VPIN)  ----> ChipShouter VOLTAGE CTRL (PWM filtered to 0-5V analog)
Raiden Pico GP15 (OUT)   ----> ChipShouter TRIGGER IN
Raiden Pico GND          ----> ChipShouter GND
```

**Voltage Control**:
- GP21 outputs PWM (1kHz, 0-100% duty cycle)
- Add RC low-pass filter: 10kΩ + 10µF → 0-5V analog
- ChipShouter maps 0-5V to 0-500V (100V per volt)
- Example: 250V → 2.5V control → 50% PWM duty

**Behavior**:
- `ARM ON`: Enables HV, sets voltage, waits for charge time
- `GLITCH`: Sends trigger pulse to ChipShouter
- `ARM OFF`: Disables HV, sets voltage to 0V

**Safety Notes**:
- ⚠️ **Start with low voltages** (100-150V) and increase gradually
- Always wait for proper charge time (5-10ms typical)
- Monitor ChipShouter temperature and LED indicators
- Use external interlock switch for emergency HV disable

---

### 3. EMFI
Generic electromagnetic fault injection platform (similar to ChipShouter).

**Use case**: Custom EMFI tools, DIY EM pulse generators

**Configuration**:
```
PLATFORM SET EMFI
PLATFORM HVPIN 20        # Optional: HV enable
PLATFORM VOLTAGE 300     # Platform-dependent voltage setting
OUT 15                   # Trigger pulse
```

**Hardware**: Similar to ChipShouter, but parameters depend on your specific EMFI tool.

---

### 4. CROWBAR
Voltage glitching via power supply crowbar (VCC shorting).

**Use case**: Voltage fault injection, power glitching

**Configuration**:
```
PLATFORM SET CROWBAR
OUT 15                   # MOSFET gate driver
```

**Hardware**:
```
Raiden Pico GP15 (OUT) ---> Gate of N-channel MOSFET (e.g., IRLZ44N)
MOSFET Drain           ---> Target VCC
MOSFET Source          ---> Target GND
```

**Behavior**:
- `GLITCH`: Sends pulse to MOSFET gate, briefly shorting VCC to GND
- Pulse width (WIDTH parameter) controls glitch duration
- **WARNING**: Can permanently damage target! Start with very short pulses (<1µs)

**Circuit**:
```
         Target VCC
              |
              D
    GP15 -G- [MOSFET]
              S
              |
         Target GND

Gate resistor: 100Ω (GP15 → Gate)
Gate-Source resistor: 10kΩ (pull-down)
Optional: Series resistor on Drain (1-10Ω) to limit current
```

**Safety Notes**:
- ⚠️ **EXTREMELY DANGEROUS** - can destroy target device
- Start with WIDTH=1 (1µs) and increase cautiously
- Use current-limited power supply (100mA typical)
- Add series resistance to limit fault current
- Test with sacrificial/duplicate targets first

---

## Platform Workflow

### Basic Setup
```
# 1. Select platform
PLATFORM SET CHIPSHOUTER

# 2. Configure platform parameters
PLATFORM VOLTAGE 200
PLATFORM CHARGE 5000
PLATFORM HVPIN 20
PLATFORM VPIN 21

# 3. Configure trigger and output
IN 10                    # Trigger input
OUT 15                   # Glitch trigger output
TRIGGER GPIO

# 4. Configure timing
SET PAUSE 1000           # Delay from trigger
SET WIDTH 50             # Pulse width
SET COUNT 1              # Single pulse

# 5. Arm system (enables HV, charges caps)
ARM ON

# 6. Execute glitch
GLITCH                   # Waits for trigger, fires glitch

# 7. Disarm when done (disables HV)
ARM OFF
```

### ChipShouter Voltage Sweep
```python
#!/usr/bin/env python3
import serial
import time

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=1)

def send(cmd):
    ser.write((cmd + '\r\n').encode())
    time.sleep(0.05)
    return ser.read(ser.in_waiting).decode()

# Configure ChipShouter
send('PLATFORM SET CHIPSHOUTER')
send('PLATFORM HVPIN 20')
send('PLATFORM VPIN 21')
send('PLATFORM CHARGE 5000')
send('IN 10')
send('OUT 15')
send('TRIGGER GPIO')
send('SET PAUSE 500')
send('SET WIDTH 50')
send('SET COUNT 1')

# Sweep voltage from 150V to 350V
for voltage in range(150, 351, 10):
    print(f"Testing {voltage}V...")

    send(f'PLATFORM VOLTAGE {voltage}')
    send('ARM ON')

    # Reset target (via external control)
    # ...

    send('GLITCH')
    time.sleep(0.5)

    # Check result
    # ...

    send('ARM OFF')
    send('RESET')

ser.close()
```

---

## Adding Custom Platforms

To add support for a new glitching platform:

### 1. Define Platform in Code
Edit `main.py` and add your platform to the `glitcher_platform` options:

```python
glitcher_platform = 'MYPLATFORM'
```

### 2. Implement Platform Functions

Add your platform to `platform_init()`:
```python
elif glitcher_platform == 'MYPLATFORM':
    # Initialize hardware (GPIOs, PWM, etc.)
    if platform_config['enable_pin'] is not None:
        enable = Pin(platform_config['enable_pin'], Pin.OUT)
        platform_config['enable_obj'] = enable
    return True
```

Add to `platform_arm()`:
```python
elif glitcher_platform == 'MYPLATFORM':
    # Enable your platform (power on, charge, etc.)
    if 'enable_obj' in platform_config:
        platform_config['enable_obj'].value(1)
        send("MYPLATFORM: Enabled")
    return True
```

Add to `platform_disarm()`:
```python
elif glitcher_platform == 'MYPLATFORM':
    # Disable your platform
    if 'enable_obj' in platform_config:
        platform_config['enable_obj'].value(0)
        send("MYPLATFORM: Disabled")
    return True
```

Add to `platform_post_glitch()` if needed:
```python
elif glitcher_platform == 'MYPLATFORM':
    # Delay or cleanup after each glitch
    time.sleep(0.001)  # 1ms recharge time
```

### 3. Add Platform Commands
Add custom configuration commands in `process_command()`:

```python
elif parts[1] == 'MYPARAMETER':
    if len(parts) != 3:
        send("ERROR: PLATFORM MYPARAMETER requires value")
        return

    platform_config['my_parameter'] = int(parts[2])
    send(f"OK: My parameter = {platform_config['my_parameter']}")
```

### 4. Update HELP
Add your platform to the help text in the `PLATFORM SET` section.

---

## Platform Parameter Reference

### Common Parameters

| Parameter | Command | Range | Description |
|-----------|---------|-------|-------------|
| Platform | `PLATFORM SET <type>` | MANUAL, CHIPSHOUTER, EMFI, CROWBAR | Select glitcher platform |
| Voltage | `PLATFORM VOLTAGE <v>` | 0-500V | Output voltage (platform-dependent) |
| Charge Time | `PLATFORM CHARGE <us>` | 0-1000000µs | Capacitor charge/wait time |
| HV Enable | `PLATFORM HVPIN <pin>` | GP7-29 | High voltage enable pin |
| Voltage Control | `PLATFORM VPIN <pin>` | GP7-29 | Voltage control PWM output pin |

### ChipShouter Specific

| Voltage (V) | Typical Use | Notes |
|-------------|-------------|-------|
| 100-150 | Testing, low power targets | Safe starting point |
| 150-250 | Most microcontrollers | Common working range |
| 250-350 | Secure elements, hardened targets | High power, use caution |
| 350-500 | Extreme cases | Risk of damage |

**Charge Time**: 5-10ms recommended for full capacitor charge between glitches.

### CROWBAR Specific

| WIDTH (µs) | Effect | Risk |
|------------|--------|------|
| 1-10 | Brief voltage dip | Low risk |
| 10-100 | Moderate glitch | Medium risk |
| 100-1000 | Strong glitch | High risk of damage |
| >1000 | Prolonged short | Very high risk |

**WARNING**: Even 1µs can damage some devices. Always test with current-limited supply first.

---

## Hardware Recommendations

### ChipShouter Interface Circuit
```
Raiden Pico GP20 (HV_EN)
      |
     [1kΩ]
      |
      +----> ChipShouter HV ENABLE (5V logic)

Raiden Pico GP21 (VPIN)
      |
     [10kΩ]
      +----+----> ChipShouter VOLTAGE CTRL
           |
         [10µF]
           |
          GND
```

### CROWBAR MOSFET Circuit
```
Raiden Pico GP15
      |
    [100Ω]
      +---Gate
          |
         [MOSFET IRLZ44N]
          |
    Drain +------[1Ω]------ Target VCC
          |
   Source +---------------- Target GND
          |
        [10kΩ]
          |
         GND
```

### General Guidelines
- Use 3.3V-tolerant interfaces for all Pico connections
- Add ESD protection diodes on all signal lines
- Keep high-power glitch circuits isolated from Pico
- Use optocouplers for HV enable signals when possible
- Provide separate grounds for Pico and glitching hardware (star ground)

---

## Troubleshooting

### ChipShouter not arming
- Check HV_EN pin is connected and configured
- Verify charge time is sufficient (5-10ms)
- Check ChipShouter power supply
- Monitor voltage control signal with multimeter

### Voltage not changing
- Verify VPIN is configured and connected
- Check PWM filter circuit (10kΩ + 10µF)
- Measure control voltage at ChipShouter input
- Confirm `PLATFORM VOLTAGE` command was accepted

### CROWBAR not glitching
- Verify MOSFET is properly connected (Gate, Drain, Source)
- Check gate drive signal with oscilloscope
- Ensure pull-down resistor on gate (10kΩ)
- Confirm target is powered during glitch

### Glitch too weak
- Increase voltage (ChipShouter/EMFI)
- Increase pulse width (WIDTH parameter)
- Decrease delay (PAUSE parameter)
- Position probe closer to target

### Glitch too strong (damage)
- Decrease voltage
- Decrease pulse width
- Use current-limited power supply
- Add series resistance

---

## Safety Checklist

Before glitching:
- [ ] Understand your glitching platform and its risks
- [ ] Start with lowest voltage/width settings
- [ ] Use current-limited power supply for target
- [ ] Test with sacrificial/duplicate hardware first
- [ ] Verify all connections before powering on
- [ ] Have emergency shutoff switch for HV systems
- [ ] Wear ESD protection when handling sensitive hardware
- [ ] Follow local regulations for RF emissions (EMFI)
- [ ] Keep flammable materials away from glitching setup
- [ ] Monitor temperature of all components

---

## Performance

- **Trigger latency**: <10µs (GPIO) or ~20µs (UART)
- **Timing jitter**: ±1µs (PIO hardware timing)
- **Platform arm time**: 5-10ms (ChipShouter charge time)
- **Max glitch rate**: ~100Hz (limited by charge time)
- **PWM voltage control**: 10-bit resolution (0.1% accuracy)

---

## Further Reading

- [ChipShouter Documentation](https://www.newae.com/chipShouter)
- [Voltage Glitching Tutorial](https://wiki.newae.com/Tutorial_A2_Introduction_to_Glitch_Attacks)
- [EM Fault Injection Paper](https://eprint.iacr.org/2019/1214)
- [RP2350 PIO Programming](https://datasheets.raspberrypi.com/rp2350/rp2350-datasheet.pdf)
