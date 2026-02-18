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

#endif // JTAG_H
