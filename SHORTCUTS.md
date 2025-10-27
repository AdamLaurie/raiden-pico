# Command Shortcuts

## Overview

Raiden Pico supports **non-ambiguous command shortcuts** for both primary commands and sub-commands. This allows you to type less while maintaining clarity and avoiding errors.

## How It Works

### Prefix Matching

The shortcut system uses prefix matching for:
- **Primary commands** (e.g., STATUS, GLITCH, PLATFORM)
- **Sub-commands** (e.g., PLATFORM SET, CHIPSHOT ARM, TARGET INIT)
- **Arguments** (e.g., trigger types, platform types, variable names)

Matching rules:
- You can abbreviate by typing just the beginning
- The system matches your input against all valid options
- If **exactly one** option matches your prefix, it's accepted
- If **multiple** options match (ambiguous), an error is shown
- If **no** options match, the normal error handling applies

### Examples

**Primary Commands:**
```
STAT      → STATUS
GL        → GLITCH
PLAT      → PLATFORM
CHIP      → CHIPSHOT
TARG      → TARGET
SW        → SWEEP
RESP      → RESPONSE
RES       → RESET
ARM       → ARM (already short!)
```

**Sub-Commands:**
```
PLAT SET       → PLATFORM SET
PLAT V         → PLATFORM VOLTAGE
PLAT C         → PLATFORM CHARGE
PLAT H         → PLATFORM HVPIN
PLAT VP        → PLATFORM VPIN

CHIP V         → CHIPSHOT VOLTAGE
CHIP A         → CHIPSHOT ARM
CHIP D         → CHIPSHOT DISARM
CHIP R         → CHIPSHOT RESET
CHIP U         → CHIPSHOT UARTPINS
CHIP C         → CHIPSHOT CMD
CHIP S         → CHIPSHOT SYNC

TARG I         → TARGET INIT
TARG B         → TARGET BAUD
TARG P         → TARGET PINS
TARG S         → TARGET SEND
TARG REA       → TARGET READ
TARG READL     → TARGET READLINE
TARG C         → TARGET CMD
TARG F         → TARGET FLUSH
TARG RES       → TARGET RESET

SW STA         → SWEEP START
SW N           → SWEEP NEXT
SW STO         → SWEEP STOP

RESP CAP       → RESPONSE CAPTURE
RESP CL        → RESPONSE CLEAR
```

**Arguments:**
```
# Trigger types
TRIG N         → TRIGGER NONE
TRIG U         → TRIGGER UART
TRIG G         → TRIGGER GPIO

# Platform types
PLAT SET M     → PLATFORM SET MANUAL
PLAT SET C     → PLATFORM SET CHIPSHOUTER
PLAT SET E     → PLATFORM SET EMFI
PLAT SET CR    → PLATFORM SET CROWBAR

# ARM states
ARM O          → ARM ON (ambiguous with OFF!)
              → Use "ARM ON" or "ARM OF" for OFF

# Variable names
SET P 1000     → SET PAUSE 1000
SET C 1        → SET COUNT 1
SET W 50       → SET WIDTH 50
SET G 100      → SET GAP 100

GET P          → GET PAUSE
GET C          → GET COUNT
GET W          → GET WIDTH
GET G          → GET GAP
```

## Ambiguous Commands

Some abbreviations are **ambiguous** and will result in an error:

### Primary Command Conflicts

| Abbreviation | Matches | Error |
|--------------|---------|-------|
| `S` | SET, STATUS, SWEEP | Ambiguous |
| `R` | RUN, RESET, RESPONSE | Ambiguous |
| `C` | CLOCK | Not ambiguous (only match) |
| `G` | GET, GLITCH | Ambiguous |

### Sub-Command Conflicts

**PLATFORM:**
- `V` is NOT ambiguous → matches `VOLTAGE` only (VPIN requires `VP`)
- `S` is NOT ambiguous → matches `SET` only

**CHIPSHOT:**
- `A` is NOT ambiguous → matches `ARM` only
- `D` is NOT ambiguous → matches `DISARM` only
- `R` is NOT ambiguous → matches `RESET` only

**TARGET:**
- `R` is NOT ambiguous → matches `READ` only (RESET and READLINE need more letters)
- `RE` is ambiguous → matches `READ`, `READLINE`, `RESET`
- `REA` matches `READ` and `READLINE` → ambiguous
- `READL` matches `READLINE` only → not ambiguous
- `RES` matches `RESET` only → not ambiguous

### Argument Conflicts

**Trigger Types:**
- `N` is NOT ambiguous → matches `NONE` only
- `U` is NOT ambiguous → matches `UART` only
- `G` is NOT ambiguous → matches `GPIO` only

**Platform Types:**
- `M` is NOT ambiguous → matches `MANUAL` only
- `C` is ambiguous → matches `CHIPSHOUTER` and `CROWBAR`
- `CH` is NOT ambiguous → matches `CHIPSHOUTER` only
- `CR` is NOT ambiguous → matches `CROWBAR` only
- `E` is NOT ambiguous → matches `EMFI` only

**ARM States:**
- `O` is ambiguous → matches `ON` and `OFF`
- `ON` is NOT ambiguous → matches `ON` only
- `OF` is NOT ambiguous → matches `OFF` only

**Variable Names:**
- `P` is NOT ambiguous → matches `PAUSE` only
- `C` is NOT ambiguous → matches `COUNT` only
- `W` is NOT ambiguous → matches `WIDTH` only
- `G` is NOT ambiguous → matches `GAP` only

## Full Command Examples

### Basic Workflow (With Argument Shortcuts)
```
PLAT SET CH            # Set platform to ChipShouter
PLAT H 20              # Set HV enable pin
PLAT VP 21             # Set voltage pin
PLAT V 250             # Set voltage to 250V
PLAT C 5000            # Set charge time to 5ms
IN 10                  # Trigger input on GP10
OUT 15                 # Output on GP15
TRIG G                 # GPIO trigger
SET P 1000             # Pause 1000µs
SET W 50               # Width 50µs
SET C 1                # Count 1
ARM ON                 # Arm system
GL                     # Execute glitch
STAT                   # Check status
ARM OF                 # Disarm
```

### Target UART Communication (Short)
```
TARG PINS 16 17        # TX=GP16, RX=GP17
TARG B 115200          # 115200 baud
TARG I                 # Initialize UART
TARG S hello           # Send "hello"
TARG REA 500           # Read with 500ms timeout
TARG READL 1000        # Read line with 1000ms timeout
TARG F                 # Flush RX buffer
```

### ChipShouter Control (Short)
```
CHIP                   # Query status
CHIP V                 # Query voltage
CHIP V 300             # Set voltage to 300V
CHIP A                 # Arm ChipShouter
CHIP D                 # Disarm ChipShouter
CHIP S                 # Sync settings
CHIP C version         # Raw command: version
```

### Parameter Sweep (Short)
```
SW STA 500 2000 100 10 100 10   # Start sweep
SW N                             # Next iteration
SW N                             # Next iteration
SW STO                           # Stop sweep
```

## Full vs. Short Command Comparison

| Full Command | Shortest Non-Ambiguous | Characters Saved |
|--------------|------------------------|------------------|
| `PLATFORM SET CHIPSHOUTER` | `PLAT SET CHIP` | 14 |
| `CHIPSHOT VOLTAGE 250` | `CHIP V 250` | 11 |
| `TARGET INIT` | `TARG I` | 7 |
| `TARGET READLINE 1000` | `TARG READL 1000` | 4 |
| `SWEEP START` | `SW STA` | 6 |
| `RESPONSE CAPTURE 100` | `RESP CAP 100` | 8 |
| `STATUS` | `STAT` | 2 |
| `GLITCH` | `GL` | 4 |

## Error Handling

### Ambiguous Primary Command
```
> S
ERROR: Ambiguous command 'S' - be more specific
```

**Solution:** Use at least `SE` for SET, `STAT` for STATUS, or `SW` for SWEEP.

### Ambiguous Sub-Command
```
> TARG RE
ERROR: Ambiguous TARGET sub-command 'RE' - be more specific
```

**Solution:** Use `REA` for READ, `READL` for READLINE, or `RES` for RESET.

### No Match
```
> FOOBAR
ERROR: Unknown command. Type HELP for available commands.
```

**Solution:** Check spelling or type `HELP` to see available commands.

## Best Practices

### Interactive Use
- **Use shortcuts liberally** during interactive testing and development
- Common shortcuts like `STAT`, `GL`, `RES` save time during rapid iteration
- Platform-specific shortcuts like `CHIP V` or `TARG I` reduce typing fatigue

### Script/Automation Use
- **Prefer full commands** in scripts for maximum readability
- Shortcuts in scripts can obscure intent for future readers
- Exception: Very common shortcuts (`STAT`, `GL`) are acceptable in well-documented scripts

### Finding the Right Abbreviation
1. Start with the first letter
2. If ambiguous, add one letter at a time until unique
3. Common patterns:
   - 2-3 letters for primary commands (e.g., `PLAT`, `CHIP`, `TARG`)
   - 1-3 letters for sub-commands (e.g., `V`, `ARM`, `READL`)

## Implementation Details

### Algorithm
```python
def match_command(abbrev, candidates):
    matches = [cmd for cmd in candidates if cmd.startswith(abbrev)]

    if len(matches) == 1:
        return matches[0]  # Unique match
    elif len(matches) > 1:
        return None  # Ambiguous
    else:
        return abbrev  # No match (let normal error handling work)
```

### Performance
- Matching is performed in O(n) where n = number of valid commands
- Primary commands: 19 options
- Sub-commands: 5-9 options depending on command
- Total overhead: <1ms per command

### Case Insensitivity
- All commands are converted to uppercase before matching
- `stat`, `Stat`, `STAT` all match `STATUS`

## Compatibility

This feature is **backward compatible**:
- Full commands continue to work exactly as before
- Old scripts using full commands will not break
- Shortcuts are purely additive functionality

## Testing Shortcuts

To test shortcut behavior on your Raiden Pico:

```
# Test primary command matching
> STAT
(Should show status)

> GL
ERROR: System not ARMED (use ARM ON first)
(Correctly recognized GLITCH)

> S
ERROR: Ambiguous command 'S' - be more specific
(Correctly detected ambiguity)

# Test sub-command matching
> PLAT SET MANUAL
OK: PLATFORM = MANUAL
(Full command works)

> PLAT SET MAN
OK: PLATFORM = MANUAL
(Abbreviated platform type works)

> TARG I
OK: Target UART initialized...
(Shortest TARGET sub-command works)

> CHIP V
ChipShouter Voltage: 250V
(Shortest CHIPSHOT sub-command works)

# Test ambiguity detection
> TARG RE
ERROR: Ambiguous TARGET sub-command 'RE' - be more specific
(Correctly detected READ/READLINE/RESET ambiguity)

> TARG REA
ERROR: Ambiguous TARGET sub-command 'REA' - be more specific
(Correctly detected READ/READLINE ambiguity)

> TARG READL
TARGET RX: (timeout)
(Correctly matched READLINE)
```

## Future Enhancements

Potential improvements for future versions:
- **Tab completion**: Press TAB to auto-complete commands
- **History**: Up/down arrows to recall previous commands
- **Aliases**: User-defined custom shortcuts (e.g., `G` → `GLITCH`)
- **Fuzzy matching**: Suggest corrections for typos

## Related Documentation

- [Main README](README.md) - Overall system documentation
- [TARGET_UART.md](TARGET_UART.md) - Target UART feature details
- [CHIPSHOT_UART.md](CHIPSHOT_UART.md) - ChipShouter UART control
- [PLATFORM_GUIDE.md](PLATFORM_GUIDE.md) - Platform abstraction
- [PIO_ARCHITECTURE.md](PIO_ARCHITECTURE.md) - PIO state machine design
