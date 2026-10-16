/*
 * JTAG interface for Raiden-Pico
 *
 * Provides CLI-accessible JTAG primitives for target debugging.
 * Supports ARM7TDMI and other JTAG-compatible devices.
 */

#ifndef JTAG_H
#define JTAG_H

#include <stdint.h>
#include <stdbool.h>

// JTAG Pin Configuration (shares TCK/TMS with SWD)
#define JTAG_TCK_PIN    17   // Same as SWD_SWCLK_PIN
#define JTAG_TMS_PIN    18   // Same as SWD_SWDIO_PIN
#define JTAG_TDI_PIN    19   // Data to target
#define JTAG_TDO_PIN    20   // Data from target
#define JTAG_RTCK_PIN   21   // Return TCK (adaptive clocking)
#define JTAG_TRST_PIN   15   // Optional reset (active low), shares with target reset

// JTAG TAP states
typedef enum {
    TAP_RESET = 0,
    TAP_IDLE,
    TAP_SELECT_DR,
    TAP_CAPTURE_DR,
    TAP_SHIFT_DR,
    TAP_EXIT1_DR,
    TAP_PAUSE_DR,
    TAP_EXIT2_DR,
    TAP_UPDATE_DR,
    TAP_SELECT_IR,
    TAP_CAPTURE_IR,
    TAP_SHIFT_IR,
    TAP_EXIT1_IR,
    TAP_PAUSE_IR,
    TAP_EXIT2_IR,
    TAP_UPDATE_IR
} jtag_tap_state_t;

// Common JTAG instructions (ARM7TDMI)
#define JTAG_IR_EXTEST      0x0
#define JTAG_IR_SCAN_N      0x2
#define JTAG_IR_SAMPLE      0x3
#define JTAG_IR_RESTART     0x4
#define JTAG_IR_CLAMP       0x5
#define JTAG_IR_HIGHZ       0x7
#define JTAG_IR_CLAMPZ      0x9
#define JTAG_IR_INTEST      0xC
#define JTAG_IR_IDCODE      0xE
#define JTAG_IR_BYPASS      0xF

// Initialize JTAG interface
void jtag_init(void);

// Deinitialize JTAG (pins to high-Z)
void jtag_deinit(void);

// Reset TAP state machine (5 TCK with TMS high)
void jtag_reset(void);

// Move to Run-Test/Idle state
void jtag_idle(void);

// Shift data through IR, returns previous IR value
uint32_t jtag_ir_shift(uint32_t ir, uint8_t bits);

// Shift data through DR, returns captured data
uint32_t jtag_dr_shift(uint32_t dr, uint8_t bits);

// Shift 64-bit data through DR
uint64_t jtag_dr_shift64(uint64_t dr, uint8_t bits);

// Read IDCODE (assumes 32-bit IDCODE)
uint32_t jtag_read_idcode(void);

// Scan chain to detect devices
// Returns number of devices found, fills idcodes array
uint8_t jtag_scan_chain(uint32_t *idcodes, uint8_t max_devices);

// Set IR length for current device (default 4 for ARM7)
void jtag_set_ir_length(uint8_t bits);

// Get current TAP state
jtag_tap_state_t jtag_get_state(void);

// Check if RTCK adaptive clocking is available
bool jtag_rtck_available(void);

// --- ARM ADIv6 JTAG-DP access ---
// Default MEM-AP base used by the convenience commands; override per target.
#define ADIV6_AXI_AP    0x7c004000u

// Raw DP/AP register transfers (ir = DP_IR_DPACC/DP_IR_APACC, defined in jtag.c).
bool jtag_dp_write(uint8_t ir, uint32_t reg, uint32_t val);
bool jtag_dp_read(uint8_t ir, uint32_t reg, uint32_t *out);
// Reset TAP, read TAP IDCODE + DP IDR, request debug power domains.
bool jtag_adiv6_dp_init(uint32_t *tap_idcode, uint32_t *dp_idr, uint32_t *ctrlstat);
// AP register access by full ADIv6 address (ap_base + reg offset).
bool jtag_adiv6_ap_read(uint32_t ap_base, uint32_t reg, uint32_t *out);
bool jtag_adiv6_ap_write(uint32_t ap_base, uint32_t reg, uint32_t val);
// DeviceEn (CSW bit 6): 0/1, or -1 on access failure.
int  jtag_adiv6_deviceen(uint32_t ap_base, uint32_t *csw_out);
// One 32-bit word of target memory via the MEM-AP (TAR + DRW).
bool jtag_adiv6_mem_read(uint32_t ap_base, uint32_t addr, uint32_t *out);
// One 32-bit word WRITE via the MEM-AP (TAR + DRW); needs CSW DeviceEn = 1.
bool jtag_adiv6_mem_write(uint32_t ap_base, uint32_t addr, uint32_t val);
// Block read with MEM-AP TAR auto-increment; returns words read (stops on error).
uint32_t jtag_adiv6_mem_read_block(uint32_t ap_base, uint32_t addr,
                                   uint32_t *buf, uint32_t count);
// DP CTRL/STAT read, sticky test, ABORT, and sticky-clear recovery.
bool jtag_adiv6_ctrlstat(uint32_t *out);
bool jtag_adiv6_ctrlstat_sticky(uint32_t cs);
void jtag_dp_abort(uint32_t flags);
bool jtag_adiv6_clear_sticky(void);
// Set MEM-AP CSW: Size=word, AddrInc single (true) or off (false).
bool jtag_adiv6_csw_config(uint32_t ap_base, bool addrinc);
// Diagnostic: three raw DPACC DPIDR-read scans (inspect ACK + data).
void jtag_adiv6_dpraw(uint64_t out[3]);

#endif // JTAG_H
