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
#define SWD_NRST_PIN    15  // Target nRST (active low, active drive)

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

// Check if SWD is currently connected
bool swd_is_connected(void);

// Connect if not already connected (no-op if connected)
bool swd_ensure_connected(void);

// Connect under reset (hold nRST, connect, halt, release)
// Required for modifying option bytes on RDP-protected targets
bool swd_connect_under_reset(void);

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

// --- Cortex-M debug registers ---
#define DHCSR       0xE000EDF0  // Debug Halting Control/Status
#define DCRSR       0xE000EDF4  // Debug Core Register Selector
#define DCRDR       0xE000EDF8  // Debug Core Register Data
#define DEMCR       0xE000EDFC  // Debug Exception/Monitor Control
#define CPUID       0xE000ED00  // CPUID Base Register
#define AIRCR       0xE000ED0C  // Application Interrupt/Reset Control
#define CFSR        0xE000ED28  // Configurable Fault Status
#define HFSR        0xE000ED2C  // HardFault Status

// DHCSR key for writes
#define DBGKEY      0xA05F0000

// FPB (Flash Patch and Breakpoint) registers
#define FP_CTRL     0xE0002000  // FPB Control Register
#define FP_REMAP    0xE0002004  // FPB Remap Register
#define FP_COMP0    0xE0002008  // FPB Comparator 0
// FP_COMPn = FP_COMP0 + n*4, n=0..5 (instruction), 6..7 (literal)

// DWT (Data Watchpoint and Trace) registers
#define DWT_CTRL    0xE0001000  // DWT Control
#define DWT_CYCCNT  0xE0001004  // DWT Cycle Counter

// Cortex-M debug ID register (STM32-specific)
#define DBG_IDCODE  0xE0042000

// Assert nRST (hold target in reset)
void swd_nrst_assert(void);

// Release nRST (let target run)
void swd_nrst_release(void);

// Pulse nRST low for given duration in ms, then release
void swd_nrst_pulse(uint32_t ms);

// Halt target core
bool swd_halt(void);

// Resume target core
bool swd_resume(void);

// Read a CPU register (r0-r15, xPSR etc) while halted
// reg: 0-15 = r0-r15, 16 = xPSR, 17 = MSP, 18 = PSP
bool swd_read_core_reg(uint8_t reg, uint32_t *value);

// Write a CPU register (r0-r15, xPSR etc) while halted
bool swd_write_core_reg(uint8_t reg, uint32_t value);

// Detect target: read CPUID and STM32 debug ID code
// Fills cpuid and dbg_idcode, returns true on success
bool swd_detect(uint32_t *cpuid, uint32_t *dbg_idcode);

// --- STM32 high-level operations (require target type set) ---

#include "config.h"

// Read RDP level from flash option register
// Returns 0, 1, or 2 for the level, or -1 on error
int swd_stm32_read_rdp(const stm32_target_info_t *info);

// Read option bytes (prints all relevant option registers)
bool swd_stm32_read_options(const stm32_target_info_t *info);

// Set RDP level (0 = unprotect + mass erase, 1 = protect)
// WARNING: Setting RDP to 0 triggers mass erase on most families
bool swd_stm32_set_rdp(const stm32_target_info_t *info, uint8_t level);

// Unlock flash controller
bool swd_stm32_flash_unlock(const stm32_target_info_t *info);

// Erase a flash page (page number, not address)
bool swd_stm32_flash_erase_page(const stm32_target_info_t *info, uint32_t page);

// Write flash (must be erased first, handles family-specific write size)
// Returns number of bytes written
uint32_t swd_stm32_flash_write(const stm32_target_info_t *info, uint32_t addr,
                                const uint8_t *data, uint32_t len);

// Wait for flash BSY flag to clear
bool swd_stm32_flash_wait(const stm32_target_info_t *info, uint32_t timeout_ms);

#endif // SWD_H
