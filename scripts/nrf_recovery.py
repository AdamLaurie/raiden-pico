#!/usr/bin/env python3
"""
nrf_recovery.py -- post-unlock APPROTECT recovery for nRF52840, branched on the
chip's build/revision code ("Dxx" vs "Fxx or later" split).

After a glitch opens the debug port, the unlock is TRANSIENT: APPROTECT is
re-latched on the next reset/power-cycle. To make debug access persist you must
write the chip's non-volatile config, and HOW you do that depends on the silicon
revision (FICR.INFO.VARIANT, 3rd ASCII char):

  legacy ("Dxx" and earlier, e.g. AAD0 = our practice chip)
      APPROTECT is "disabled" only when UICR.APPROTECT reads 0xFFFFFFFF (erased).
      NVMC can only clear bits, so you can't write 0xFFFFFFFF over an enabled
      value -- you ERASEUICR (erases the UICR page, KEEPS code flash). One step,
      fully handled here (--apply).

  hardened ("Fxx" or later, e.g. AAF0)
      Two parts, and one of them the glitch rig CANNOT do alone:
        1. UICR.APPROTECT = HwDisabled token (persistent, NVMC-programmable), AND
        2. APPROTECT.DISABLE = SwDisable, written by the TARGET'S OWN FIRMWARE on
           EVERY boot -- because (per the Nordic PS note) APPROTECT.DISABLE is a
           volatile register reset by pin/brownout/watchdog reset and System-OFF
           wake. A one-shot SWD write does not survive a reset.
      => This path is PRINT-ONLY here. The rig has no way to re-assert SwDisable
         each boot; that needs a firmware shim flashed to the target (a Reset
         handler that writes APPROTECT.DISABLE), or the device kept powered.
         The exact token/register address vary by variant -- verify against the
         Nordic PS (DIF chapter) for YOUR part before applying.

Default behaviour is a DRY RUN: read the build code + current UICR.APPROTECT,
classify, and PRINT the correct plan. Nothing is written. This is what the
attack scripts call automatically after a dump, so an unattended unlock never
mutates the chip. Pass --apply to actually execute the (legacy-only) ERASEUICR.

Run `./nrf_recovery.py --help` for usage and examples. Importable too:
`from nrf_recovery import run_recovery, parse_hexdump`.
"""
import argparse
from colors import ColorHelpFormatter
import os
import re
import time


def project_dir(name="nrf52840"):
    """Per-target output folder `scripts/<name>/` for logs, dumps and screenshots,
    created on demand so the scripts/ dir stays clean. One folder per target chip."""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), name)
    os.makedirs(d, exist_ok=True)
    return d


def project_path(filename, name="nrf52840"):
    """Full path to `filename` inside the per-target output folder (see project_dir)."""
    return os.path.join(project_dir(name), filename)


def unique_dump_path(label=""):
    """A UNIQUE, target-scoped flash-dump path so a successful dump never overwrites old
    findings: always timestamped; `label` (e.g. 'dongle', 'my_target') becomes a subfolder
    AND a filename tag. Creates the directory. The RAM dump derives as <path>.ram.bin via
    ram_path_for(). Pass an explicit --out to override."""
    import time as _t
    ts = _t.strftime("%Y%m%d_%H%M%S")
    fname = f"nrf52840_{label}_flash_{ts}.bin" if label else f"nrf52840_flash_{ts}.bin"
    p = project_path(os.path.join(label, fname)) if label else project_path(fname)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def resolve_outdir(dest=None):
    """Resolve a destination folder (created on demand):
      None / ""        -> the default project folder (scripts/nrf52840/),
      a bare name      -> scripts/<name>/  (e.g. "pic18"),
      a path           -> that path (relative to cwd, or absolute)."""
    if not dest:
        return project_dir()
    if os.path.isabs(dest) or os.sep in dest or (os.altsep and os.altsep in dest):
        os.makedirs(dest, exist_ok=True)
        return dest
    return project_dir(dest)


# Concise --help description (the full rationale lives in the module docstring).
DESCRIPTION = """\
Post-unlock APPROTECT recovery for nRF52840, branched on the chip's build code
(FICR.INFO.VARIANT): legacy 'Dxx' parts need only ERASEUICR; hardened 'Fxx+'
parts need a HwDisabled token plus a boot-persistent firmware shim the glitch
rig cannot supply (so that path is print-only).

Needs an unlocked AP first (glitch the part, or 'TARGET NRF ERASE' for a
transient unlock); the tool gates on AHB-AP readability, not the STATUS string.
Default is a dry run -- it reads and classifies but writes nothing."""

EXAMPLES = """\
Examples:
  # Dry run (default): classify the part and print the recovery plan; writes nothing.
  ./nrf_recovery.py

  # Same, with the serial port given explicitly (skips auto-detect).
  ./nrf_recovery.py --pico-port /dev/ttyACM0

  # Legacy parts (e.g. AAD0): execute ERASEUICR for a persistent unlock.
  # Code flash is preserved; all other UICR config is erased.
  ./nrf_recovery.py --apply

Notes:
  * --apply is a no-op on hardened (Fxx+) parts: that path is print-only.
  * The attack scripts call this automatically after a flash dump
    (nrf_attack.py honours --apply-recovery; nrf_autopwn.py is always dry-run)."""

# --- register map (nRF52840) ---
FICR_INFO_PART    = 0x10000100   # 0x00052840 on nRF52840 (AHB-AP readability probe)
FICR_INFO_VARIANT = 0x10000104   # 4 ASCII chars, MSB = first letter
UICR_APPROTECT    = 0x10001208   # 0xFFFFFFFF = disabled (legacy)
NVMC_READY        = 0x4001E400   # bit0: 1=idle/ready
NVMC_CONFIG       = 0x4001E504   # 0=ReadOnly 1=Wen(write) 2=Een(erase)
NVMC_ERASEUICR    = 0x4001E514   # write 1 to erase the UICR page

APPROTECT_DISABLED = 0xFFFFFFFF
HWDISABLED_TOKEN   = 0x0000005A  # "HwDisabled"/SwDisable PALL token (hardened parts)
HARDENED_FROM      = 'F'         # revision letter >= 'F' => hardened APPROTECT


# ---------------------------------------------------------------- dump parsing
def parse_hexdump(text):
    """Parse a firmware hex dump into raw bytes.

    Tolerant of both formats the firmware emits:
      TARGET NRF DUMP :  '08000000: DE AD BE EF'      (no 0x, 4 bytes/line)
      SWD READ        :  '0x08000000: DE AD ..  ....' (0x prefix, 16 bytes + ASCII)
    The previous inline parsers keyed on startswith('0x') and so silently
    dropped EVERY NRF-DUMP line -- the unlock dump files came out empty.
    """
    out = bytearray()
    addr_re = re.compile(r'^\s*(?:0x)?[0-9A-Fa-f]{8}:\s*(.*)$')
    for ln in text.splitlines():
        m = addr_re.match(ln)
        if not m:
            continue
        # Hex bytes are single-space separated; the SWD-READ ASCII gutter is set
        # off by 2+ spaces, so the first such segment is exactly the byte run.
        hexrun = re.split(r'\s{2,}', m.group(1).strip())[0]
        for tok in hexrun.split():
            if len(tok) == 2 and all(c in '0123456789abcdefABCDEF' for c in tok):
                out.append(int(tok, 16))
            else:
                break
    return bytes(out)


NRF52840_FLASH_SIZE = 0x100000     # 1 MiB internal code flash @ 0x00000000 (256 pages x 4 KiB)
NRF52840_CODERAM_BASE = 0x00800000 # SRAM mirror on the CODE bus (same 256 KiB as 0x20000000)
NRF52840_FICR_BASE  = 0x10000000   # factory info (PART/VARIANT/DEVICEID...)
NRF52840_UICR_BASE  = 0x10001000   # user config (UICR.APPROTECT @0x10001208)
NRF52840_XIP_BASE   = 0x12000000   # eXecute-In-Place external QSPI flash window (to 0x19FFFFFF).
                                   # ★ On a REAL target the firmware/keys may live HERE, not internal
                                   # flash — dump the actual external-flash size (varies by board).
NRF52840_RAM_BASE   = 0x20000000   # 256 KiB data RAM
NRF52840_RAM_SIZE   = 0x40000
NRF52840_APB_BASE   = 0x40000000   # APB peripherals (instances 0..47, 0x1000 each -> up to 0x4002F000).
NRF52840_APB_SIZE   = 0x30000      # covers all APB instances incl. ECB/CCM/AAR (AES), NVMC/ACL,
                                   # APPROTECT, QSPI (ext-flash iface) — see WORK/nRF52840_instantiation_table.md
NRF52840_AHB_BASE   = 0x50000000   # AHB peripherals: GPIO P0 @0x50000000, P1 @0x50000300
NRF52840_CRYPTOCELL_BASE = 0x5002A000  # ★ CRYPTOCELL 310 security subsystem (AES/ChaCha/HASH/PKA/RNG,
                                       # CC_* engines @0x5002B000) — the crypto/key block, dump on a target
# NOTE: reading peripheral registers over the DAP can have SIDE EFFECTS (read-to-clear
# events, etc.) and unmapped addresses may bus-fault -> dump_region just gets short/0
# there. Dump bounded ranges, not the whole 0x40000000-0x5FFFFFFF space.


def dump_region(p, base, size, out_path, chunk=32768, log=print):
    """Dump `size` bytes from `base` to out_path via `TARGET NRF DUMP`, in CHUNKS
    each streamed until the firmware's 'Dumped' trailer (a single huge DUMP would
    overrun a fixed read wait, so we chunk + stream + show progress). Works for
    flash (base=0) OR RAM (base=0x20000000) — any AHB-readable region. Requires the
    part to be (transiently) unlocked. Returns the number of bytes written (short =
    the dump stalled / the part re-locked)."""
    data = bytearray()
    addr = 0
    while addr < size:
        n = min(chunk, size - addr)
        p.s.reset_input_buffer()
        p.s.write(f"TARGET NRF DUMP 0x{base + addr:08X} {n}\r\n".encode())
        buf = b""; t0 = time.time(); timeout = 20 + n / 2048.0
        while time.time() - t0 < timeout:
            m = p.s.in_waiting
            if m:
                buf += p.s.read(m)
                if b"Dumped" in buf:
                    break
            else:
                time.sleep(0.02)
        d = parse_hexdump(buf.decode("utf-8", "replace"))
        if not d:
            log(f"  dump stalled at 0x{base + addr:08X} (0 bytes parsed) — stopping")
            break
        data.extend(d); addr += len(d)
        if (addr % (128 * 1024)) == 0 or addr >= size:
            log(f"  dumped {addr // 1024}/{size // 1024} KB")
    with open(out_path, "wb") as f:
        f.write(data)
    return len(data)


def dump_flash(p, size, out_path, chunk=32768, log=print):
    """Back-compat shim: dump `size` bytes of flash from 0x0."""
    return dump_region(p, 0, size, out_path, chunk, log)


def ram_path_for(out_path):
    """Derive the RAM dump filename from the flash dump path (foo.bin -> foo.ram.bin)."""
    return (out_path[:-4] if out_path.endswith(".bin") else out_path) + ".ram.bin"


def dump_flash_and_ram(p, out_path, log=print):
    """On unlock, dump the FULL flash (1 MiB @0) to out_path AND the RAM (256 KiB
    @0x20000000) to <out_path w/o .bin>.ram.bin. Returns (flash_bytes, ram_bytes,
    ram_path). RAM holds runtime data / secrets on a live target."""
    log(f"dumping FULL flash ({NRF52840_FLASH_SIZE // 1024} KB) -> {out_path} ...")
    fn = dump_region(p, 0, NRF52840_FLASH_SIZE, out_path, log=log)
    ram_out = ram_path_for(out_path)
    log(f"dumping RAM ({NRF52840_RAM_SIZE // 1024} KB) -> {ram_out} ...")
    rn = dump_region(p, NRF52840_RAM_BASE, NRF52840_RAM_SIZE, ram_out, log=log)
    return fn, rn, ram_out


# ---------------------------------------------------------------- SWD helpers
def read_word(p, addr):
    """Read one 32-bit word via 'SWD READ <addr> 1' (memory is little-endian).
    Returns int or None."""
    b = parse_hexdump(p.cmd(f"SWD READ 0x{addr:08X} 1", 0.6))
    if len(b) < 4:
        return None
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)


def variant_str(word):
    """FICR.INFO.VARIANT word -> 4-char build code, e.g. 0x41414430 -> 'AAD0'."""
    chars = []
    for sh in (24, 16, 8, 0):
        c = (word >> sh) & 0xFF
        chars.append(chr(c) if 32 <= c <= 126 else '?')
    return "".join(chars)


def classify(variant):
    """Return ('legacy'|'hardened', revletter) from the build code."""
    rev = variant[2] if len(variant) >= 3 else '?'
    cls = "hardened" if rev >= HARDENED_FROM else "legacy"
    return cls, rev


def pretty_info(info_text):
    """Annotate a firmware `TARGET NRF INFO` dump with decoded fields so it reads
    nicely and consistently with the recovery output, e.g.:
        INFO.PART:    0x00052840  = nRF52840
        INFO.VARIANT: 0x41414430 = 'AAD0'  (revision 'D', legacy APPROTECT)
        UICR.APPROT:  0xFFFFFF00  = APPROTECT enabled (locked value; a glitch unlock
                                    leaves this set = clean skip, not UICR corruption)
    Unrecognised lines pass through unchanged. Returns the annotated multi-line string."""
    lines = []
    for ln in info_text.splitlines():
        s = ln.rstrip()
        if not s:
            continue
        m = re.search(r'INFO\.PART:\s*0x0*([0-9A-Fa-f]+)', s)
        if m and int(m.group(1), 16) == 0x52840:
            s += "  = nRF52840"
        m = re.search(r'INFO\.VARIANT:\s*0x([0-9A-Fa-f]{8})', s)
        if m:
            v = variant_str(int(m.group(1), 16))
            cls, rev = classify(v)
            s += f" = '{v}'  (revision '{rev}', {cls} APPROTECT)"
        m = re.search(r'UICR\.APPROT\w*\s*:?\s*0x([0-9A-Fa-f]{8})', s)
        if m:
            u = int(m.group(1), 16)
            s += ("  = unprotected (UICR erased)" if (u & 0xFF) == 0xFF else
                  "  = APPROTECT enabled (locked value; a glitch unlock leaves this set "
                  "= clean skip, not UICR corruption)")
        lines.append(s)
    return "\n".join(lines)


# ---------------------------------------------------------------- recovery
def _nvmc_wait_ready(p, log, timeout_s=2.0):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        v = read_word(p, NVMC_READY)
        if v is not None and (v & 1):
            return True
        time.sleep(0.02)
    log("  WARN: NVMC.READY did not assert within %.1fs" % timeout_s)
    return False


def _erase_uicr(p, log):
    """Legacy persistent-unlock: ERASEUICR (keeps code flash). Returns True on
    UICR.APPROTECT == 0xFFFFFFFF afterwards."""
    log("  NVMC.CONFIG = Een (erase-enable)")
    p.cmd(f"SWD WRITE 0x{NVMC_CONFIG:08X} 2", 0.4)
    _nvmc_wait_ready(p, log)
    log("  NVMC.ERASEUICR = 1 (erase UICR page; code flash untouched)")
    p.cmd(f"SWD WRITE 0x{NVMC_ERASEUICR:08X} 1", 0.4)  # write-only reg: verify-fail msg is expected
    _nvmc_wait_ready(p, log)
    log("  NVMC.CONFIG = ReadOnly")
    p.cmd(f"SWD WRITE 0x{NVMC_CONFIG:08X} 0", 0.3)
    val = read_word(p, UICR_APPROTECT)
    if val is None:
        log("  ERROR: could not read UICR.APPROTECT back")
        return False
    ok = (val == APPROTECT_DISABLED)
    log(f"  UICR.APPROTECT = 0x{val:08X} -> {'DISABLED (persistent)' if ok else 'still set!'}")
    return ok


def run_recovery(p, apply=False, log=print):
    """Classify the part and print (or, for legacy + apply, execute) the recovery
    plan. `p` is any object with .cmd(str, wait_s)->str (the attack scripts' Pico).
    Returns the class string ('legacy'|'hardened'|'unknown')."""
    log("\n--- APPROTECT recovery (build-code branch) ---")
    vw = read_word(p, FICR_INFO_VARIANT)
    if vw is None:
        log("ERROR: cannot read FICR.INFO.VARIANT -- is the part actually unlocked?")
        return "unknown"
    variant = variant_str(vw)
    cls, rev = classify(variant)
    uicr = read_word(p, UICR_APPROTECT)
    log(f"FICR.INFO.VARIANT : 0x{vw:08X} = '{variant}'  (revision '{rev}')")
    log(f"UICR.APPROTECT    : " + ("0x%08X" % uicr if uicr is not None else "unreadable"))
    log(f"Class             : {cls.upper()}")

    if cls == "legacy":
        if uicr == APPROTECT_DISABLED:
            log("UICR.APPROTECT already 0xFFFFFFFF (erased) -> legacy 'disabled' state; "
                "no NVMC write needed. Confirm by power-cycling and re-reading: a "
                "legacy part should then boot UNPROTECTED. (If it re-locks, the AP was "
                "only transiently open -- e.g. post-ERASEALL -- and the glitch, not "
                "this helper, is what must persist.)")
            return cls
        log("Plan (legacy / 'Dxx'): ERASEUICR -> UICR.APPROTECT becomes 0xFFFFFFFF "
            "(disabled). Code flash is preserved; all other UICR config is cleared.")
        if not apply:
            log("DRY RUN: pass --apply (or --apply-recovery to the attack script) to "
                "execute. Re-run leaves the chip untouched.")
            return cls
        log("APPLYING ERASEUICR ...")
        ok = _erase_uicr(p, log)
        log("RESULT: " + ("UICR cleared -- power-cycle should now boot UNPROTECTED."
                          if ok else "FAILED -- UICR not disabled; inspect above."))
        return cls

    # hardened
    log("Plan (hardened / 'Fxx or later') -- PRINT ONLY, the rig cannot fully persist this:")
    log(f"  1. (persistent) NVMC Wen -> write UICR.APPROTECT = 0x{HWDISABLED_TOKEN:08X} "
        "(HwDisabled token; programmable from erased 0xFFFFFFFF).")
    log("  2. (this session) write APPROTECT.DISABLE = 0x5A (SwDisable) over SWD.")
    log("  3. (REQUIRED for persistence) flash a target firmware shim whose Reset "
        "handler re-writes APPROTECT.DISABLE = 0x5A on EVERY boot -- it is volatile "
        "(reset by pin/brownout/watchdog reset + System-OFF wake). Without that the "
        "part re-locks on reset.")
    log("  NOTE: token value + APPROTECT register address vary by variant; verify "
        "against the Nordic PS (DIF chapter) for this part before writing.")
    log("  Not auto-applied: unverifiable on this (legacy) bench and step 3 is out of "
        "the glitch rig's scope.")
    return cls


# ---------------------------------------------------------------- standalone
def main():
    ap = argparse.ArgumentParser(
        description=DESCRIPTION, epilog=EXAMPLES,
        formatter_class=ColorHelpFormatter)
    ap.add_argument("--pico-port", metavar="DEV", default=None,
                    help="Raiden Pico CDC serial port (default: auto-detect /dev/ttyACM*)")
    ap.add_argument("--apply", action="store_true",
                    help="execute the recovery instead of a dry run "
                         "(legacy parts only: ERASEUICR; ignored on hardened parts)")
    a = ap.parse_args()

    from nrf_attack import Pico, find_port   # reuse the robust serial helpers
    port = find_port(a.pico_port)
    print(f"[pico] {port}")
    p = Pico(port)
    try:
        p.cmd("TARGET NRF52840", 0.4)
        print(p.cmd("TARGET NRF STATUS", 0.8).strip())
        # Gate on actual AHB-AP READABILITY, not the STATUS string: ERASEALL leaves
        # the AP transiently open while CTRL-AP.APPROTECTSTATUS can still read
        # PROTECTED, so trusting STATUS would wrongly refuse a working unlock.
        part = read_word(p, FICR_INFO_PART)
        if part is None or (part & 0x000FFFFF) != 0x52840:
            print(f"AHB-AP not readable (FICR.INFO.PART="
                  f"{'None' if part is None else '0x%08X' % part}). Recovery needs an "
                  "unlocked AP -- glitch first, or TARGET NRF ERASE for a transient "
                  "unlock. Aborting.")
            return
        run_recovery(p, apply=a.apply)
    finally:
        p.close()


if __name__ == "__main__":
    main()
