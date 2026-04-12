"""Config NONE: Tests requiring no external hardware - just the Pico2 over USB.

Wiring: None. Only USB connection to Raiden Pico required.
"""

import pytest


# ── Help / Version / Status ──────────────────────────────────

class TestSystemInfo:

    def test_help_has_sections(self, raiden):
        """Check HELP output contains key sections.

        Note: HELP output is ~4KB which can overflow USB CDC TX buffer.
        We check sections reliably received in the first ~3KB.
        """
        r = raiden.cmd("HELP", wait=5)
        for section in ["ChipSHOUTER", "Glitch", "Target", "Trigger", "Trace", "SWD"]:
            assert section in r, f"HELP missing section: {section}"

    def test_version(self, raiden):
        r = raiden.cmd("VERSION")
        assert "Raiden Pico" in r

    def test_status_fields(self, raiden):
        r = raiden.cmd("STATUS", wait=2)
        for field in ["RP2350", "Armed", "Pause", "Width", "Gap", "Count", "Trigger", "Target"]:
            assert field in r, f"STATUS missing field: {field}"

    def test_pins_output(self, raiden):
        r = raiden.cmd("PINS", wait=2)
        for pin in ["GP2", "GP7", "GP15", "GP17", "GP18", "GP25"]:
            assert pin in r, f"PINS missing: {pin}"
        assert "Glitch Output (normal)" in r
        assert "Glitch Output (inverted)" in r

    def test_unknown_command(self, raiden):
        r = raiden.cmd("FOOBAR")
        assert "ERROR" in r
        assert "Unknown" in r or "HELP" in r


# ── SET / GET parameters ─────────────────────────────────────

class TestGlitchParams:

    def test_set_get_pause(self, raiden):
        raiden.cmd("SET PAUSE 1000")
        r = raiden.cmd("GET PAUSE")
        assert "1000" in r

    def test_set_get_width(self, raiden):
        raiden.cmd("SET WIDTH 150")
        r = raiden.cmd("GET WIDTH")
        assert "150" in r

    def test_set_get_gap(self, raiden):
        raiden.cmd("SET GAP 200")
        r = raiden.cmd("GET GAP")
        assert "200" in r

    def test_set_get_count(self, raiden):
        raiden.cmd("SET COUNT 5")
        r = raiden.cmd("GET COUNT")
        assert "5" in r

    def test_get_all(self, raiden):
        r = raiden.cmd("GET")
        for param in ["PAUSE", "WIDTH", "GAP", "COUNT"]:
            assert param in r, f"GET missing: {param}"

    def test_set_all(self, raiden):
        r = raiden.cmd("SET")
        for param in ["PAUSE", "WIDTH", "GAP", "COUNT"]:
            assert param in r, f"SET missing: {param}"

    def test_set_unknown_param(self, raiden):
        r = raiden.cmd("SET FOOBAR 100")
        assert "ERROR" in r

    def test_get_unknown_param(self, raiden):
        r = raiden.cmd("GET FOOBAR")
        assert "ERROR" in r


# ── Trigger configuration ────────────────────────────────────

class TestTrigger:

    def test_trigger_none(self, raiden):
        r = raiden.cmd("TRIGGER NONE")
        assert "OK" in r or "disabled" in r.lower()

    def test_trigger_show(self, raiden):
        raiden.cmd("TRIGGER NONE")
        r = raiden.cmd("TRIGGER")
        assert "NONE" in r

    def test_trigger_gpio_rising(self, raiden):
        raiden.cmd("TRIGGER GPIO RISING")
        r = raiden.cmd("TRIGGER")
        assert "GPIO" in r
        assert "RISING" in r

    def test_trigger_gpio_falling(self, raiden):
        raiden.cmd("TRIGGER GPIO FALLING")
        r = raiden.cmd("TRIGGER")
        assert "GPIO" in r
        assert "FALLING" in r

    def test_trigger_uart_byte(self, raiden):
        raiden.cmd("TRIGGER UART 3F")
        r = raiden.cmd("TRIGGER")
        assert "UART" in r
        assert "3F" in r.upper()

    def test_trigger_uart_tx(self, raiden):
        raiden.cmd("TRIGGER UART 0D TX")
        r = raiden.cmd("TRIGGER")
        assert "TX" in r

    def test_trigger_uart_rx(self, raiden):
        raiden.cmd("TRIGGER UART 0A RX")
        r = raiden.cmd("TRIGGER")
        assert "RX" in r

    def test_trigger_cleanup(self, raiden):
        raiden.cmd("TRIGGER NONE")


# ── Arm / Disarm ─────────────────────────────────────────────

class TestArm:

    def test_arm_disarm_cycle(self, raiden):
        raiden.cmd("TRIGGER GPIO RISING")  # Need trigger configured to arm

        r = raiden.cmd("ARM ON")
        assert "Armed" in r or "OK" in r

        r = raiden.cmd("ARM")
        assert "ARMED" in r.upper()

        r = raiden.cmd("ARM OFF")
        assert "Disarmed" in r or "OK" in r

        raiden.cmd("TRIGGER NONE")

    def test_arm_off_when_disarmed(self, raiden):
        r = raiden.cmd("ARM OFF")
        # Should not error
        assert "ERROR" not in r

    def test_arm_trace_mode(self, raiden):
        raiden.cmd("TRIGGER GPIO RISING")
        r = raiden.cmd("ARM TRACE")
        assert "OK" in r or "armed" in r.lower() or "Trace" in r
        raiden.cmd("ARM OFF")
        raiden.cmd("TRIGGER NONE")

    def test_arm_bad_arg(self, raiden):
        r = raiden.cmd("ARM FOOBAR")
        assert "ERROR" in r


# ── Clock generator ──────────────────────────────────────────

class TestClock:

    def test_clock_set_and_on(self, raiden):
        r = raiden.cmd("CLOCK 12000000 ON")
        assert "12" in r or "OK" in r

    def test_clock_off(self, raiden):
        r = raiden.cmd("CLOCK OFF")
        assert "OK" in r or "OFF" in r or "stopped" in r.lower()

    def test_clock_query(self, raiden):
        raiden.cmd("CLOCK 8000000 ON")
        r = raiden.cmd("CLOCK")
        assert "8000000" in r or "8.0" in r or "8MHz" in r
        raiden.cmd("CLOCK OFF")

    def test_clock_invalid(self, raiden):
        r = raiden.cmd("CLOCK FOOBAR")
        assert "ERROR" in r


# ── Debug mode ───────────────────────────────────────────────

class TestDebug:

    def test_debug_on_off(self, raiden):
        r = raiden.cmd("DEBUG ON")
        assert "ON" in r

        r = raiden.cmd("DEBUG OFF")
        assert "OFF" in r

    def test_debug_query(self, raiden):
        raiden.cmd("DEBUG OFF")
        r = raiden.cmd("DEBUG")
        assert "OFF" in r


# ── Target type ──────────────────────────────────────────────

class TestTargetType:

    @pytest.mark.parametrize("target", ["LPC", "STM32F1", "STM32F3", "STM32F4", "STM32L4"])
    def test_set_target_type(self, raiden, target):
        r = raiden.cmd(f"TARGET {target}")
        assert "OK" in r or target in r

    def test_target_type_in_status(self, raiden):
        raiden.cmd("TARGET STM32F1")
        r = raiden.cmd("STATUS", wait=2)
        assert "STM32F1" in r

    def test_target_timeout_roundtrip(self, raiden):
        raiden.cmd("TARGET TIMEOUT 100")
        r = raiden.cmd("TARGET TIMEOUT")
        assert "100" in r

    def test_target_reset_no_target(self, raiden):
        """TARGET RESET toggles GP15, safe without target."""
        r = raiden.cmd("TARGET RESET")
        assert "ERROR" not in r

    def test_target_power_toggle(self, raiden):
        r = raiden.cmd("TARGET POWER ON")
        assert "ERROR" not in r

        r = raiden.cmd("TARGET POWER OFF")
        assert "ERROR" not in r

        raiden.cmd("TARGET POWER ON")  # restore

    def test_target_power_cycle(self, raiden):
        r = raiden.cmd("TARGET POWER CYCLE 100", wait=1.5)
        assert "ERROR" not in r

    def test_target_bad_subcommand(self, raiden):
        r = raiden.cmd("TARGET FOOBAR")
        assert "ERROR" in r


# ── Glitch execution ─────────────────────────────────────────

class TestGlitch:

    def test_glitch_execute(self, raiden):
        r = raiden.cmd("GLITCH")
        # May fail (no trigger) or succeed, but should not crash
        assert "Glitch" in r or "ERROR" in r

    def test_reset_command(self, raiden):
        r = raiden.cmd("RESET")
        assert "ERROR" not in r


# ── API mode ─────────────────────────────────────────────────

class TestAPIMode:

    def test_api_on_off(self, raiden):
        r = raiden.cmd("API ON")
        assert "ON" in r or "+" in r

        r = raiden.cmd("API OFF")
        # After API OFF, response is in normal mode
        assert "ERROR" not in r

    def test_api_protocol(self, raiden):
        raiden.cmd("API ON")
        r = raiden.cmd("VERSION")
        assert "+" in r  # success indicator
        raiden.cmd("API OFF")

    def test_api_error_protocol(self, raiden):
        raiden.cmd("API ON")
        r = raiden.cmd("FOOBAR")
        assert "!" in r  # error indicator
        raiden.cmd("API OFF")


# ── Trace state management ───────────────────────────────────

class TestTrace:

    def test_trace_status_idle(self, raiden):
        raiden.cmd("TRACE RESET")
        r = raiden.cmd("TRACE STATUS")
        assert "IDLE" in r

    def test_trace_no_args_is_status(self, raiden):
        raiden.cmd("TRACE RESET")
        r = raiden.cmd("TRACE")
        assert "IDLE" in r or "Trace" in r

    def test_trace_rate_set(self, raiden):
        r = raiden.cmd("TRACE RATE 50")
        assert "OK" in r or "clkdiv" in r

    def test_trace_reset(self, raiden):
        r = raiden.cmd("TRACE RESET")
        assert "ERROR" not in r


# ── Breakpoint management ────────────────────────────────────

class TestBreakpoints:

    def test_bp_list_empty(self, raiden):
        raiden.cmd("SET BP CLEAR ALL")
        r = raiden.cmd("SET BP LIST")
        assert "Breakpoints" in r or "none" in r.lower()

    def test_bp_clear_all(self, raiden):
        r = raiden.cmd("SET BP CLEAR ALL")
        assert "ERROR" not in r


# ── SWD speed ────────────────────────────────────────────────

class TestSWDSpeed:

    def test_swd_speed_query(self, raiden):
        r = raiden.cmd("SWD SPEED")
        assert "delay" in r.lower() or "kHz" in r or "speed" in r.lower()

    def test_swd_speed_set_and_read(self, raiden):
        raiden.cmd("SWD SPEED 2")
        r = raiden.cmd("SWD SPEED")
        assert "2" in r

        # Restore default
        raiden.cmd("SWD SPEED 1")


# ── Prefix matching ──────────────────────────────────────────

class TestPrefixMatching:

    def test_stat_matches_status(self, raiden):
        r = raiden.cmd("STAT")
        assert "System Status" in r or "RP2350" in r

    def test_ver_matches_version(self, raiden):
        r = raiden.cmd("VER")
        assert "Raiden Pico" in r

    def test_pin_matches_pins(self, raiden):
        r = raiden.cmd("PIN")
        assert "Pin Configuration" in r or "GP" in r
