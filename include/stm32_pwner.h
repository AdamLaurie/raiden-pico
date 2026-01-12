/*
 * STM32 RDP Bypass Integration for Raiden-Pico
 *
 * Supports STM32F1 and STM32F4 series
 *
 * Based on stm32f1-picopwner and stimpik by Patrick Pedersen (CTXz)
 * Original attack by Johannes Obermaier, Marc Schink and Kosma Moczek
 * Paper: https://www.usenix.org/system/files/woot20-paper-obermaier.pdf
 *
 * This module integrates the STM32 RDP Level 1 bypass attack into
 * the raiden-pico glitching platform.
 *
 * Target connections (Pico side is always GP4/GP5 at 9600 baud):
 *   STM32F1: Connect to PA9 (TX) / PA10 (RX) - USART1
 *   STM32F4: Connect to PC10 (TX) / PC11 (RX) - USART4
 */

#ifndef STM32_PWNER_H
#define STM32_PWNER_H

#include <stdint.h>
#include <stdbool.h>

// Pin configuration for STM32 attack (remapped to raiden-pico pins)
#define STM32_BOOT0_PIN     13   // GP13 - BOOT0 control
#define STM32_BOOT1_PIN     14   // GP14 - BOOT1 control
#define STM32_POWER_PIN     10   // GP10 - Target power (existing)
#define STM32_RESET_PIN     15   // GP15 - Target reset (existing)
#define STM32_UART_TX_PIN   4    // GP4 - Target UART TX (existing)
#define STM32_UART_RX_PIN   5    // GP5 - Target UART RX (existing)

// UART configuration
#define STM32_UART_ID       uart1
#define STM32_UART_BAUD     9600

// Timing parameters (milliseconds)
#define STM32_STAGE1_DELAY_MS   10   // Wait for SRAM exploit stage 1 (after power glitch)
#define STM32_BOOT0_DELAY_MS    1    // Delay after setting BOOT0 low before reset
#define STM32_RESET_DELAY_MS    15   // Reset hold time

// Power-off timing (loop iterations, not time-based)
// This value is empirically determined based on board capacitance
// The original stimpik uses 100000 - adjust if reset monitoring fails
#define STM32_POWEROFF_LOOPS    100000

// Magic bytes indicating dump start
#define STM32_DUMP_MAGIC_0  0x10
#define STM32_DUMP_MAGIC_1  0xAD
#define STM32_DUMP_MAGIC_2  0xDA
#define STM32_DUMP_MAGIC_3  0x7A

// Attack result codes
typedef enum {
    STM32_OK = 0,
    STM32_ERR_NOT_INITIALIZED,
    STM32_ERR_TIMEOUT,
    STM32_ERR_NO_MAGIC,
    STM32_ERR_UART_FAIL
} stm32_result_t;

// Attack state
typedef enum {
    STM32_STATE_IDLE = 0,
    STM32_STATE_ARMED,
    STM32_STATE_GLITCHING,
    STM32_STATE_WAITING_MAGIC,
    STM32_STATE_DUMPING,
    STM32_STATE_COMPLETE,
    STM32_STATE_ERROR
} stm32_state_t;

// Initialize STM32 pwner module
void stm32_pwner_init(void);

// Configure BOOT0 pin
void stm32_pwner_set_boot0_pin(uint8_t pin);

// Get current BOOT0 pin
uint8_t stm32_pwner_get_boot0_pin(void);

// Set BOOT0 state (high = SRAM/bootloader boot, low = flash boot)
void stm32_pwner_set_boot0(bool high);

// Configure BOOT1 pin
void stm32_pwner_set_boot1_pin(uint8_t pin);

// Get current BOOT1 pin
uint8_t stm32_pwner_get_boot1_pin(void);

// Set BOOT1 state (BOOT0=1,BOOT1=0: bootloader, BOOT0=1,BOOT1=1: SRAM)
void stm32_pwner_set_boot1(bool high);

// Execute the power glitch attack
// This assumes exploit firmware is already loaded into target SRAM via debug probe
// Returns: STM32_OK on success, error code otherwise
stm32_result_t stm32_pwner_attack(void);

// Check current state
stm32_state_t stm32_pwner_get_state(void);

// Get bytes received during dump
uint32_t stm32_pwner_get_bytes_received(void);

// Process incoming UART data (call from main loop during dump)
void stm32_pwner_process(void);

// Abort current operation
void stm32_pwner_abort(void);

// Get result string
const char* stm32_result_str(stm32_result_t result);

#endif // STM32_PWNER_H
