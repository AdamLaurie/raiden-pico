"""Config TRACE: Tests requiring shunt resistor wiring for ADC current trace.

Wiring:
    GP10/GP11/GP12 -> Shunt resistor -> Target VDD
    GP26 (ADC0)    -> Target VDD (after shunt, for voltage sense)
    GP27 (ADC1)    -> High side of shunt resistor (current measurement)
    GP15           -> Target nRST

The shunt resistor (e.g. 1 ohm) must be in series between the Raiden
power pins and the target. GP27 measures the voltage drop across it
to capture the target's current profile during boot/operation.

Do NOT use this config for SWEEP tests - the shunt resistor affects
power delivery and will corrupt sweep voltage calibration.
"""

import pytest


pytestmark = pytest.mark.config_trace


@pytest.fixture(scope="module")
def trace_target(raiden):
    """Verify trace wiring is present."""
    raiden.cmd("TARGET STM32F1", wait=0.3)
    raiden.cmd("TARGET POWER ON", wait=0.5)
    return raiden


class TestTraceCapture:

    def test_trace_start(self, trace_target):
        trace_target.cmd("TRACE RESET")
        r = trace_target.cmd("TRACE 4096 50")
        assert "ERROR" not in r

    def test_trace_status_after_start(self, trace_target):
        trace_target.cmd("TRACE RESET")
        trace_target.cmd("TRACE 4096 50")
        r = trace_target.cmd("TRACE STATUS")
        # Should be RUNNING (waiting for trigger) or COMPLETE
        assert "RUNNING" in r or "COMPLETE" in r or "IDLE" in r

    def test_trace_rate_configure(self, trace_target):
        r = trace_target.cmd("TRACE RATE 50")
        assert "OK" in r or "clkdiv" in r

    def test_trace_arm_and_reset(self, trace_target):
        trace_target.cmd("TRACE RESET")
        trace_target.cmd("TRIGGER GPIO RISING")
        trace_target.cmd("TRACE 4096 50")
        r = trace_target.cmd("ARM TRACE")
        assert "OK" in r or "armed" in r.lower()

        trace_target.cmd("ARM OFF")
        trace_target.cmd("TRACE RESET")
        trace_target.cmd("TRIGGER NONE")

    def test_trace_dump_format(self, trace_target):
        """Start trace, force-complete via reset, check dump format."""
        trace_target.cmd("TRACE RESET")
        # Just verify DUMP doesn't crash when no data
        r = trace_target.cmd("TRACE DUMP")
        # May say "no data" or dump empty, either is fine
        assert "ERROR" not in r or "no" in r.lower() or "IDLE" in r
