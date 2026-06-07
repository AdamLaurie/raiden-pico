"""Shared fixtures and helpers for Raiden Pico hardware-in-the-loop tests.

Wiring configs are gated by --config flags. Without flags, only config_none
tests run (USB-only, no external wiring).

Usage:
    pytest tests/ -v                        # config_none only
    pytest tests/ -v --config=swd           # + SWD target tests
    pytest tests/ -v --config=sweep         # + power sweep tests
    pytest tests/ -v --config=trace         # + shunt/ADC trace tests
    pytest tests/ -v --config=swd-rdp       # + destructive RDP tests
    pytest tests/ -v --config=power-int     # + INTERNAL-mode power tests
    pytest tests/ -v --config=power-ext     # + EXTERNAL/crowbar power tests

SAFETY — power-int / power-ext: these drive the GP10/11/12 power group
(power on/off/cycle, mode switching, and the crowbar gate fires in EXTERNAL).
Run them ONLY with the bench wired for the matching mode, or the target
disconnected: on the wrong wiring they can damage a connected target or short
GP10 against GP11/12 (the ganged-harness gotcha) and report false failures.
That's why they never run under the default config_none. Dual-mode tests carry
BOTH markers and require both flags.
"""

import os
import time
import pytest
import serial


RAIDEN_PORT = os.environ.get("RAIDEN_PORT", "/dev/ttyACM0")
RAIDEN_BAUD = 115200

# Marker-to-flag mapping. config_none is always enabled.
_CONFIG_MARKERS = {
    "config_swd": "swd",
    "config_sweep": "sweep",
    "config_trace": "trace",
    "config_swd_rdp": "swd-rdp",
    "config_power_int": "power-int",
    "config_power_ext": "power-ext",
}


def pytest_addoption(parser):
    parser.addoption(
        "--config",
        action="append",
        default=[],
        help="Enable a wiring config: swd, sweep, trace, swd-rdp, power-int, power-ext. Repeat for multiple.",
    )
    parser.addoption(
        "--destructive",
        action="store_true",
        default=False,
        help="Allow destructive tests (flash erase/write, RDP changes).",
    )


def pytest_collection_modifyitems(config, items):
    enabled = set(config.getoption("config"))
    allow_destructive = config.getoption("destructive")

    for item in items:
        markers = {m.name for m in item.iter_markers()}

        # Check wiring config gate
        for marker, flag in _CONFIG_MARKERS.items():
            if marker in markers and flag not in enabled:
                item.add_marker(pytest.mark.skip(
                    reason=f"Needs --config={flag} (confirm wiring first)"
                ))
                break

        # Check destructive gate (separate from config gate)
        if "destructive" in markers and not allow_destructive:
            item.add_marker(pytest.mark.skip(
                reason="Destructive test: needs --destructive flag"
            ))


class RaidenClient:
    """Serial communication helper for Raiden Pico CLI."""

    def __init__(self, port, baud=RAIDEN_BAUD):
        self.port = port
        self.baud = baud
        self.ser = serial.Serial(port, baud, timeout=3)
        time.sleep(1)
        self.ser.reset_input_buffer()

    def reboot(self):
        """Soft-reboot the device and reconnect. Writes REBOOT WITHOUT draining a
        reply — the CDC drops mid-command, so reading in_waiting would raise an
        I/O error — then re-opens the port via reconnect()."""
        try:
            self.ser.reset_input_buffer()
            self.ser.write(b"REBOOT\r\n")
            self.ser.flush()
        except Exception:
            pass
        self.reconnect()

    def reconnect(self, settle=2.0, timeout=25):
        """Close and reopen the serial port after a REBOOT (the USB CDC drops and
        re-enumerates). Waits for the port to come back and the CLI to respond, so
        tests can check the power-on-reset state. Raises if it never reappears."""
        try:
            if self.ser and self.ser.is_open:
                self.ser.close()
        except Exception:
            pass
        time.sleep(settle)
        deadline = time.time() + timeout
        while time.time() < deadline:
            if os.path.exists(self.port):
                try:
                    self.ser = serial.Serial(self.port, self.baud, timeout=3)
                    time.sleep(1)
                    self.ser.reset_input_buffer()
                    if "Raiden" in self.cmd("VERSION", wait=0.5):
                        return
                    self.ser.close()
                except serial.SerialException:
                    pass
            time.sleep(0.5)
        raise RuntimeError(f"{self.port} did not come back / respond after REBOOT")

    def cmd(self, command, wait=1.0):
        """Send command, return response text."""
        self.ser.reset_input_buffer()
        self.ser.write(f"{command}\r\n".encode())
        print(f"\n>>> {command}")
        time.sleep(wait)
        data = b""
        # Drain all available data — keep reading until 0.5s of silence
        empty_count = 0
        while empty_count < 5:
            n = self.ser.in_waiting
            if n:
                data += self.ser.read(n)
                empty_count = 0
            else:
                empty_count += 1
            time.sleep(0.1)
        resp = data.decode("utf-8", errors="replace").strip()
        for line in resp.splitlines():
            print(f"  {line}")
        return resp

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()


@pytest.fixture(scope="session")
def raiden():
    """Session-scoped Raiden CLI client. Skips all tests if no hardware."""
    if not os.path.exists(RAIDEN_PORT):
        pytest.skip(f"No hardware: {RAIDEN_PORT} not found")

    try:
        client = RaidenClient(RAIDEN_PORT)
    except serial.SerialException as e:
        pytest.skip(f"Cannot open {RAIDEN_PORT}: {e}")

    r = client.cmd("VERSION", wait=0.5)
    if "Raiden" not in r:
        client.close()
        pytest.skip("Device not responding to VERSION command")

    # Ensure clean state
    client.cmd("ARM OFF", wait=0.3)
    client.cmd("API OFF", wait=0.3)
    client.cmd("TRIGGER NONE", wait=0.3)

    yield client
    client.close()


@pytest.fixture(scope="module")
def swd_target(raiden):
    """Ensure SWD target is connected. Skip module if not."""
    raiden.cmd("TARGET STM32F1", wait=0.3)
    r = raiden.cmd("SWD CONNECT")
    if "Connected" not in r:
        pytest.skip("No SWD target connected")
    raiden.cmd("SWD HALT", wait=0.5)
    return raiden
