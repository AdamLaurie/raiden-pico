"""Config SWD RDP: Destructive RDP level tests - mass erases target flash.

Wiring: Same as Config SWD.
    GP17  -> STM32 SWCLK
    GP18  -> STM32 SWDIO
    GP15  -> STM32 nRST
    GP10/11/12 -> Target VDD (or external power)

WARNING: These tests SET RDP1 and then clear back to RDP0, which triggers
a mass erase of all flash. Only run on expendable targets.

Run explicitly: pytest tests/test_config_swd_rdp.py -v
"""

import time
import pytest


pytestmark = [pytest.mark.config_swd_rdp, pytest.mark.destructive]


@pytest.fixture(scope="module")
def swd_rdp_target(raiden):
    """Ensure SWD target is at RDP0 before RDP tests."""
    raiden.cmd("TARGET STM32F1", wait=0.3)
    r = raiden.cmd("SWD CONNECT")
    if "Connected" not in r:
        pytest.skip("No SWD target connected")

    raiden.cmd("SWD HALT", wait=0.5)
    r = raiden.cmd("SWD RDP")
    if "Level 0" not in r:
        pytest.skip("Target not at RDP0 - refusing destructive RDP tests")
    return raiden


class TestRDPRoundTrip:

    def test_rdp_set_2_refused(self, swd_rdp_target):
        r = swd_rdp_target.cmd("SWD RDP SET 2")
        assert "PERMANENT" in r or "Refusing" in r

    def test_rdp_set_invalid(self, swd_rdp_target):
        r = swd_rdp_target.cmd("SWD RDP SET 3")
        assert "ERROR" in r

    def test_rdp_set_0_prompts_wipe(self, swd_rdp_target):
        r = swd_rdp_target.cmd("SWD RDP SET 0")
        assert "WIPE" in r or "MASS ERASE" in r

    def test_rdp1_round_trip(self, swd_rdp_target):
        """Full RDP0 -> RDP1 -> RDP0 cycle with mass erase."""
        cli = swd_rdp_target

        # Write marker to flash
        cli.cmd("SWD HALT")
        cli.cmd("SWD WRITE 08000000 BADC0FFE ERASE", wait=3)

        # Set RDP1
        r = cli.cmd("SWD RDP SET 1", wait=5)
        assert "OK" in r

        # Reset to apply
        cli.cmd("SWD RESET", wait=2)
        time.sleep(1)

        # Reconnect and verify RDP1
        r = cli.cmd("SWD CONNECT")
        assert "Connected" in r

        cli.cmd("SWD HALT")
        r = cli.cmd("SWD RDP")
        assert "Level 1" in r

        # Flash blocked at RDP1
        r = cli.cmd("SWD READ 08000000")
        assert "ERROR" in r

        # SRAM still accessible
        r = cli.cmd("SWD WRITE 20000000 DEADBEEF")
        assert "verified" in r

        # Set RDP back to 0 (mass erase)
        r = cli.cmd("SWD RDP SET 0 WIPE", wait=10)
        assert "OK" in r

        # Reset and reconnect
        cli.cmd("SWD RESET", wait=2)
        time.sleep(2)
        r = cli.cmd("SWD CONNECT")
        assert "Connected" in r

        cli.cmd("SWD HALT")
        r = cli.cmd("SWD RDP")
        assert "Level 0" in r

        # Flash should be erased
        r = cli.cmd("SWD READ 08000000 4")
        assert "FF FF FF FF" in r
