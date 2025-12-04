the target is a pico2 not a pico so ensure all code is for the RP2350

use /dev/ttyACM0 to do your own debugging

always close the tty when finished and never open it unless it is for flashing or testing

use the command "REBOOT BL" to put the pico2 into bootloader mode for flashing new code

the SDK path is PICO_SDK_PATH=/home/software/unpacked/pico-sdk

you have full control of the pico2 and can flash it and test it under your own power. make sure everything works before notifying me. do not ask me for confirmation unless you need verification with external tools like oscilloscope.

## UART Switching (Target vs Grbl)

The system uses UART1 for both Target (GP4/GP5) and Grbl XY platform (GP8/GP9 via alternate function).
Only one can be active at a time - switching between them requires reconfiguration.

**Important Limitation:**
- Switching from Target to Grbl (or vice versa) breaks the active UART connection
- After using GRBL commands, TARGET SYNC must be run again before TARGET SEND commands will work
- This is a hardware limitation - UART1 can only be on one set of pins at a time

**Workflow:**
1. TARGET SYNC → TARGET SEND commands (works)
2. GRBL commands (auto-switches, breaks target connection)
3. TARGET SYNC → TARGET SEND commands (re-establishes connection, works)

**Auto-switching behavior:**
- GRBL commands automatically initialize Grbl UART (GP8/GP9) if not active
- TARGET commands automatically initialize Target UART (GP4/GP5) if not active
- Switching happens transparently but breaks the other connection
- TRIGGER UART functionality continues to work correctly after auto-switching
