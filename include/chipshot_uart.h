#ifndef CHIPSHOT_UART_H
#define CHIPSHOT_UART_H

#include <stdint.h>
#include <stdbool.h>

// Initialize ChipShouter UART
void chipshot_uart_init(void);

// Send command to ChipShouter
void chipshot_uart_send(const char *data);

// Process incoming data (call from main loop)
void chipshot_uart_process(void);

// Check if response is ready
bool chipshot_uart_response_ready(void);

// Get response from ChipShouter
const char* chipshot_uart_get_response(void);

// Clear response buffer
void chipshot_uart_clear_response(void);

// Blocking read - waits for complete response or timeout
// Returns pointer to response buffer, or NULL on timeout
const char* chipshot_uart_read_response_blocking(uint32_t timeout_ms);

// ChipShouter control commands
void chipshot_arm(void);
void chipshot_disarm(void);
void chipshot_fire(void);
void chipshot_set_voltage(uint32_t voltage);
void chipshot_set_pulse(uint32_t pulse_ns);
void chipshot_get_status(void);
void chipshot_set_trigger_hw(bool active_high);   // Set hardware trigger mode (true=active high, false=active low)
void chipshot_set_trigger_sw(void);   // Set software trigger mode

// Get armed state
bool chipshot_is_armed(void);

#endif // CHIPSHOT_UART_H
