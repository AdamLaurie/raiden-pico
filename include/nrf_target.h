/*
 * nRF52840 target support for Raiden-Pico
 *
 * Implements the Nordic nRF52 APPROTECT (readback protection) bypass via
 * voltage fault injection, plus SWD-based flash readout once unlocked.
 *
 * Attack reference: LimitedResults "nRF52 Debug Resurrection (APPROTECT bypass)"
 * and atc1441/ESP32_nRF52_SWD. The glitch corrupts the boot-time APPROTECT
 * latch so the AHB-AP (debug memory access) stays enabled; flash is then read
 * over standard SWD.
 *
 * Built entirely on the vendor-neutral swd_* API in swd.h:
 *   - CTRL-AP (AP index 1) for APPROTECTSTATUS / RESET / ERASEALL
 *   - AHB-AP  (AP index 0) for FICR / UICR / flash memory reads
 *
 * Inject point (crowbar): the module DEC1 pin (~1.3V core regulator output),
 * NOT main VDD. Trigger model: power-cycle the target, wait into the early-boot
 * APPROTECT window, then pulse the crowbar MOSFET (PIN_GLITCH_OUT / GP2).
 */

#ifndef NRF_TARGET_H
#define NRF_TARGET_H

#include <stdint.h>
#include <stdbool.h>

// --- SWD identity ---
#define NRF_DPIDR              0x2BA01477u  // nRF52 series Debug Port ID

// Crowbar pulse-width band for nRF52840, in 6.67ns cycles @150MHz.
// UPPER bound kept at ~3 us: a longer pulse risks the boot disturbance latching
// a mass-erase (NVMC ERASEALL) instead of a clean APPROTECT-readback fault.
// LOWER bound dropped to ~67 ns: a full 1 us crowbar fully collapses DEC1 (C4
// 100nF still fitted) and just cleanly RESETS the part (0 disrupts/0 unlock over
// 7k attempts). atc1441's working glitch is a SHALLOW partial droop, not a
// collapse, so we need sub-us pulses that only dent the rail and corrupt the
// single APPROTECT-readback instruction without browning the core out.
#define NRF_GLITCH_WIDTH_MIN_CYC 10u   // ~67 ns
#define NRF_GLITCH_WIDTH_MAX_CYC 450u  // ~3.0 us

// --- CTRL-AP (AP index 1) register offsets ---
#define NRF_CTRL_AP            1
#define NRF_CTRLAP_RESET            0x00  // soft reset request
#define NRF_CTRLAP_ERASEALL         0x04  // mass erase trigger
#define NRF_CTRLAP_ERASEALLSTATUS   0x08  // erase-in-progress
#define NRF_CTRLAP_APPROTECTSTATUS  0x0C  // bit0: 1 = unprotected (debug allowed)
#define NRF_CTRLAP_IDR              0xFC

// --- FICR (Factory Information, read via AHB-AP) ---
#define NRF_FICR_CODEPAGESIZE  0x10000010
#define NRF_FICR_CODESIZE      0x10000014  // in pages
#define NRF_FICR_DEVICEID0     0x10000060
#define NRF_FICR_DEVICEID1     0x10000064
#define NRF_FICR_INFO_PART     0x10000100  // e.g. 0x52840
#define NRF_FICR_INFO_VARIANT  0x10000104
#define NRF_FICR_INFO_PACKAGE  0x10000108

// --- UICR ---
#define NRF_UICR_APPROTECT     0x10001208  // 0xFFFFFFFF = unprotected

// --- NVMC (flash controller) ---
#define NRF_NVMC_READY         0x4001E400
#define NRF_NVMC_CONFIG        0x4001E504

// nRF connection state mirrors atc1441: 0=no link, 1=locked, 2=unlocked
typedef struct {
    uint32_t dpidr;
    uint8_t  state;        // 0 none, 1 connected+locked, 2 connected+unlocked
    uint32_t info_part;
    uint32_t info_variant;
    uint32_t info_package;
    uint32_t codepage_size;
    uint32_t codesize;
    uint32_t flash_size;   // codepage_size * codesize
    uint32_t uicr_approtect;
} nrf_info_t;

// Connect over SWD and read the DP ID. Does NOT require AHB-AP (works while
// the part is locked). Returns DPIDR (NRF_DPIDR on success, 0 on failure).
uint32_t nrf_connect(void);

// Read CTRL-AP APPROTECTSTATUS. Returns true if the debug interface is
// currently UNPROTECTED (bit0 set) - i.e. a glitch succeeded or part is open.
bool nrf_is_unlocked(void);

// Populate info (FICR/UICR). FICR/UICR reads require an unlocked part.
// Returns true if connected. Fills i->state accordingly.
bool nrf_read_info(nrf_info_t *i);

// Print INFO to the CLI (connect + lock state + FICR/UICR if readable).
void nrf_cmd_info(void);

// Soft-reset the target via CTRL-AP.
void nrf_soft_reset(void);

// CTRL-AP ERASEALL recovery: mass-erase flash+UICR and (on a recoverable part)
// re-open debug. Destructive. Returns true if the part is UNPROTECTED afterwards.
bool nrf_erase_all(void);

// Single shot for scope work: power-cycle the target, wait `delay_us`, fire one
// crowbar pulse of `width_cyc` (clamped to the 1-3us safe band). No SWD probe.
// The GP22 GLITCH_FIRED marker pulses on the shot - use it as the scope trigger
// while probing DEC1. Lets the host park the scope and capture the droop.
void nrf_glitch_shot(uint32_t delay_us, uint32_t width_cyc);

// RESET-boot single shot for fault-mapping the nRST window (scope marker survival).
// Reboots via nRST (VDD stays on), waits delay_us from the release edge, fires one
// crowbar pulse. No SWD probe. Trigger the scope on the GP15 release edge.
void nrf_glitch_shot_reset(uint32_t delay_us, uint32_t width_cyc, uint32_t reset_hold_us);

// Dump `bytes` of target memory starting at `addr` over AHB-AP as a hex dump.
// Requires an unlocked part. Returns bytes actually read.
uint32_t nrf_dump_mem(uint32_t addr, uint32_t bytes);

// APPROTECT voltage-glitch 2D sweep (delay x width).
//   delay_start_us..delay_end_us : crowbar delay after power-on (outer sweep)
//   delay_step_us                : sweep increment for the delay
//   width_start_cyc..width_end_cyc : crowbar pulse width range (inner sweep),
//                                  6.67ns cycles @150MHz; clamped to the safe
//                                  1-3 us band [NRF_GLITCH_WIDTH_MIN/MAX_CYC]
//   width_step_cyc               : sweep increment for the width
//   power_off_ms                 : target off-time per power cycle
//   settle_ms                    : wait after glitch before the SWD probe
//   max_attempts                 : total power-cycle attempts before giving up
// Width is swept inner / delay outer: each delay is tried across all widths
// before the delay advances. Returns true on unlock (SWD left connected).
bool nrf_glitch_approtect(uint32_t delay_start_us, uint32_t delay_end_us,
                          uint32_t delay_step_us, uint32_t width_start_cyc,
                          uint32_t width_end_cyc, uint32_t width_step_cyc,
                          uint32_t power_off_ms, uint32_t settle_ms,
                          uint32_t max_attempts);

// RESET-based variant: VDD stays on, reboot via nRST (GP15) each attempt. `delay_us`
// is from the nRST RELEASE edge; `reset_hold_us` is the nRST low duration. De-jitters
// the boot (clean reset edge, no rail ramp) without a stiff PSU.
//
// `via_ctrlap`: reboot over SWD via CTRL-AP RESET instead of the GP15 nRST pin. Needs
// NO physical reset pad (CTRL-AP is reachable even when APPROTECT-locked) — useful when
// the board's nRST is unfindable. Still a WARM reset (VDD/DEC1 stay up), so it inherits
// the same coupling limit as the nRST variant; the release edge is also SWD-jittered.
bool nrf_glitch_approtect_reset(uint32_t delay_start_us, uint32_t delay_end_us,
                                uint32_t delay_step_us, uint32_t width_start_cyc,
                                uint32_t width_end_cyc, uint32_t width_step_cyc,
                                uint32_t reset_hold_us, uint32_t settle_ms,
                                uint32_t max_attempts, bool via_ctrlap);

#endif // NRF_TARGET_H
