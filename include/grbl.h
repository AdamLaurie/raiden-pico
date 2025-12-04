#ifndef GRBL_H
#define GRBL_H

#include <stdint.h>
#include <stdbool.h>

// Initialize Grbl UART (hardware UART1 on GP8/GP9)
void grbl_init(void);

// Deinitialize Grbl UART (for switching to target)
void grbl_deinit(void);

// Send G-code command
void grbl_send(const char *gcode);

// Move to absolute position (mm)
void grbl_move_absolute(float x, float y, float feedrate);

// Move relative (mm)
void grbl_move_relative(float dx, float dy, float feedrate);

// Home the machine
void grbl_home(void);

// Get current position
bool grbl_get_position(float *x, float *y, float *z);

// Wait for movement to complete
bool grbl_wait_idle(uint32_t timeout_ms);

// Get Grbl response (non-blocking)
const char* grbl_get_response(void);

// Check if response is available
bool grbl_response_ready(void);

// Clear response buffer
void grbl_clear_response(void);

// Debug: Get raw RX FIFO status and read any bytes
int grbl_debug_rx_fifo(char *buffer, int max_len);

// Test hardware UART loopback
bool grbl_test_loopback(void);

// Check if Grbl UART is active
bool grbl_is_active(void);

#endif // GRBL_H
