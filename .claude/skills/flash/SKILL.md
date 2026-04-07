---
name: flash
description: Build and flash firmware to Pico2. Use after code changes to deploy to hardware.
disable-model-invocation: true
allowed-tools: Bash
---

Build and flash firmware to the Pico2:

1. Reset the Pico2 via FTDI DTR, wait for re-enumeration, then flash:
   ```
   python3 -c "import serial, time; s = serial.Serial('/dev/ttyUSB0'); s.dtr = True; time.sleep(0.5); s.dtr = False; s.close()" && sleep 5 && cd /home/addy/work/claude-code/raiden-pico/build && make flash
   ```
2. If cmake cache is stale (pico_sdk_import.cmake error), run:
   ```
   cd /home/addy/work/claude-code/raiden-pico/build && PICO_SDK_PATH=/home/software/unpacked/pico-sdk cmake -DPICO_BOARD=pico2 ..
   ```
   then retry step 1.
3. If flash still fails, ask the user to manually hold BOOTSEL and reset.

All commands must include explicit `cd /home/addy/work/claude-code/raiden-pico/build &&` prefix. Never use `rm -rf` without a full absolute path.
