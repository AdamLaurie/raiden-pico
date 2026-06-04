/*
 * EFM32 Leopard Gecko (EFM32LG) target support for Raiden-Pico.
 *
 * Implements the Silicon Labs EFM32 debug-unlock bypass via voltage fault
 * injection, plus SWD flash readout once unlocked.
 *
 * Attack model (see WORK/efm32_findings.md, target EFM32LG):
 *   The EFM32 protects debug with a single Debug Lock Word (DLW) in the flash
 *   info-block lock-bits page. When locked, the SW-DP connects but the AHB-AP
 *   (memory access) is disabled, so flash cannot be read. The only "official"
 *   recovery is the Authentication Access Port (AAP) DEVICEERASE, which mass-
 *   erases the part - it can never read. The glitch corrupts the DLW latch
 *   evaluated during the fixed tRESET ~163 us boot window so the AHB-AP comes
 *   up ENABLED; flash is then read over standard SWD WITHOUT triggering the
 *   AAP erase. This is the EFM32 analogue of the nRF52 APPROTECT bypass and
 *   the STM32 RDP1->RDP0 readout glitch.
 *
 * Built entirely on the vendor-neutral swd_* API in swd.h:
 *   - AHB-AP  (AP index 0) for flash / SRAM / Device-Info (DI) page reads
 *   - AAP     (custom AP)  for DEVICEERASE recovery (destructive)
 *
 * GROUND TRUTH for "unlocked" = the AHB-AP can actually read the DI page. On a
 * debug-locked EFM32 the AHB-AP transaction faults; mirroring the nRF module's
 * FICR-read truth (do NOT trust a status bit alone).
 *
 * Inject point (crowbar): the module DECOUPLE pin (internal ~1.8 V core LDO
 * output), NOT main VDD. Trigger model: power-cycle (or nRST-reboot) the target,
 * wait into the early-boot DLW-latch window, then pulse the crowbar MOSFET
 * (PIN_GLITCH_OUT / GP2). Identical rig to the nRF52840 DEC1 crowbar.
 */

#ifndef EFM32_TARGET_H
#define EFM32_TARGET_H

#include <stdint.h>
#include <stdbool.h>

// --- SWD identity ---
// Generic ARM Cortex-M3 SW-DP IDCODE. NOTE: this is shared by many M3 parts
// (STM32F1, LPC17xx, ...), so DPIDR alone does NOT confirm an EFM32 - the
// Device-Info (DI) PART register read over the AHB-AP is the positive ID.
#define EFM32_DPIDR_M3         0x2BA01477u

// Crowbar pulse-width band, in 6.67 ns cycles @150 MHz (same rig as nRF DEC1).
// Lower ~67 ns: a shallow droop that dents the core rail and faults the single
// DLW-latch instruction without a full brownout. Upper ~3 us: beyond this the
// DECOUPLE rail collapses far enough that the BOD latches a clean reset (which
// just aborts the attempt - EFM32 has NO glitch-triggered mass-erase hazard,
// unlike the nRF, so the cap is about avoiding wasted clean resets, not erase).
#define EFM32_GLITCH_WIDTH_MIN_CYC 10u    // ~67 ns
#define EFM32_GLITCH_WIDTH_MAX_CYC 450u   // ~3.0 us

// --- Memory map (EFM32LG) ---
#define EFM32_FLASH_BASE       0x00000000u
#define EFM32_SRAM_BASE        0x20000000u

// Device Information (DI) page - factory-programmed, read-only, always present
// when the AHB-AP is enabled. Reading it is the authoritative unlock test.
#define EFM32_DI_BASE          0x0FE08000u
#define EFM32_DI_UNIQUEL       0x0FE081F0u  // unique ID, low word
#define EFM32_DI_UNIQUEH       0x0FE081F4u  // unique ID, high word
#define EFM32_DI_MSIZE         0x0FE081F8u  // FLASH kB [15:0], SRAM kB [31:16]
#define EFM32_DI_PART          0x0FE081FCu  // PARTNUMBER [15:0], FAMILY [23:16], PRODREV [31:24]

// PART register field extractors
#define EFM32_PART_NUMBER(p)   ((p) & 0xFFFFu)
#define EFM32_PART_FAMILY(p)   (((p) >> 16) & 0xFFu)
#define EFM32_PART_PRODREV(p)  (((p) >> 24) & 0xFFu)
#define EFM32_FAMILY_LG        16u           // Leopard Gecko family code (informational; not gated on)

// MSIZE field extractors (sizes in kB)
#define EFM32_MSIZE_FLASH_KB(m) ((m) & 0xFFFFu)
#define EFM32_MSIZE_SRAM_KB(m)  (((m) >> 16) & 0xFFFFu)

// Lock-bits page (info block). The DLW (Debug Lock Word) lives here; the exact
// word offset is part/RM specific - not needed for the glitch (we test the
// AHB-AP read directly), documented for the host tooling only.
#define EFM32_LOCKBITS_BASE    0x0FE04000u

// --- Authentication Access Port (AAP) ---
// The AAP is a custom debug AP, exposed only in the reset window, whose ONLY
// useful command is a full device erase. Register offsets follow OpenOCD's
// efm32 driver (AAP register space). The AAP's AP index is discovered at
// runtime by matching AAP_IDR (it is NOT a fixed index across all parts), so
// efm32_device_erase() probes for it rather than hard-coding one.
#define EFM32_AAP_CMD          0x00u   // bit0 DEVICEERASE, bit1 SYSRESETREQ
#define EFM32_AAP_CMDKEY       0x04u   // write EFM32_AAP_CMDKEY_VAL to enable CMD
#define EFM32_AAP_STATUS       0x08u   // bit0 ERASEBUSY
#define EFM32_AAP_IDR          0xFCu   // identification register
#define EFM32_AAP_CMDKEY_VAL   0xCFACC118u
#define EFM32_AAP_DEVICEERASE  (1u << 0)
#define EFM32_AAP_ERASEBUSY    (1u << 0)
// Known AAP IDR. ADIv5 IDR carries a revision in bits[31:28] and an AP
// class/variant in the low 12 bits, both of which vary; match only the stable
// designer/identity field (published EFM32 AAP IDR 0x16E60001 -> identity 0x6E60).
// efm32_aap_probe() always prints the raw IDRs so the operator can confirm the
// exact value for a given part against the EFM32LG Reference Manual.
#define EFM32_AAP_IDR_MASK     0x0FFFF000u
#define EFM32_AAP_IDR_VAL      0x06E60000u

typedef struct {
    uint32_t dpidr;
    uint8_t  state;        // 0 none, 1 connected+locked, 2 connected+unlocked
    uint32_t part;         // DI PART register
    uint32_t part_number;
    uint32_t part_family;
    uint32_t prod_rev;
    uint32_t msize;        // DI MSIZE register
    uint32_t flash_kb;
    uint32_t sram_kb;
    uint32_t unique_l;
    uint32_t unique_h;
} efm32_info_t;

// Connect over SWD and read the DP ID. Does NOT require the AHB-AP (works while
// the part is debug-locked). Returns DPIDR (EFM32_DPIDR_M3 on a Gecko, 0 on fail).
uint32_t efm32_connect(void);

// GROUND TRUTH unlock test: can the AHB-AP read the DI PART register? On a
// locked part the AHB-AP faults and this returns false. Returns true only if a
// plausible (non-blank) DI PART word reads back.
bool efm32_is_unlocked(void);

// Populate info from the DI page. DI reads require an unlocked part. Returns
// true if connected; fills i->state (0/1/2).
bool efm32_read_info(efm32_info_t *i);

// Print INFO to the CLI (connect + lock state + DI page if readable).
void efm32_cmd_info(void);

// Probe AP indices 0..3 for the AAP (by IDR) and print what's found. Useful to
// confirm the AAP is reachable before attempting a DEVICEERASE recovery.
void efm32_aap_probe(void);

// AAP DEVICEERASE recovery: connect under reset, find the AAP, mass-erase the
// device (main flash + lock bits -> debug unlocked, but ALL firmware gone).
// Destructive. Returns true if the part is UNLOCKED (AHB-AP readable) afterwards.
bool efm32_device_erase(void);

// Single shot for scope work: power-cycle the target, wait `delay_us`, fire one
// crowbar pulse of `width_cyc` on GP2->DECOUPLE. No SWD probe. The GP22
// GLITCH_FIRED marker pulses - use it as the scope trigger while probing DECOUPLE.
void efm32_glitch_shot(uint32_t delay_us, uint32_t width_cyc);

// nRST-reboot single shot (VDD stays on): reboot via nRST(GP15), wait `delay_us`
// from the release edge, fire one crowbar pulse. Trigger the scope on GP15.
void efm32_glitch_shot_reset(uint32_t delay_us, uint32_t width_cyc, uint32_t reset_hold_us);

// Dump `bytes` of target memory from `addr` over the AHB-AP as a hex dump.
// Requires an unlocked part. Returns bytes actually read.
uint32_t efm32_dump_mem(uint32_t addr, uint32_t bytes);

// Debug-unlock voltage-glitch 2D sweep (delay x width), POWER-CYCLE entry.
// Mirrors nrf_glitch_approtect: width inner / delay outer, crowbar on
// GP2->DECOUPLE, pre-flight abort if the part is already open. delay is from
// power-on; centre it on the tRESET ~163 us boot window. Returns true on unlock.
bool efm32_glitch_unlock(uint32_t delay_start_us, uint32_t delay_end_us,
                         uint32_t delay_step_us, uint32_t width_start_cyc,
                         uint32_t width_end_cyc, uint32_t width_step_cyc,
                         uint32_t power_off_ms, uint32_t settle_ms,
                         uint32_t max_attempts);

// RESET-based variant: VDD stays on, reboot via nRST(GP15) each attempt. delay
// is from the nRST RELEASE edge; reset_hold_us is the nRST low duration.
bool efm32_glitch_unlock_reset(uint32_t delay_start_us, uint32_t delay_end_us,
                               uint32_t delay_step_us, uint32_t width_start_cyc,
                               uint32_t width_end_cyc, uint32_t width_step_cyc,
                               uint32_t reset_hold_us, uint32_t settle_ms,
                               uint32_t max_attempts);

#endif // EFM32_TARGET_H
