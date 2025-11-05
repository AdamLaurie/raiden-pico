#ifndef TARGET_UART_H
#define TARGET_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// Initialize target subsystem with defaults
void target_init(void);

// Set target type
void target_set_type(target_type_t type);

// Get target type
target_type_t target_get_type(void);

// Enter bootloader mode with optional baud rate and crystal speed
bool target_enter_bootloader(uint32_t baud, uint32_t crystal_khz);

// Initialize target UART (low-level)
void target_uart_init(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud);

// Send single byte to target
void target_uart_send_byte(uint8_t byte);

// Send ASCII string to target (appends \r automatically)
void target_uart_send_string(const char *str);

// Send hex string to target (appends \r automatically)
void target_uart_send_hex(const char *hex_str);

// Process incoming data (call from main loop)
void target_uart_process(void);

// Get response count
uint16_t target_uart_get_response_count(void);

// Get response data
const char* target_uart_get_response(void);

// Clear response buffer
void target_uart_clear_response(void);

// Print response as hex
void target_uart_print_response_hex(void);

// Configure target reset
void target_reset_config(uint8_t pin, uint32_t period_ms, bool active_high);

// Execute target reset
void target_reset_execute(void);

// Check if initialized
bool target_is_initialized(void);

// Debug mode control
void target_set_debug(bool enable);
bool target_get_debug(void);

// Transparent bridge timeout control (milliseconds)
void target_set_timeout(uint32_t timeout_ms);
uint32_t target_get_timeout(void);

#endif // TARGET_UART_H
