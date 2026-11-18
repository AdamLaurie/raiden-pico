"""LIVE crowbar-bench tests (TARGET CROWBAR + EXTERNAL supply + FET on GP12).

GATED behind --config=crowbar-wired. This is the ONLY suite that expects a crowbar
MOSFET physically wired to GP12 AND an external supply powering the target rail, and
it **fires the crowbar into the live rail**. Run it only on that bench.

SAFETY (differs from power-ext, which assumes the target is disconnected):
  * The suite stays in EXTERNAL the whole time and NEVER drives INTERNAL — INTERNAL
    would drive GP12 HIGH as a power output, latching the gate FET into a sustained
    short. The autouse fixture re-flags the crowbar (EXTERNAL + gate idled) before
    each test and only ever ARM OFFs after; it does not touch the power mode.
  * It SKIPS itself if the rail isn't powered (ADC0 on GP26), so it can't fire blind.
  * Set the supply's current limit before running — it's the FET backstop if a shot
    ever fails to release (`scripts/psu_usb.py set --volts 3.30 --ilimit 0.100`).

WHAT IT CHECKS (firmware-observable + rail health):
  * crowbar flag forces EXTERNAL and refuses INTERNAL on the live bench
  * a crowbar shot fires (glitch count ++), then soft-disarms
  * VDD (ADC0) stays nominal across firing — i.e. the gate FET fires transiently and
    RELEASES (a stuck-on FET would short the core rail, dragging VDD down via the
    regulator into the PSU's current limit)
  * the target still responds after a crowbar burst (not bricked/latched)

The crowbar's actual VCAP droop depth/width is a SCOPE measurement (CH3 at the FET
drain) — see scripts/crowbar_scope_test.py; this suite only asserts the firmware and
rail-health side that USB can see.
"""

import re
import time

import pytest

pytestmark = pytest.mark.config_crowbar_wired

VDD_OK = 2.5   # volts; below this the rail is considered down/collapsed


def _adc_vdd(raiden):
    """ADC0 (GP26) in volts, or None if it couldn't be read."""
    m = re.search(r"voltage=([\d.]+)\s*V", raiden.cmd("ADC 0", wait=0.5))
    return float(m.group(1)) if m else None


def _glitch_count(raiden):
    m = re.search(r"Glitch Count:\s*(\d+)", raiden.cmd("STATUS", wait=1.5))
    assert m, "STATUS missing 'Glitch Count:'"
    return int(m.group(1))


@pytest.fixture(autouse=True)
def _crowbar_safe(raiden):
    """Crowbar-safe bench state around every test: flagged EXTERNAL (gate idled
    de-asserted), disarmed, no trigger. NEVER drives INTERNAL."""
    raiden.cmd("TARGET CROWBAR ON AHIGH PSU EXT", wait=0.5)  # EXTERNAL + idle gate
    raiden.cmd("TRIGGER NONE", wait=0.3)
    raiden.cmd("SET COUNT 1", wait=0.3)
    yield
    raiden.cmd("ARM OFF", wait=0.3)
    raiden.cmd("SET WIDTH 100", wait=0.3)
    # Leave EXTERNAL + gate idled (flag stays ON). Deliberately NO TARGET POWER INT.


@pytest.fixture
def powered(raiden):
    """Skip the test unless the external rail is actually up (wiring/supply check)."""
    v = _adc_vdd(raiden)
    if v is None or v < VDD_OK:
        pytest.skip(f"rail not powered (ADC0={v} V) — enable the supply "
                    f"(scripts/psu_usb.py on) before the crowbar suite")
    return v


class TestCrowbarWired:

    def test_flag_forces_external_and_refuses_internal(self, raiden):
        assert "Power mode: EXTERNAL" in raiden.cmd("TARGET POWER")
        r = raiden.cmd("TARGET POWER INT")
        assert "ERROR" in r and "crowbar bench" in r.lower()

    def test_supply_is_present(self, raiden, powered):
        assert powered >= VDD_OK   # fixture already skipped if not

    def test_crowbar_fires_and_fet_releases(self, raiden, powered):
        # A narrow single shot; VDD must stay nominal (FET fires transiently and
        # releases — a stuck-on gate would short the core rail and sag VDD).
        raiden.cmd("SET WIDTH 15")      # ~100 ns
        before = _glitch_count(raiden)
        assert "armed" in raiden.cmd("ARM ON").lower()
        assert "Glitch executed" in raiden.cmd("GLITCH")
        assert _glitch_count(raiden) == before + 1
        assert "DISARMED" in raiden.cmd("ARM")          # soft-disarm in EXTERNAL
        time.sleep(0.2)
        v = _adc_vdd(raiden)
        assert v is not None and v >= VDD_OK, \
            f"VDD sagged to {v} V after a crowbar shot — gate FET stuck on?"

    def test_target_survives_crowbar_burst(self, raiden, powered):
        raiden.cmd("SET WIDTH 15")
        for _ in range(10):
            raiden.cmd("ARM ON", wait=0.2)
            raiden.cmd("GLITCH", wait=0.2)
        v = _adc_vdd(raiden)
        assert v is not None and v >= VDD_OK, \
            f"VDD collapsed to {v} V after a 10-shot crowbar burst"

    def test_bootloader_survives_crowbar(self, raiden, powered):
        # End-to-end: the crowbar perturbs a live target but doesn't brick it.
        raiden.cmd("TARGET STM32F4", wait=0.3)
        r = raiden.cmd("TARGET SYNC", wait=6)
        if "Bootloader mode active" not in r and "ACK received" not in r:
            pytest.skip("bootloader not syncing on GP4/GP5 — wiring/target")
        raiden.cmd("SET WIDTH 15")
        raiden.cmd("ARM ON", wait=0.3)
        raiden.cmd("GLITCH", wait=0.3)                   # fire into the live rail
        r2 = raiden.cmd("TARGET SYNC", wait=6)           # must re-enter (not bricked)
        assert "Bootloader mode active" in r2 or "ACK received" in r2, \
            "target not responding after a crowbar shot — bricked/latched?"
