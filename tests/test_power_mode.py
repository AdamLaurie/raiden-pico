"""TARGET POWER INTERNAL/EXTERNAL (crowbar) regression tests.

GATED — not config_none. These drive the GP10/11/12 power group and (in
TestCrowbarFire) arm + fire the crowbar gate, so they require an explicit
wiring config and never run under the default `pytest tests/`:

    --config=power-int   INTERNAL-mode tests (ganged GP10/11/12 power)
    --config=power-ext   EXTERNAL-mode tests + crowbar gate firing
    (dual-mode tests carry both markers and need both flags)

SAFETY: run only with the bench wired for the matching mode, or the target
disconnected. On the wrong wiring EXTERNAL drives GP10 high while GP11/12 idle
low — a ganged 3-wire harness shorts them, collapsing the rail (false OFF
readings), and firing the crowbar into a real target can damage it.

Covers the feat/crowbar-power-mode additions:
  * TARGET POWER [INT|EXT [AHIGH|ALOW]] — set mode directly (no MODE keyword),
    query (bare TARGET POWER), polarity
  * mode-aware STATUS / bare-TARGET-POWER output labels
  * CLI error handling on malformed power sub-commands, incl. the now-removed
    MODE keyword (cli-errors rule)
  * crowbar gate arm/fire/soft-disarm in EXTERNAL (both polarities)

Each test leaves the device de-energized, INTERNAL and disarmed (autouse
fixture below), so it can't strand a later test or clamp a rail.
"""

import pytest


@pytest.fixture(autouse=True)
def _restore_internal(raiden):
    """Return to a safe state after each test: disarmed, de-energized, INTERNAL.
    Power OFF first so re-ganging GP11/12 in INTERNAL can't drive a live rail."""
    yield
    raiden.cmd("ARM OFF")
    raiden.cmd("TRIGGER NONE")
    raiden.cmd("TARGET POWER OFF")
    raiden.cmd("TARGET POWER INT")


# ── Setting / querying the mode ──────────────────────────────

class TestPowerModeSet:

    @pytest.mark.config_power_int
    def test_set_internal(self, raiden):
        r = raiden.cmd("TARGET POWER INT")
        assert "Power mode INTERNAL" in r
        assert "ganged" in r

    @pytest.mark.config_power_ext
    def test_set_external_bare_enters_external(self, raiden):
        # Bare EXT enters EXTERNAL mode; it keeps the current polarity (which is
        # AHIGH at boot but sticky once set), so don't assert a specific polarity here.
        r = raiden.cmd("TARGET POWER EXT")
        assert "Power mode EXTERNAL" in r
        assert "crowbar gate" in r

    @pytest.mark.config_power_int
    @pytest.mark.config_power_ext
    def test_external_polarity_is_sticky(self, raiden):
        # Bare EXT (no polarity arg) retains the last-set polarity across a mode round-trip.
        raiden.cmd("TARGET POWER EXT ALOW")
        raiden.cmd("TARGET POWER INT")
        r = raiden.cmd("TARGET POWER EXT")   # no polarity arg -> retains ALOW
        assert "active-LOW/idle-HIGH" in r

    @pytest.mark.config_power_ext
    def test_set_external_ahigh(self, raiden):
        r = raiden.cmd("TARGET POWER EXT AHIGH")
        assert "EXTERNAL" in r
        assert "active-HIGH/idle-LOW" in r

    @pytest.mark.config_power_ext
    def test_set_external_alow(self, raiden):
        r = raiden.cmd("TARGET POWER EXT ALOW")
        assert "EXTERNAL" in r
        assert "active-LOW/idle-HIGH" in r

    @pytest.mark.config_power_int
    def test_query_internal(self, raiden):
        raiden.cmd("TARGET POWER INT")
        r = raiden.cmd("TARGET POWER")          # bare query now reports state + mode
        assert "Power mode: INTERNAL" in r
        assert "ganged power source" in r

    @pytest.mark.config_power_ext
    def test_query_external_reports_polarity(self, raiden):
        raiden.cmd("TARGET POWER EXT ALOW")
        r = raiden.cmd("TARGET POWER")
        assert "Power mode: EXTERNAL" in r
        assert "crowbar gate" in r
        assert "active-LOW/idle-HIGH" in r

    @pytest.mark.config_power_ext
    def test_polarity_round_trip(self, raiden):
        raiden.cmd("TARGET POWER EXT ALOW")
        assert "active-LOW" in raiden.cmd("TARGET POWER")
        raiden.cmd("TARGET POWER EXT AHIGH")
        assert "active-HIGH" in raiden.cmd("TARGET POWER")

    @pytest.mark.config_power_int
    @pytest.mark.config_power_ext
    def test_full_word_external_internal_accepted(self, raiden):
        # The matcher accepts the canonical names, not just the INT/EXT abbreviations.
        assert "EXTERNAL" in raiden.cmd("TARGET POWER EXTERNAL")
        assert "INTERNAL" in raiden.cmd("TARGET POWER INTERNAL")


# ── Mode-aware STATUS + bare TARGET POWER ────────────────────

class TestPowerModeStatus:

    @pytest.mark.config_power_int
    def test_status_internal_power_line(self, raiden):
        raiden.cmd("TARGET POWER INT")
        r = raiden.cmd("STATUS", wait=2)
        assert "Power mode:   INTERNAL (GP10/11/12 ganged source)" in r
        assert "Power (GP10/11/12):" in r

    @pytest.mark.config_power_ext
    def test_status_external_power_line(self, raiden):
        raiden.cmd("TARGET POWER EXT AHIGH")
        r = raiden.cmd("STATUS", wait=2)
        assert "Power mode:   EXTERNAL (GP10 supply enable, GP11 crowbar gate active-HIGH/idle-LOW, GP12 spare)" in r
        assert "Supply (GP10):" in r

    @pytest.mark.config_power_int
    def test_bare_target_power_internal(self, raiden):
        raiden.cmd("TARGET POWER INT")
        r = raiden.cmd("TARGET POWER")
        assert "Target power (GP10/11/12):" in r
        assert ("ON" in r or "OFF" in r)

    @pytest.mark.config_power_ext
    def test_bare_target_power_external(self, raiden):
        raiden.cmd("TARGET POWER EXT")
        r = raiden.cmd("TARGET POWER")
        assert "Target power (GP10 supply):" in r

    @pytest.mark.config_power_ext
    def test_pins_reflects_external_mode(self, raiden):
        raiden.cmd("TARGET POWER EXT")
        r = raiden.cmd("PINS", wait=2)
        assert "GP10 - Target Power supply enable (EXTERNAL mode)" in r
        assert "Crowbar gate" in r

    @pytest.mark.config_power_int
    def test_pins_reflects_internal_mode(self, raiden):
        raiden.cmd("TARGET POWER INT")
        r = raiden.cmd("PINS", wait=2)
        assert "GP10 - Target Power (ganged" in r


# ── CLI error handling (cli-errors rule: never silently no-op) ──

@pytest.mark.config_power_int
class TestPowerModeErrors:

    def test_power_unknown_subcommand(self, raiden):
        r = raiden.cmd("TARGET POWER WIBBLE")
        assert "ERROR" in r
        assert "Unknown" in r

    def test_old_mode_keyword_rejected(self, raiden):
        # The MODE keyword was removed (now: TARGET POWER INT|EXT). The old syntax
        # must error explicitly, not silently no-op or mis-run.
        r = raiden.cmd("TARGET POWER MODE EXT")
        assert "ERROR" in r
        assert "Unknown" in r

    def test_power_ext_bogus_polarity(self, raiden):
        r = raiden.cmd("TARGET POWER EXT BOGUS")
        assert "ERROR" in r

    def test_mode_change_while_armed_refused(self, raiden):
        raiden.cmd("TRIGGER GPIO RISING")
        raiden.cmd("ARM ON")
        r = raiden.cmd("TARGET POWER EXT")
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
# WIRING (config_power_ext): the crowbar gate (GP11) fires here. Run only with
# the bench wired for EXTERNAL/crowbar (GP11 broken out to the MOSFET gate) or
# the target disconnected. With the ganged 3-wire power harness still connected,
# driving GP11 fights GP10 (shorted rail, false readings) — free GP11 first.

def _glitch_count(raiden):
    import re
    m = re.search(r"Glitch Count:\s*(\d+)", raiden.cmd("STATUS", wait=2))
    assert m, "STATUS is missing the 'Glitch Count:' line"
    return int(m.group(1))


def _is_armed(resp):
    return "ARMED" in resp and "DISARMED" not in resp


@pytest.mark.config_power_ext
class TestCrowbarFire:

    def _arm_fire_disarm(self, raiden, pol):
        raiden.cmd(f"TARGET POWER EXT {pol}")
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
        raiden.cmd("TARGET POWER EXT AHIGH")
        raiden.cmd("TRIGGER NONE")
        raiden.cmd("SET COUNT 3")
        before = _glitch_count(raiden)
        raiden.cmd("ARM ON")
        assert "Glitch executed" in raiden.cmd("GLITCH")
        assert _glitch_count(raiden) == before + 1
        assert "DISARMED" in raiden.cmd("ARM")
        raiden.cmd("SET COUNT 1")


# ── Boot power-state default (power-on-reset) ────────────────
# The boot default is OFF (de-energized) in both modes. The only honest way to
# check a power-ON-RESET state is to actually reboot: energize, REBOOT, drop and
# re-open the tty, then read power BEFORE issuing any power command. Gated
# power-int (boot mode is INTERNAL; drives the ganged group), and it reboots the
# device — keep it last in the file so the autouse restore runs on the
# reconnected client.

@pytest.mark.config_power_int
class TestBootPowerDefault:

    def test_power_off_at_boot(self, raiden):
        # Energize first, so a post-reboot OFF proves the boot default rather than
        # leftover state.
        raiden.cmd("TARGET POWER INT")
        raiden.cmd("TARGET POWER ON")
        assert "ON" in raiden.cmd("TARGET POWER")
        # Soft reboot: firmware restarts and target_init drives the group LOW.
        # reboot() writes REBOOT without draining (CDC drops mid-command) and
        # reconnects the tty.
        raiden.reboot()
        # First power query after boot, before any power command:
        r = raiden.cmd("TARGET POWER")
        assert "OFF" in r, f"expected boot power OFF, got: {r!r}"
        assert "INTERNAL" in r          # boot mode is INTERNAL
