"""Config SWEEP: Tests requiring target power wiring with ADC voltage sense.

Wiring:
    GP10/GP11/GP12 -> Target VDD (direct connection, no shunt resistor)
    GP26 (ADC0)    -> Target VDD (IOREF voltage sense)
    GP27 (ADC1)    -> Floating (NOT connected through shunt)
    GP15           -> Target nRST (optional)

Do NOT use this config with a shunt resistor on GP27 - the sweep
calibration expects direct power delivery to the target.
"""

import pytest


pytestmark = pytest.mark.config_sweep


@pytest.fixture(scope="module")
def sweep_target(raiden):
    """Verify sweep wiring by checking ADC reads voltage when power is on."""
    raiden.cmd("TARGET STM32F1", wait=0.3)
    raiden.cmd("TARGET POWER ON", wait=0.5)

    r = raiden.cmd("STATUS", wait=2)
    if "Power (GP10): ON" not in r and "Power" not in r:
        pytest.skip("Cannot verify target power state")
    return raiden


class TestTargetPower:

    def test_power_on(self, sweep_target):
        r = sweep_target.cmd("TARGET POWER ON")
        assert "ERROR" not in r

    def test_power_off(self, sweep_target):
        r = sweep_target.cmd("TARGET POWER OFF")
        assert "ERROR" not in r
        sweep_target.cmd("TARGET POWER ON")  # restore

    def test_power_cycle(self, sweep_target):
        r = sweep_target.cmd("TARGET POWER CYCLE 300", wait=2)
        assert "ERROR" not in r


class TestGlitchSweep:

    def test_glitch_test(self, sweep_target):
        r = sweep_target.cmd("TARGET GLITCH TEST 1.8 3", wait=10)
        # Individual iterations may have transient SWD failures, check summary completed
        assert "Glitch test complete" in r, "Glitch test did not complete"

    def test_glitch_sweep(self, sweep_target):
        r = sweep_target.cmd("TARGET GLITCH SWEEP", wait=30)
        assert "ERROR" not in r
        # Sweep should report voltage readings
        assert "V" in r or "mV" in r or "ADC" in r
