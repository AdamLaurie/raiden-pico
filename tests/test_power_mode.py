"""Config NONE: TARGET POWER MODE (INTERNAL / EXTERNAL crowbar) regression tests.

Wiring: None. Only the USB connection to Raiden Pico is required — every test
asserts on CLI text responses, so no scope/target is needed.

Covers the feat/crowbar-power-mode additions:
  * TARGET POWER MODE [INT|EXT [AHIGH|ALOW]] — set, query, polarity
  * mode-aware STATUS / bare-TARGET-POWER output labels
  * CLI error handling on malformed power-mode sub-commands (cli-errors rule)

Each test leaves the device back in INTERNAL + disarmed (autouse fixture below),
so it can't corrupt other config_none tests (STATUS / PINS / power-group glitch
availability all depend on the mode).
"""

import re

import pytest


def _glitch_count(raiden):
    m = re.search(r"Glitch Count:\s*(\d+)", raiden.cmd("STATUS", wait=2))
    assert m, "STATUS is missing the 'Glitch Count:' line"
    return int(m.group(1))


def _is_armed(resp):
    # ARM prints 'ARMED' or 'DISARMED' — ARMED is a substring of DISARMED.
    return "ARMED" in resp and "DISARMED" not in resp


@pytest.fixture(autouse=True)
def _restore_internal(raiden):
    """Always return the device to a clean INTERNAL/disarmed state after each test."""
    yield
    raiden.cmd("ARM OFF")
    raiden.cmd("TRIGGER NONE")
    raiden.cmd("TARGET POWER MODE INT")


# ── Setting / querying the mode ──────────────────────────────

class TestPowerModeSet:

    def test_set_internal(self, raiden):
        r = raiden.cmd("TARGET POWER MODE INT")
        assert "Power mode INTERNAL" in r
        assert "ganged" in r

    def test_set_external_bare_enters_external(self, raiden):
        # Bare EXT enters EXTERNAL mode; it keeps the current polarity (which is
        # AHIGH at boot but sticky once set), so don't assert a specific polarity here.
        r = raiden.cmd("TARGET POWER MODE EXT")
        assert "Power mode EXTERNAL" in r
        assert "crowbar gate" in r

    def test_external_polarity_is_sticky(self, raiden):
        # Bare EXT (no polarity arg) retains the last-set polarity across a mode round-trip.
        raiden.cmd("TARGET POWER MODE EXT ALOW")
        raiden.cmd("TARGET POWER MODE INT")
        r = raiden.cmd("TARGET POWER MODE EXT")   # no polarity arg -> retains ALOW
        assert "active-LOW/idle-HIGH" in r

    def test_set_external_ahigh(self, raiden):
        r = raiden.cmd("TARGET POWER MODE EXT AHIGH")
        assert "EXTERNAL" in r
        assert "active-HIGH/idle-LOW" in r

    def test_set_external_alow(self, raiden):
        r = raiden.cmd("TARGET POWER MODE EXT ALOW")
        assert "EXTERNAL" in r
        assert "active-LOW/idle-HIGH" in r

    def test_query_internal(self, raiden):
        raiden.cmd("TARGET POWER MODE INT")
        r = raiden.cmd("TARGET POWER MODE")
        assert "Power mode: INTERNAL" in r
        assert "ganged target power source" in r

    def test_query_external_reports_polarity(self, raiden):
        raiden.cmd("TARGET POWER MODE EXT ALOW")
        r = raiden.cmd("TARGET POWER MODE")
        assert "Power mode: EXTERNAL" in r
        assert "crowbar gate" in r
        assert "active-LOW, idle HIGH" in r

    def test_polarity_round_trip(self, raiden):
        raiden.cmd("TARGET POWER MODE EXT ALOW")
        assert "active-LOW" in raiden.cmd("TARGET POWER MODE")
        raiden.cmd("TARGET POWER MODE EXT AHIGH")
        assert "active-HIGH" in raiden.cmd("TARGET POWER MODE")

    def test_full_word_external_internal_accepted(self, raiden):
        # The matcher accepts the canonical names, not just the INT/EXT abbreviations.
        assert "EXTERNAL" in raiden.cmd("TARGET POWER MODE EXTERNAL")
        assert "INTERNAL" in raiden.cmd("TARGET POWER MODE INTERNAL")


# ── Mode-aware STATUS / bare TARGET POWER ────────────────────

class TestPowerModeStatus:

    def test_status_internal_power_line(self, raiden):
        raiden.cmd("TARGET POWER MODE INT")
        r = raiden.cmd("STATUS", wait=2)
        assert "Power mode:   INTERNAL (GP10/11/12 ganged source)" in r
        assert "Power (GP10/11/12):" in r

    def test_status_external_power_line(self, raiden):
        raiden.cmd("TARGET POWER MODE EXT AHIGH")
        r = raiden.cmd("STATUS", wait=2)
        assert "Power mode:   EXTERNAL (GP10 supply enable, GP11 crowbar gate active-HIGH/idle-LOW, GP12 spare)" in r
        assert "Supply (GP10):" in r

    def test_bare_target_power_internal(self, raiden):
        raiden.cmd("TARGET POWER MODE INT")
        r = raiden.cmd("TARGET POWER")
        assert "Target power (GP10/11/12):" in r
        assert ("ON" in r or "OFF" in r)

    def test_bare_target_power_external(self, raiden):
        raiden.cmd("TARGET POWER MODE EXT")
        r = raiden.cmd("TARGET POWER")
        assert "Target power (GP10 supply):" in r

    def test_pins_reflects_external_mode(self, raiden):
        raiden.cmd("TARGET POWER MODE EXT")
        r = raiden.cmd("PINS", wait=2)
        assert "GP10 - Target Power supply enable (EXTERNAL mode)" in r
        assert "Crowbar gate" in r

    def test_pins_reflects_internal_mode(self, raiden):
        raiden.cmd("TARGET POWER MODE INT")
        r = raiden.cmd("PINS", wait=2)
        assert "GP10 - Target Power (ganged" in r


# ── CLI error handling (cli-errors rule: never silently no-op) ──

class TestPowerModeErrors:

    def test_power_ext_without_mode(self, raiden):
        # 'EXT' is not a POWER sub-command (MODE is) — must error, not no-op.
        r = raiden.cmd("TARGET POWER EXT")
        assert "ERROR" in r
        assert "Unknown" in r

    def test_power_mode_bogus(self, raiden):
        r = raiden.cmd("TARGET POWER MODE BOGUS")
        assert "ERROR" in r

    def test_power_mode_ext_bogus_polarity(self, raiden):
        r = raiden.cmd("TARGET POWER MODE EXT BOGUS")
        assert "ERROR" in r

    def test_power_bogus_subcommand(self, raiden):
        r = raiden.cmd("TARGET POWER WIBBLE")
        assert "ERROR" in r

    def test_mode_change_while_armed_refused(self, raiden):
        raiden.cmd("TRIGGER GPIO RISING")
        raiden.cmd("ARM ON")
        r = raiden.cmd("TARGET POWER MODE EXT")
        assert "ERROR" in r
        assert "disarm" in r.lower()
        raiden.cmd("ARM OFF")


# ── Crowbar gate firing path (EXTERNAL mode) ─────────────────
# Exercises the crowbar code path — crowbar_pulse_start/stop and the 2nd PIO0
# pulse SM that drives GP11 — over USB. The GP11 waveform itself (polarity,
# pulse count, width) is validated separately with a scope by
# scripts/crowbar_scope_test.py. Here we only assert the firmware-observable
# side effects: arming + firing in EXTERNAL succeeds, the glitch count bumps,
# and the system soft-disarms afterwards, for both gate polarities.
#
# WIRING: config_none assumes nothing is strapped across GP10/11/12. On a bench
# with the ganged power harness still connected, free GP11 (the crowbar line)
# before running these, or driving GP11 fights GP10.

class TestCrowbarFire:

    def _arm_fire_disarm(self, raiden, pol):
        raiden.cmd(f"TARGET POWER MODE EXT {pol}")
        raiden.cmd("TRIGGER NONE")
        raiden.cmd("SET COUNT 1")
        before = _glitch_count(raiden)
        assert "armed" in raiden.cmd("ARM ON").lower()      # crowbar_pulse_start ran
        assert _is_armed(raiden.cmd("ARM"))
        assert "Glitch executed" in raiden.cmd("GLITCH")    # GP2 + GP11 SMs fired
        assert _glitch_count(raiden) == before + 1
        assert "DISARMED" in raiden.cmd("ARM")              # soft-disarm in EXTERNAL

    def test_arm_fire_external_ahigh(self, raiden):
        self._arm_fire_disarm(raiden, "AHIGH")

    def test_arm_fire_external_alow(self, raiden):
        self._arm_fire_disarm(raiden, "ALOW")

    def test_arm_fire_external_multipulse(self, raiden):
        # COUNT>1 still produces exactly one glitch event (one count bump) and disarms.
        raiden.cmd("TARGET POWER MODE EXT AHIGH")
        raiden.cmd("TRIGGER NONE")
        raiden.cmd("SET COUNT 3")
        before = _glitch_count(raiden)
        raiden.cmd("ARM ON")
        assert "Glitch executed" in raiden.cmd("GLITCH")
        assert _glitch_count(raiden) == before + 1
        assert "DISARMED" in raiden.cmd("ARM")
        raiden.cmd("SET COUNT 1")
