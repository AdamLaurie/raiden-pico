/*
 * PIC18 (classic) target support for Raiden-Pico - ICSP code-protect (CP)
 * bypass via voltage fault injection.
 *
 * PIC18 has no SWD and no UART ROM bootloader: program memory is read over
 * ICSP (bit-banged PGC/PGD + MCLR). When the CONFIG5L code-protect bits are
 * set, an ICSP Table Read of a protected block returns 0x00. The attack faults
 * the read-gating logic during the table-read clock-out so the true byte is
 * returned instead of 0x00 - the optical/UV analogue is bunnie's 18F1320 break,
 * this is the non-invasive transient version (crowbar on VDD).
 *
 * Entry: LVP (clock the 32-bit "MCHP" key while raising MCLR to VDD; requires
 * the part's LVP config bit = 1) so only GPIO is needed - no 9V VPP driver.
 *
 * Reference: PIC18FX220/X320 Flash Programming Spec (DS39592E) and PIC18F4320
 * datasheet (DS39599C), both in salto/WORK/. Verify the exact part's spec
 * before trusting opcodes: PIC18-Q (NVMREG) parts use a different scheme.
 *
 * WARNING: bit-level ICSP timing here is per-spec but unscoped - validate on a
 * known-UNPROTECTED part first (read DEVID + an open block) before trusting a
 * glitched dump, exactly as the nRF module was brought up against a scope.
 */
#ifndef PIC18_TARGET_H
#define PIC18_TARGET_H

#include <stdint.h>
#include <stdbool.h>

// --- ICSP pins (reuse the SWD header: PGC<-SWCLK GP17, PGD<-SWDIO GP18) ---
#define PIC_PGC_PIN    17    // ICSP clock  (target PGC / RB6)
#define PIC_PGD_PIN    18    // ICSP data   (target PGD / RB7)
#define PIC_MCLR_PIN   15    // MCLR/VPP - LVP drives this to VDD (shared nRST)
#define PIC_PGM_PIN    19    // RB5/PGM - held HIGH for PGM-pin LVP (F4321 family)

// LVP entry key, clocked MSB-first on PGD: 'M','C','H','P'
#define PIC_LVP_KEY    0x4D434850u

// LVP entry method. The FX220/X320 family (DS39592) enters by clocking the 32-bit
// "MCHP" key while MCLR rises. The PIC18F4321 family (DS39687) instead enters
// single-supply ICSP by holding RB5/PGM HIGH while MCLR rises - no key is clocked.
// Pick the right one for the silicon: a key-clock entry will NOT connect an F4321
// (you'll read NOT CONNECTED), and vice versa. Default is the key sequence to keep
// the original FX320 behaviour; switch at runtime via `TARGET PIC LVP PGM`.
typedef enum {
    PIC_LVP_MODE_KEY = 0,   // FX220/X320: clock the "MCHP" key (PGM pin unused)
    PIC_LVP_MODE_PGM,       // F4321 family: hold RB5/PGM high, no key
} pic18_lvp_t;

// Select the LVP entry method used by pic18_connect(). Persists until changed.
void pic18_set_lvp(pic18_lvp_t mode);

// 4-bit ICSP commands (clocked MSB-first per DS39592 Table 2-4)
#define ICSP_CORE_INST      0x0   // shift in 16-bit PIC18 instruction, execute
#define ICSP_TBLRD          0x8   // table read
#define ICSP_TBLRD_POSTINC  0x9   // table read, post-increment TBLPTR
#define ICSP_TBLRD_POSTDEC  0xA   // table read, post-decrement TBLPTR
#define ICSP_TBLRD_PREINC   0xB   // table read, pre-increment TBLPTR

// Configuration / ID space (TBLPTR byte addresses)
#define PIC_DEVID1_ADDR     0x3FFFFE   // DEVID1 (low),  DEVID2 at 0x3FFFFF
#define PIC_CONFIG_BASE     0x300000
#define PIC_CONFIG5L_ADDR   0x300008   // CP0..CP3 code-protect bits

// CONFIG5L bit positions (0 = protected). CP0..CP3 protect 8KB blocks.
#define PIC_CP0  (1u << 0)
#define PIC_CP1  (1u << 1)
#define PIC_CP2  (1u << 2)
#define PIC_CP3  (1u << 3)

// Safe crowbar pulse-width band, in 6.67ns cycles @150MHz. Keep narrow: an
// over-wide pulse can disturb the NVM controller into an erase instead of a
// clean read-gate fault. Tune against the target on a scope.
#define PIC_GLITCH_WIDTH_MIN_CYC  15u    // ~0.1 us
#define PIC_GLITCH_WIDTH_MAX_CYC  450u   // ~3.0 us

typedef struct {
    uint16_t devid;        // DEVID2:DEVID1 (part + revision)
    uint8_t  cfg5l;        // CONFIG5L - code-protect bits
    uint8_t  state;        // 0 no link, 1 linked+protected, 2 linked+readable
} pic18_info_t;

// Enter ICSP (LVP key sequence), read DEVID. Returns DEVID (0 on failure).
uint16_t pic18_connect(void);

// Release ICSP: drop MCLR, park PGC/PGD low.
void pic18_disconnect(void);

// Read CONFIG5L; true if at least one CP block is protected (any CP bit == 0).
bool pic18_is_protected(void);

// Populate info (DEVID + CONFIG5L). Returns true if a link was established.
bool pic18_read_info(pic18_info_t *i);

// Print INFO (DEVID, decoded CP state) to the CLI.
void pic18_cmd_info(void);

// Read `bytes` of program memory from `addr` over ICSP as a hex dump. On a
// protected part the protected blocks come back as 0x00. Returns bytes read.
uint32_t pic18_dump_mem(uint32_t addr, uint32_t bytes);

// Single scope shot: power-cycle, wait delay_us, fire one crowbar pulse of
// width_cyc (clamped to the safe band). Pulses GP22 GLITCH_FIRED as the scope
// trigger. No ICSP probe - for parking the scope on VDD droop.
void pic18_glitch_shot(uint32_t delay_us, uint32_t width_cyc);

// CP-bypass 2D glitch sweep (delay x width), same shape as nrf_glitch_approtect.
// Per attempt: power-cycle, re-enter ICSP, point TBLPTR at probe_addr (a known
// protected location), glitch during the table-read clock-out, and report when
// the byte read back is non-zero. Returns true on first non-zero (leaked) byte.
//   probe_addr      : a byte address inside a code-protected block
//   width_*_cyc      : crowbar pulse band, clamped to PIC_GLITCH_WIDTH_MIN/MAX
//   power_off_ms     : target off-time per power cycle
//   settle_ms        : wait after power-on before ICSP entry
//   max_attempts     : total power-cycle attempts before giving up
bool pic18_glitch_cp(uint32_t delay_start_us, uint32_t delay_end_us,
                     uint32_t delay_step_us, uint32_t width_start_cyc,
                     uint32_t width_end_cyc, uint32_t width_step_cyc,
                     uint32_t probe_addr, uint32_t power_off_ms,
                     uint32_t settle_ms, uint32_t max_attempts);

#endif // PIC18_TARGET_H
