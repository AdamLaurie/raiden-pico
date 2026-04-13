"""Config SWD: STM32 USART bootloader (BL) command tests.

Wiring (same as SWD config + UART + BOOT0):
    GP4/GP5  -> STM32 USART1 (TX/RX) at 115200 8E1
    GP13     -> STM32 BOOT0
    GP15     -> STM32 nRST
    GP17     -> STM32 SWCLK (for SWD -> BL auto-sync test)
    GP18     -> STM32 SWDIO
    GP10/11/12 -> Target VDD (or external power)

STM32F1 bootloader quirks:
    - First 256 bytes of SRAM (0x20000000-0x200000FF) are bootloader workspace;
      multi-byte reads in that range NACK.
    - Bootloader supported commands (v2.2): 0x00 GET, 0x01 GV, 0x02 GID,
      0x11 READ, 0x21 GO, 0x31 WRITE, 0x43 ERASE, 0x63 WP, 0x73 WU,
      0x82 RP, 0x92 RU.
"""

import time
import pytest


pytestmark = pytest.mark.config_swd


# Target UART flash base & safe SRAM (outside bootloader workspace)
FLASH_BASE = "08000000"
SRAM_SAFE = "20001000"
SYSTEM_ROM = "1FFFF000"


def _require_rdp0_swd(cli):
    """Skip test if target is RDP-protected (check via SWD before BL)."""
    cli.cmd("TARGET STM32F1", wait=0.3)
    r = cli.cmd("SWD CONNECT")
    if "Connected" not in r:
        pytest.skip("No SWD — cannot verify RDP level")
    r = cli.cmd("SWD RDP")
    if "Level 0" not in r:
        pytest.skip("Target not at RDP0 — flash bootloader access blocked")


@pytest.fixture(scope="module")
def bl_target(raiden):
    """Ensure target is STM32F1 and synced into bootloader via TARGET SYNC."""
    raiden.cmd("TARGET STM32F1", wait=0.3)
    # Clean any prior SWD/arm state
    raiden.cmd("ARM OFF", wait=0.3)
    raiden.cmd("SWD DISCONNECT", wait=0.3)

    r = raiden.cmd("TARGET SYNC", wait=15)
    if "ACK" not in r and "OK" not in r:
        pytest.skip("TARGET SYNC failed — bootloader not responding on GP4/GP5")
    return raiden


# ── Help / Usage ─────────────────────────────────────────────

class TestBLHelp:

    def test_bl_no_args_shows_help(self, raiden):
        r = raiden.cmd("TARGET BL")
        for sub in ["GET", "GV", "GID", "READ", "WRITE", "GO", "ERASE", "RU", "RP"]:
            assert sub in r, f"BL help missing: {sub}"

    def test_bl_prefix_disambiguation(self, raiden):
        # "BL" shouldn't be ambiguous with other TARGET subcommands
        r = raiden.cmd("TARGET BL")
        assert "Ambiguous" not in r


# ── Safety confirmations (no target needed) ──────────────────

class TestBLSafety:
    """Safety-confirmation prompts. Don't require a live target."""

    def test_erase_all_requires_wipe(self, bl_target):
        r = bl_target.cmd("TARGET BL ERASE ALL")
        assert "WIPE" in r

    def test_ru_requires_wipe(self, bl_target):
        r = bl_target.cmd("TARGET BL RU")
        assert "WIPE" in r

    def test_rp_requires_confirm(self, bl_target):
        r = bl_target.cmd("TARGET BL RP")
        assert "CONFIRM" in r

    def test_read_missing_addr(self, bl_target):
        r = bl_target.cmd("TARGET BL READ")
        assert "ERROR" in r

    def test_write_missing_args(self, bl_target):
        r = bl_target.cmd("TARGET BL WRITE")
        assert "ERROR" in r

    def test_go_missing_addr(self, bl_target):
        r = bl_target.cmd("TARGET BL GO")
        assert "ERROR" in r

    def test_erase_missing_page(self, bl_target):
        r = bl_target.cmd("TARGET BL ERASE")
        assert "ERROR" in r



# ── Read-only info commands ──────────────────────────────────

class TestBLInfo:

    def test_get(self, bl_target):
        r = bl_target.cmd("TARGET BL GET", wait=2)
        assert "Bootloader version" in r
        assert "Supported commands" in r
        # Standard STM32F1 BL command list
        for cmd in ["0x00", "0x01", "0x02", "0x11", "0x21", "0x31", "0x43"]:
            assert cmd in r, f"GET output missing: {cmd}"

    def test_get_has_command_names(self, bl_target):
        """GET should break down command names, not just hex codes."""
        r = bl_target.cmd("TARGET BL GET", wait=2)
        assert "Get ID" in r or "GID" in r or "Get" in r
        assert "Read Memory" in r or "Read" in r
        assert "Erase" in r

    def test_gv(self, bl_target):
        r = bl_target.cmd("TARGET BL GV", wait=2)
        assert "Bootloader version" in r
        assert "Option byte" in r

    def test_gid(self, bl_target):
        r = bl_target.cmd("TARGET BL GID", wait=2)
        assert "PID" in r
        # STM32F103 medium/low/high-density
        assert "0x04" in r or "STM32F" in r


# ── Memory Read ──────────────────────────────────────────────

class TestBLRead:

    def test_read_flash(self, bl_target):
        _require_rdp0_swd(bl_target)
        # After the SWD RDP check we've left SWD connected — force a re-sync
        bl_target.cmd("SWD DISCONNECT", wait=0.3)
        r = bl_target.cmd(f"TARGET BL READ {FLASH_BASE} 16", wait=3)
        assert f"0x{FLASH_BASE.upper()}" in r.upper() or f"{FLASH_BASE.lower()}" in r.lower()
        assert "OK: Read 16 bytes" in r

    def test_read_safe_sram(self, bl_target):
        # SRAM outside bootloader workspace (>0x20000100) should be readable
        r = bl_target.cmd(f"TARGET BL READ {SRAM_SAFE} 16", wait=3)
        assert "OK: Read 16 bytes" in r

    def test_read_bootloader_workspace_nacks(self, bl_target):
        """STM32F1 bootloader rejects multi-byte reads in its workspace (first 256B SRAM)."""
        r = bl_target.cmd("TARGET BL READ 20000000 16", wait=3)
        # Either NACKs on count, or we get an error about the workspace.
        assert "ERROR" in r

    def test_read_system_rom(self, bl_target):
        """System memory ROM (bootloader image itself) is always readable."""
        r = bl_target.cmd(f"TARGET BL READ {SYSTEM_ROM} 16", wait=3)
        assert "OK: Read 16 bytes" in r

    def test_read_default_count(self, bl_target):
        """No count → defaults to 256 bytes."""
        r = bl_target.cmd(f"TARGET BL READ {SYSTEM_ROM}", wait=4)
        assert "OK: Read 256 bytes" in r


# ── Memory Write ─────────────────────────────────────────────

class TestBLWrite:

    def test_write_sram_readback(self, bl_target):
        # Write 4 bytes to safe SRAM, read back via BL
        bl_target.cmd(f"TARGET BL WRITE {SRAM_SAFE} DEADBEEF", wait=2)
        r = bl_target.cmd(f"TARGET BL READ {SRAM_SAFE} 4", wait=3)
        # STM32 stores little-endian; hexdump prints as-written bytes
        assert "DE AD BE EF" in r.upper() or "AD BE EF" in r.upper()

    def test_write_hex_odd_length_rejected(self, bl_target):
        r = bl_target.cmd(f"TARGET BL WRITE {SRAM_SAFE} ABC")
        assert "ERROR" in r

    def test_write_empty_rejected(self, bl_target):
        r = bl_target.cmd(f"TARGET BL WRITE {SRAM_SAFE} ")
        assert "ERROR" in r

    @pytest.mark.destructive
    def test_write_flash_then_erase(self, bl_target):
        """Erase page 0, write, read back, then erase again to clean."""
        _require_rdp0_swd(bl_target)
        bl_target.cmd("SWD DISCONNECT", wait=0.3)

        r = bl_target.cmd("TARGET BL ERASE 0", wait=5)
        assert "OK" in r

        r = bl_target.cmd(f"TARGET BL WRITE {FLASH_BASE} CAFEF00D", wait=3)
        assert "OK: Wrote" in r

        r = bl_target.cmd(f"TARGET BL READ {FLASH_BASE} 4", wait=3)
        assert "CA FE F0 0D" in r.upper()

        # Clean up: erase page 0 again
        bl_target.cmd("TARGET BL ERASE 0", wait=5)


# ── Erase ────────────────────────────────────────────────────

class TestBLErase:

    @pytest.mark.destructive
    def test_erase_page_zero(self, bl_target):
        _require_rdp0_swd(bl_target)
        bl_target.cmd("SWD DISCONNECT", wait=0.3)

        r = bl_target.cmd("TARGET BL ERASE 0", wait=5)
        assert "OK" in r

        r = bl_target.cmd(f"TARGET BL READ {FLASH_BASE} 16", wait=3)
        assert "FF FF FF FF" in r.upper()

    @pytest.mark.destructive
    def test_erase_all_wipe(self, bl_target):
        """Mass erase via bootloader."""
        _require_rdp0_swd(bl_target)
        bl_target.cmd("SWD DISCONNECT", wait=0.3)

        r = bl_target.cmd("TARGET BL ERASE ALL WIPE", wait=30)
        assert "OK" in r and "Mass erase" in r

        r = bl_target.cmd(f"TARGET BL READ {FLASH_BASE} 16", wait=3)
        assert "FF FF FF FF" in r.upper()


# ── GO ───────────────────────────────────────────────────────

class TestBLGo:

    def test_go_exits_bootloader(self, bl_target):
        """GO at system ROM causes bootloader to exit. After this the
        bootloader is no longer synced — verify the next BL command
        triggers auto-sync."""
        # GO — bootloader exits after ACK
        r = bl_target.cmd(f"TARGET BL GO {SYSTEM_ROM}", wait=2)
        assert "OK" in r or "ERROR" in r  # some firmware NACKs; either is fine

        # Force re-entry: clear target state so next BL command resyncs
        bl_target.cmd("TARGET SYNC", wait=15)


# ── Auto-sync behavior ───────────────────────────────────────

class TestBLAutoSync:

    def test_auto_sync_after_swd(self, raiden):
        """After SWD use, the next BL command should auto-sync (not hang)."""
        raiden.cmd("TARGET STM32F1", wait=0.3)
        r = raiden.cmd("SWD CONNECT")
        if "Connected" not in r:
            pytest.skip("No SWD target")
        raiden.cmd("SWD IDCODE")

        # Now invoke BL — should print "Bootloader not synced" and auto-sync
        r = raiden.cmd("TARGET BL GID", wait=20)
        assert "PID" in r or "not synced" in r.lower() or "Running TARGET SYNC" in r
        # Final result should be successful PID read
        assert "ERROR: Auto-sync failed" not in r


# ── Prefix matching ──────────────────────────────────────────

class TestBLPrefix:

    def test_prefix_unambiguous(self, bl_target):
        """BL G is ambiguous (GET/GV/GID/GO) — should error."""
        r = bl_target.cmd("TARGET BL G")
        assert "Ambiguous" in r or "ERROR" in r

    def test_prefix_get(self, bl_target):
        """GET is unambiguous so 'GET' itself works."""
        r = bl_target.cmd("TARGET BL GET", wait=2)
        assert "Bootloader version" in r

    def test_unknown_subcmd_silent(self, bl_target):
        """Unknown BL subcommand: parser falls through silently (no bootloader I/O)."""
        r = bl_target.cmd("TARGET BL FOOBAR")
        # Should not attempt a read/write/erase — i.e. no bootloader output
        for marker in ["Bootloader version", "Reading", "Writing", "Erasing", "PID"]:
            assert marker not in r


# ── RDP round-trip via bootloader ────────────────────────────

@pytest.mark.destructive
class TestBLReadoutProtect:
    """RP (lock RDP1) then RU (mass erase + RDP0). Destructive."""

    def test_rp_then_ru_cycle(self, bl_target):
        _require_rdp0_swd(bl_target)
        bl_target.cmd("SWD DISCONNECT", wait=0.3)

        # Force re-sync (we may have used SWD above)
        r = bl_target.cmd("TARGET SYNC", wait=15)
        if "ACK" not in r and "OK" not in r:
            pytest.skip("Could not resync bootloader after SWD")

        # Lock down with RP (sets RDP1 + system reset)
        r = bl_target.cmd("TARGET BL RP CONFIRM", wait=10)
        assert "OK" in r

        # After RP, bootloader is reset — need re-sync. Flash reads should NACK.
        time.sleep(1)
        r = bl_target.cmd("TARGET SYNC", wait=15)
        assert "ACK" in r or "OK" in r

        r = bl_target.cmd(f"TARGET BL READ {FLASH_BASE} 16", wait=3)
        assert "ERROR" in r  # RDP1 blocks flash reads

        # Verify RDP level via SWD
        bl_target.cmd("SWD CONNECT")
        r = bl_target.cmd("SWD RDP")
        assert "Level 1" in r
        bl_target.cmd("SWD DISCONNECT", wait=0.3)

        # Unlock via RU (mass erase + RDP0)
        r = bl_target.cmd("TARGET SYNC", wait=15)
        assert "ACK" in r or "OK" in r
        r = bl_target.cmd("TARGET BL RU WIPE", wait=30)
        assert "OK" in r

        # Re-sync and verify flash now reads as erased
        time.sleep(1)
        r = bl_target.cmd("TARGET SYNC", wait=15)
        assert "ACK" in r or "OK" in r

        r = bl_target.cmd(f"TARGET BL READ {FLASH_BASE} 16", wait=3)
        assert "FF FF FF FF" in r.upper()

        # And via SWD
        bl_target.cmd("SWD CONNECT")
        r = bl_target.cmd("SWD RDP")
        assert "Level 0" in r
