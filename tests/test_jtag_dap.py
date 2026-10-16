"""Config NONE: ADIv6 JTAG-DP commands (DPINIT/CTRLSTAT/ABORT/DEVICEEN/APREAD/
MEMREAD/MEMWRITE/MEMDUMP).

Wiring: None required. These assert the commands are registered, dispatch, and
error explicitly on bad input. The happy path (a real memory readback) needs a
live ADIv6 JTAG-DP target with memory access enabled (CSW DeviceEn=1), so it is
not a unit test — these cover registration + the error/validation paths, which
need no target.
"""

import pytest


class TestJtagDapRegistration:

    def test_jtag_usage_lists_dap_commands(self, raiden):
        """Bare JTAG prints usage including the new sub-commands."""
        r = raiden.cmd("JTAG", wait=2)
        for sub in ["DPINIT", "CTRLSTAT", "ABORT", "DEVICEEN", "APREAD",
                    "MEMREAD", "MEMWRITE", "MEMDUMP"]:
            assert sub in r, f"JTAG usage missing: {sub}"

    def test_help_lists_deviceen(self, raiden):
        """HELP's JTAG section advertises the DEVICEEN command."""
        r = raiden.cmd("HELP", wait=5)
        assert "DEVICEEN" in r, "HELP missing JTAG DEVICEEN"

    def test_unknown_jtag_subcommand_errors(self, raiden):
        r = raiden.cmd("JTAG WIBBLE", wait=2)
        assert "ERROR" in r
        assert "Unknown" in r


class TestJtagDapDispatch:
    """Each command must give an explicit, well-formed response (never a silent
    no-op) even with no target attached."""

    def test_dpinit_responds(self, raiden):
        r = raiden.cmd("JTAG DPINIT", wait=3)
        # Either the DP came up (OK + TAP IDCODE) or it explicitly failed.
        assert ("OK: TAP IDCODE" in r) or ("DP init failed" in r), r

    def test_deviceen_responds(self, raiden):
        r = raiden.cmd("JTAG DEVICEEN", wait=3)
        # Either a reading (DEVICEEN=0/1) or an explicit read failure.
        assert ("DEVICEEN=" in r) or ("CSW read failed" in r), r


class TestJtagDapArgErrors:

    def test_apread_missing_args_errors(self, raiden):
        r = raiden.cmd("JTAG APREAD", wait=2)
        assert "ERROR" in r and "Usage" in r

    def test_apread_one_arg_errors(self, raiden):
        r = raiden.cmd("JTAG APREAD 0x7c004000", wait=2)
        assert "ERROR" in r and "Usage" in r

    def test_apread_bad_arg_errors(self, raiden):
        r = raiden.cmd("JTAG APREAD notahex 0xd00", wait=2)
        assert "ERROR" in r

    def test_memread_missing_addr_errors(self, raiden):
        r = raiden.cmd("JTAG MEMREAD", wait=2)
        assert "ERROR" in r and "Usage" in r

    def test_memread_bad_addr_errors(self, raiden):
        r = raiden.cmd("JTAG MEMREAD notahex", wait=2)
        assert "ERROR" in r

    def test_deviceen_bad_ap_errors(self, raiden):
        r = raiden.cmd("JTAG DEVICEEN notahex", wait=2)
        assert "ERROR" in r

    def test_memwrite_missing_args_errors(self, raiden):
        r = raiden.cmd("JTAG MEMWRITE 0x20000000", wait=2)
        assert "ERROR" in r and "Usage" in r

    def test_memwrite_bad_val_errors(self, raiden):
        r = raiden.cmd("JTAG MEMWRITE 0x20000000 notahex", wait=2)
        assert "ERROR" in r

    def test_memdump_missing_args_errors(self, raiden):
        r = raiden.cmd("JTAG MEMDUMP 0x20000000", wait=2)
        assert "ERROR" in r and "Usage" in r

    def test_memdump_zero_words_errors(self, raiden):
        r = raiden.cmd("JTAG MEMDUMP 0x20000000 0", wait=2)
        assert "ERROR" in r

    def test_memdump_too_many_words_errors(self, raiden):
        r = raiden.cmd("JTAG MEMDUMP 0x20000000 99999", wait=2)
        assert "ERROR" in r
