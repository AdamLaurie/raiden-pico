"""Hardware-UART ownership (uart_mux) regression tests.

UART0 (ChipSHOUTER GP0/1) and UART1 (Target GP4/5, GRBL GP8/9, PSU GP10/11) are
single peripherals with several pin alternates. Every user claims / borrows them
through uart_mux, which detaches the previous owner and parks its pins. STATUS
reports the current owner of each UART, so the handovers are observable over USB.

TestUartOwnership is config_none: it only switches UART1 between the target and
GRBL pins and sends a byte with nothing attached (same as TestUartSwitching).

TestPsuUartSharing needs --config=psu (claims the PSU: GP10 TX idles HIGH, so
GP10/11 must go to the PSU MAX3232 or be unconnected). It passes with or without
a PSU attached; with one, it also checks the PSU still answers after the target
and GRBL have re-programmed UART1. It never sends PSU VOLT / CURR.
"""

import re

import pytest


def _uart(raiden, n):
    r = raiden.cmd("STATUS", wait=2)
    m = re.search(rf"UART{n}:\s+(.+)", r)
    assert m, f"STATUS is missing the 'UART{n}:' line"
    return m.group(1).strip()


class TestUartOwnership:

    def test_uart0_owned_by_chipshouter(self, raiden):
        assert _uart(raiden, 0).startswith("ChipSHOUTER (GP0/GP1) 115200 8N1")

    def test_target_then_grbl_then_target(self, raiden):
        raiden.cmd("TARGET LPC")
        raiden.cmd("TARGET SEND 7F", wait=1)
        assert _uart(raiden, 1).startswith("Target (GP4/GP5)")
        raiden.cmd("GRBL POS", wait=3)
        assert _uart(raiden, 1).startswith("GRBL (GP8/GP9) 115200 8N1")
        r = raiden.cmd("TARGET SEND 7F", wait=1)
        assert "reclaimed from GRBL" in r
        assert _uart(raiden, 1).startswith("Target (GP4/GP5)")

    def test_stm32_target_reinit_uses_8e1(self, raiden):
        raiden.cmd("GRBL POS", wait=3)           # force a fresh target init next
        raiden.cmd("TARGET STM32F1")
        raiden.cmd("TARGET SEND 7F", wait=1)
        assert _uart(raiden, 1).startswith("Target (GP4/GP5) 115200 8E1")
        raiden.cmd("TARGET LPC")

    def test_target_baud_applies_live_when_target_owns(self, raiden):
        raiden.cmd("TARGET SEND 7F", wait=1)
        raiden.cmd("TARGET BAUD 57600")
        try:
            assert _uart(raiden, 1).startswith("Target (GP4/GP5) 57600")
        finally:
            raiden.cmd("TARGET BAUD 115200")

    def test_target_baud_and_format_change_only_their_half(self, raiden):
        # TARGET BAUD must not reset an STM32 session's 8E1 to the console 8N1, and
        # TARGET FORMAT must not reset the baud (regression in the first uart_mux cut).
        raiden.cmd("GRBL POS", wait=3)
        raiden.cmd("TARGET STM32F1")
        raiden.cmd("TARGET SEND 7F", wait=1)
        try:
            raiden.cmd("TARGET BAUD 57600")
            assert _uart(raiden, 1).startswith("Target (GP4/GP5) 57600 8E1")
            raiden.cmd("TARGET FORMAT 7O2")
            assert _uart(raiden, 1).startswith("Target (GP4/GP5) 57600 7O2")
        finally:
            raiden.cmd("TARGET BAUD 115200")
            raiden.cmd("TARGET FORMAT 8N1")
            raiden.cmd("TARGET LPC")

    @pytest.mark.parametrize("pin,owner", [("0", "ChipSHOUTER"), ("1", "ChipSHOUTER"),
                                           ("4", "Target"), ("5", "Target"),
                                           ("8", "GRBL"), ("11", "PSU")])
    def test_reset_pin_refuses_uart_pins(self, raiden, pin, owner):
        r = raiden.cmd(f"TARGET RESET PIN {pin}")
        assert "ERROR" in r and f"{owner} UART pin" in r
        assert _uart(raiden, 0).startswith("ChipSHOUTER (GP0/GP1)")   # UART0 untouched

    @pytest.mark.parametrize("arg", ["99", "abc", "-1"])
    def test_reset_pin_rejects_bad_numbers(self, raiden, arg):
        r = raiden.cmd(f"TARGET RESET PIN {arg}")
        assert "ERROR" in r and "Invalid TARGET RESET PIN" in r

    def test_reset_pin_default_still_accepted(self, raiden):
        r = raiden.cmd("TARGET RESET PIN 15")
        assert "OK: Reset configured: GP15" in r


@pytest.mark.config_psu
class TestPsuUartSharing:
    """PSU transactions borrow UART1 and hand it back; the target / GRBL taking
    UART1 must no longer kill the PSU link (it used to: UART1 re-initialised at
    the target's baud while the PSU code still thought it owned it)."""

    @pytest.fixture(autouse=True, scope="class")
    def _psu(self, raiden):
        raiden.cmd("ARM OFF")
        r = raiden.cmd("PSU ID", wait=2)
        assert "PSU UART claimed" in r or "PSU ID:" in r, r
        yield
        raiden.cmd("TARGET BAUD 115200")
        raiden.cmd("TARGET FORMAT 8N1")
        raiden.cmd("PSU RELEASE")

    @staticmethod
    def _psu_answers(raiden):
        r = raiden.cmd("PSU STATUS", wait=3)
        return "PSU set:" in r, r

    def test_psu_query_hands_uart1_back_to_target(self, raiden):
        raiden.cmd("TARGET LPC")
        raiden.cmd("TARGET SEND 7F", wait=1)
        assert _uart(raiden, 1).startswith("Target (GP4/GP5)")
        ok, r = self._psu_answers(raiden)
        assert ok or "no response" in r, r
        assert _uart(raiden, 1).startswith("Target (GP4/GP5)"), "PSU borrow did not restore the target"

    def test_psu_survives_target_baud_and_format_change(self, raiden):
        present, _ = self._psu_answers(raiden)
        raiden.cmd("TARGET SEND 7F", wait=1)
        raiden.cmd("TARGET BAUD 115200")
        raiden.cmd("TARGET FORMAT 8E1")
        assert _uart(raiden, 1).startswith("Target (GP4/GP5) 115200 8E1")
        ok, r = self._psu_answers(raiden)
        assert ok == present, f"PSU link changed after TARGET BAUD/FORMAT: {r}"
        assert _uart(raiden, 1).startswith("Target (GP4/GP5) 115200 8E1"), "target line not restored"

    def test_psu_survives_grbl(self, raiden):
        present, _ = self._psu_answers(raiden)
        raiden.cmd("GRBL POS", wait=3)
        assert _uart(raiden, 1).startswith("GRBL (GP8/GP9)")
        ok, r = self._psu_answers(raiden)
        assert ok == present, f"PSU link changed after GRBL: {r}"
        assert _uart(raiden, 1).startswith("GRBL (GP8/GP9)"), "PSU borrow did not restore GRBL"

    def test_power_verbs_still_route_to_psu_after_target_session(self, raiden):
        present, _ = self._psu_answers(raiden)
        raiden.cmd("TARGET SEND 7F", wait=1)
        r = raiden.cmd("TARGET POWER ON", wait=6)
        if present:
            assert "OK: Target power ON (PSU output)" in r, r
        else:
            assert "ERROR: PSU did not confirm output" in r, r
        assert _uart(raiden, 1).startswith("Target (GP4/GP5)")
