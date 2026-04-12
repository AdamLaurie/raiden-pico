"""Config SWD: Tests requiring an STM32F1 target connected via SWD.

Wiring:
    GP17  -> STM32 SWCLK
    GP18  -> STM32 SWDIO
    GP15  -> STM32 nRST
    GP13  -> STM32 BOOT0
    GP10/11/12 -> Target VDD (or external power)
    Target must be powered and responsive.
"""

import pytest


pytestmark = pytest.mark.config_swd


def _require_rdp0(swd_target):
    """Skip test if target is not at RDP Level 0 (flash inaccessible)."""
    r = swd_target.cmd("SWD RDP")
    if "Level 0" not in r:
        pytest.skip("Target not at RDP0 — flash access blocked")


# ── Connection ───────────────────────────────────────────────

class TestSWDConnect:

    def test_connect(self, swd_target):
        r = swd_target.cmd("SWD CONNECT")
        assert "Connected" in r
        assert "DPIDR" in r

    def test_connect_idempotent(self, swd_target):
        swd_target.cmd("SWD CONNECT")
        r = swd_target.cmd("SWD CONNECT")
        assert "Connected" in r

    def test_connectrst(self, swd_target):
        r = swd_target.cmd("SWD CONNECTRST", wait=2)
        assert "Connected" in r

    def test_disconnect_reconnect(self, swd_target):
        swd_target.cmd("SWD DISCONNECT")
        r = swd_target.cmd("SWD CONNECT")
        assert "Connected" in r

    def test_idcode(self, swd_target):
        r = swd_target.cmd("SWD IDCODE")
        assert "DPIDR" in r
        assert "CPUID" in r
        assert "Cortex" in r

    def test_help_text(self, swd_target):
        r = swd_target.cmd("SWD")
        for sub in ["CONNECT", "CONNECTRST", "DISCONNECT", "READ", "WRITE",
                     "FILL", "HALT", "RESUME", "RESET", "RDP", "OPT",
                     "REGS", "SETREG", "SPEED", "BPTEST"]:
            assert sub in r, f"SWD help missing: {sub}"

    def test_prefix_ambiguous(self, swd_target):
        r = swd_target.cmd("SWD CON")
        assert "Ambiguous" in r

    def test_prefix_unambiguous(self, swd_target):
        r = swd_target.cmd("SWD ID")
        assert "DPIDR" in r

    def test_unknown_subcmd(self, swd_target):
        r = swd_target.cmd("SWD FOOBAR")
        assert "ERROR" in r


# ── Memory Read ──────────────────────────────────────────────

class TestSWDRead:

    def test_read_cpuid(self, swd_target):
        swd_target.cmd("SWD HALT")
        r = swd_target.cmd("SWD READ E000ED00")
        assert "E000ED00:" in r

    def test_read_count(self, swd_target):
        r = swd_target.cmd("SWD READ 20000000 4")
        assert "Reading 16 bytes" in r

    def test_read_sram_alias(self, swd_target):
        r = swd_target.cmd("SWD READ SRAM 4")
        assert "Reading 16 bytes" in r
        assert "20000000:" in r

    def test_read_flash_alias(self, swd_target):
        _require_rdp0(swd_target)
        r = swd_target.cmd("SWD READ FLASH 4")
        assert "Reading 16 bytes" in r
        assert "08000000:" in r

    def test_read_dp(self, swd_target):
        r = swd_target.cmd("SWD READ DP 0")
        assert "DP[0x0]" in r

    def test_read_ap(self, swd_target):
        r = swd_target.cmd("SWD READ AP FC")
        assert "AP[0xFC]" in r

    def test_read_missing_addr(self, swd_target):
        r = swd_target.cmd("SWD READ DP")
        assert "ERROR" in r


# ── Memory Write ─────────────────────────────────────────────

class TestSWDWrite:

    def test_write_sram_verify(self, swd_target):
        swd_target.cmd("SWD HALT")
        r = swd_target.cmd("SWD WRITE 20000000 DEADBEEF")
        assert "verified" in r

    def test_write_sram_readback(self, swd_target):
        swd_target.cmd("SWD WRITE 20000000 12345678")
        r = swd_target.cmd("SWD READ 20000000")
        assert "78 56 34 12" in r

    def test_write_flash_prompts_erase(self, swd_target):
        r = swd_target.cmd("SWD WRITE 08000000 DEADBEEF")
        assert "ERASE" in r

    @pytest.mark.destructive
    def test_write_flash_with_erase(self, swd_target):
        _require_rdp0(swd_target)
        r = swd_target.cmd("SWD WRITE 08000000 DEADBEEF ERASE", wait=3)
        assert "verified" in r

    def test_write_dp(self, swd_target):
        r = swd_target.cmd("SWD WRITE DP 0 1E")
        assert "DP[0x0]" in r


# ── Memory Fill ──────────────────────────────────────────────

class TestSWDFill:

    def test_fill_sram(self, swd_target):
        swd_target.cmd("SWD HALT")
        r = swd_target.cmd("SWD FILL 20000000 A5A5A5A5 8")
        assert "Filled 8 words" in r
        assert "verified" in r

    def test_fill_sram_readback(self, swd_target):
        swd_target.cmd("SWD FILL 20000000 A5A5A5A5 8")
        r = swd_target.cmd("SWD READ 20000000 8")
        assert "A5 A5 A5 A5" in r

    def test_fill_missing_args(self, swd_target):
        r = swd_target.cmd("SWD FILL")
        assert "ERROR" in r

    def test_fill_flash_prompts_erase(self, swd_target):
        r = swd_target.cmd("SWD FILL FLASH DEADBEEF 4")
        assert "ERASE" in r


# ── Core Registers ───────────────────────────────────────────

class TestSWDRegs:

    def test_regs_all(self, swd_target):
        swd_target.cmd("SWD HALT")
        r = swd_target.cmd("SWD REGS", wait=2)
        for reg in ["r0", "sp", "lr", "pc", "xPSR"]:
            assert reg in r, f"REGS missing: {reg}"

    def test_setreg_readback(self, swd_target):
        swd_target.cmd("SWD HALT")
        swd_target.cmd("SWD SETREG R0 AABBCCDD")
        r = swd_target.cmd("SWD REGS", wait=2)
        assert "AABBCCDD" in r.upper()


# ── Target Reset ─────────────────────────────────────────────

class TestSWDReset:

    def test_reset_pulse(self, swd_target):
        r = swd_target.cmd("SWD RESET", wait=1.5)
        assert "OK" in r

    def test_reconnect_after_reset(self, swd_target):
        swd_target.cmd("SWD RESET", wait=1.5)
        r = swd_target.cmd("SWD CONNECT")
        assert "Connected" in r

    def test_reset_hold_release(self, swd_target):
        r = swd_target.cmd("SWD RESET HOLD")
        assert "OK" in r

        r = swd_target.cmd("SWD RESET RELEASE")
        assert "OK" in r

        r = swd_target.cmd("SWD CONNECT")
        assert "Connected" in r

    def test_halt_resume(self, swd_target):
        swd_target.cmd("SWD CONNECT")
        r = swd_target.cmd("SWD HALT")
        assert "OK" in r

        r = swd_target.cmd("SWD RESUME")
        assert "OK" in r


# ── Flash Operations ─────────────────────────────────────────

@pytest.mark.destructive
class TestSWDFlash:

    def test_flash_erase_page0(self, swd_target):
        _require_rdp0(swd_target)
        swd_target.cmd("SWD HALT")
        r = swd_target.cmd("SWD FLASH ERASE 0", wait=2)
        assert "OK" in r

    def test_flash_erased_is_ff(self, swd_target):
        _require_rdp0(swd_target)
        swd_target.cmd("SWD HALT")
        swd_target.cmd("SWD FLASH ERASE 0", wait=2)
        r = swd_target.cmd("SWD READ 08000000 4")
        assert "FF FF FF FF" in r

    def test_flash_write_persists_reset(self, swd_target):
        _require_rdp0(swd_target)
        swd_target.cmd("SWD HALT")
        swd_target.cmd("SWD WRITE 08000000 F00DCAFE ERASE", wait=3)
        swd_target.cmd("SWD RESET", wait=1.5)
        swd_target.cmd("SWD CONNECT")
        swd_target.cmd("SWD HALT")
        r = swd_target.cmd("SWD READ 08000000")
        assert "FE CA 0D F0" in r


# ── Option Bytes ─────────────────────────────────────────────

class TestSWDOpt:

    def test_opt_read(self, swd_target):
        swd_target.cmd("SWD HALT")
        r = swd_target.cmd("SWD OPT")
        assert "RDP" in r
        assert "ERROR" not in r

    def test_rdp_read(self, swd_target):
        r = swd_target.cmd("SWD RDP")
        assert "RDP Level" in r


# ── Error Recovery ───────────────────────────────────────────

class TestSWDErrors:

    def test_bad_addr_recovers(self, swd_target):
        swd_target.cmd("SWD HALT")
        swd_target.cmd("SWD READ FFFFFFFC")  # triggers STICKYERR
        r = swd_target.cmd("SWD READ E000ED00")
        assert "E000ED00:" in r

    def test_multiple_errors_recover(self, swd_target):
        swd_target.cmd("SWD READ FFFFFFFC")
        swd_target.cmd("SWD READ FFFFFFFC")
        swd_target.cmd("SWD READ FFFFFFFC")
        r = swd_target.cmd("SWD READ SRAM 4")
        assert "Read complete" in r

    def test_sram_works_after_error(self, swd_target):
        swd_target.cmd("SWD HALT")
        swd_target.cmd("SWD READ FFFFFFFC")
        r = swd_target.cmd("SWD WRITE 20000000 AABBCCDD")
        assert "verified" in r

    def test_halt_when_halted(self, swd_target):
        swd_target.cmd("SWD HALT")
        r = swd_target.cmd("SWD HALT")
        assert "OK" in r


# ── Trigger Fire (requires UART GP4/GP5 wiring) ────────────

class TestTriggerFire:
    """Verify triggers actually fire. Requires UART wiring (GP4/GP5)."""

    def _cleanup(self, cli):
        cli.cmd("ARM OFF")
        cli.cmd("TRIGGER NONE")

    def _arm_ok(self, cli):
        """Arm and return True only if ARM ON actually succeeded."""
        r = cli.cmd("ARM ON")
        return "OK" in r and "ERROR" not in r

    def test_uart_tx_trigger_fires(self, swd_target):
        """ARM with UART TX trigger, send the trigger byte, confirm it fires."""
        cli = swd_target
        self._cleanup(cli)

        # Init target UART first so PIO trigger sets up after pin is configured
        cli.cmd("TARGET SEND 00", wait=0.5)

        cli.cmd("TRIGGER UART 0D TX")
        assert self._arm_ok(cli), "Failed to arm — PIO may be full"

        r = cli.cmd("ARM")
        assert "DISARMED" not in r, "System disarmed before trigger sent"

        # Send the trigger byte on TX — PIO UART decoder should match
        cli.cmd("TARGET SEND 0D", wait=0.5)

        # Trigger should have fired → system disarmed
        r = cli.cmd("ARM")
        assert "DISARMED" in r, "UART TX trigger did not fire"

        self._cleanup(cli)

    def test_uart_tx_wrong_byte_no_fire(self, swd_target):
        """ARM with UART TX trigger for 0D, send different byte, should stay armed."""
        cli = swd_target
        self._cleanup(cli)

        # Init target UART first
        cli.cmd("TARGET SEND 00", wait=0.5)

        cli.cmd("TRIGGER UART 0D TX")
        assert self._arm_ok(cli), "Failed to arm — PIO may be full"

        # Send a non-matching byte
        cli.cmd("TARGET SEND AA", wait=0.5)

        # Should still be armed (wrong byte)
        r = cli.cmd("ARM")
        assert "DISARMED" not in r, "False trigger — system disarmed on wrong byte (0xAA)"

        self._cleanup(cli)

    def test_uart_rx_trigger_fires(self, swd_target):
        """ARM with UART RX trigger, enter bootloader, sync triggers ACK (0x79)."""
        cli = swd_target
        self._cleanup(cli)

        # Put target in bootloader mode via BOOT0 + reset
        cli.cmd("TARGET STM32F1")
        r = cli.cmd("TARGET SYNC", wait=15)
        if "ACK" not in r:
            self._cleanup(cli)
            pytest.skip("TARGET SYNC failed — bootloader not responding on UART")

        # Now set RX trigger for ACK byte and arm
        cli.cmd("TRIGGER UART 79 RX")
        assert self._arm_ok(cli), "Failed to arm — PIO may be full"

        r = cli.cmd("ARM")
        assert "DISARMED" not in r, "System disarmed before sending command"

        # Send GET command (0x00 0xFF) — bootloader replies with ACK (0x79) + data
        cli.cmd("TARGET SEND 00FF", wait=1)

        r = cli.cmd("ARM")
        assert "DISARMED" in r, "UART RX trigger did not fire on bootloader ACK"

        self._cleanup(cli)

    def test_gpio_trigger_fires(self, swd_target):
        """ARM with GPIO RISING trigger on GP3, confirm PIO loads and arms.

        Note: Actually firing the trigger requires GP3 wired to a signal source.
        The default trigger pin (GP3) is NOT wired to nRST (GP15), so we only
        test that arming succeeds (PIO program loads) and the system stays armed.
        """
        cli = swd_target
        self._cleanup(cli)

        cli.cmd("TRIGGER GPIO RISING")
        if not self._arm_ok(cli):
            self._cleanup(cli)
            pytest.skip("PIO0 full — cannot load GPIO trigger")

        r = cli.cmd("ARM")
        assert "ARMED" in r, "GPIO trigger did not arm properly"

        self._cleanup(cli)
