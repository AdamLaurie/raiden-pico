/*
 * SWD (Serial Wire Debug) interface for Raiden-Pico
 *
 * Provides CLI-accessible SWD primitives for target debugging.
 */

#ifndef SWD_H
#define SWD_H

#include <stdint.h>
#include <stdbool.h>

// SWD Pin Configuration
#define SWD_SWCLK_PIN   17
#define SWD_SWDIO_PIN   18

// SWD ACK responses
#define SWD_ACK_OK      0x1
#define SWD_ACK_WAIT    0x2
#define SWD_ACK_FAULT   0x4

// Debug Port (DP) register addresses (A[3:2])
#define DP_DPIDR        0x0     // Read: ID Register
#define DP_ABORT        0x0     // Write: Abort Register
#define DP_CTRL_STAT    0x4     // Control/Status
#define DP_SELECT       0x8     // AP Select
#define DP_RDBUFF       0xC     // Read Buffer (for AP reads)

// Common AP register addresses
#define AP_CSW          0x00    // Control/Status Word
#define AP_TAR          0x04    // Transfer Address Register
#define AP_DRW          0x0C    // Data Read/Write
#define AP_IDR          0xFC    // Identification Register

// Initialize SWD interface
void swd_init(void);

// Deinitialize SWD (pins to high-Z)
void swd_deinit(void);

// Connect to target (line reset + JTAG-to-SWD switch)
// Returns true on success
bool swd_connect(void);

// Read Debug Port register
// addr: register address (0, 4, 8, or C)
// value: pointer to store result
// Returns true on ACK OK
bool swd_read_dp(uint8_t addr, uint32_t *value);

// Write Debug Port register
bool swd_write_dp(uint8_t addr, uint32_t value);

// Read Access Port register
// Handles SELECT register automatically
// ap: AP number (usually 0 for AHB-AP)
// addr: register address within AP
bool swd_read_ap(uint8_t ap, uint8_t addr, uint32_t *value);

// Write Access Port register
bool swd_write_ap(uint8_t ap, uint8_t addr, uint32_t value);

// Read memory via AHB-AP
// addr: target memory address
// data: buffer to store data
// count: number of 32-bit words to read
// Returns number of words successfully read
uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);

// Write memory via AHB-AP
// Returns number of words successfully written
uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);

// Get last ACK response
uint8_t swd_get_last_ack(void);

// Clear any fault condition
bool swd_clear_errors(void);

// Connect and read DPIDR (convenience function)
// Returns DPIDR on success, 0 on failure
uint32_t swd_identify(void);

#endif // SWD_H
