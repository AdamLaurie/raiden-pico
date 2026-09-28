---
name: tty-contention-check
description: Before opening the target serial port (/dev/ttyACM* or $RAIDEN_PORT) to flash or drive the Raiden CLI, check that no other process already holds it — the user's own minicom/screen/picocom/cat terminal. Port contention makes reads come back EMPTY (no CLI response) even though the device is fine, which is easily mis-diagnosed as "device dead / firmware hung / SWD broken." Invoke before any pyserial/tty access, and whenever CLI reads come back empty.
---

# Check the tty is free before you use it

Only one process can usefully read a USB-CDC serial port. If the user has a
terminal open on `/dev/ttyACM0` (minicom, screen, picocom, `cat`, a serial
monitor), your pyserial reads will silently return **empty** — the bytes go to
their terminal, not yours. This looks exactly like a dead device, and in this
project it has twice sent debugging down a rabbit hole ("SWD broken", "firmware
hung", "board swapped") when the real cause was a second terminal.

## Do this before opening the port

```bash
port=$(ls /dev/ttyACM* 2>/dev/null | head -1)
fuser "$port" 2>/dev/null    # prints PID(s) holding it; empty = free
```

- **Empty output → free**, proceed.
- **Prints a PID → contended.** STOP. Tell the user which port is held and ask
  them to close their terminal (or `! ` -prefix their command so it runs in this
  session). Do NOT start guessing at the device. `fuser -v "$port"` / `lsof "$port"`
  name the process if you need to show them.

(`fuser` returning nothing also covers the normal case where *you* aren't
holding it either. If a previous script of yours left the port open, that's still
contention — make sure each script `p.close()`s.)

## When reads come back empty mid-session

If `VERSION` (or any command) returns an empty string but the device is powered:
**suspect contention first**, run the `fuser` check, and ask the user before
concluding the device or firmware is broken. A powered, running Raiden always
answers `VERSION`.

## Also (from CLAUDE.md)

- Use `/dev/ttyACM0` (or `$RAIDEN_PORT`); after a flash the CDC re-enumerates and
  the number can change (ACM0↔ACM1) — re-glob `ls /dev/ttyACM*`.
- **Close the tty when finished** (`p.close()` / release it) so you don't become
  the contending process for the next step (e.g. `make flash`, which needs the
  port to send `REBOOT BL`).
