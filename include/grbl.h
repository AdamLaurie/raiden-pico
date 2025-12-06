#ifndef GRBL_H
#define GRBL_H

#include <stdint.h>
#include <stdbool.h>

// Initialize Grbl UART (hardware UART1 on GP8/GP9)
void grbl_init(void);

// Deinitialize Grbl UART (for switching to target)
void grbl_deinit(void);

// Send G-code command (async - returns immediately)
void grbl_send(const char *gcode);

// Wait for "ok" or "error" response from Grbl
bool grbl_wait_ack(uint32_t timeout_ms);

// Send command and wait for ok + idle state (synchronous)
bool grbl_send_sync(const char *gcode, uint32_t timeout_ms);

// Move to absolute position (mm) - async
void grbl_move_absolute(float x, float y, float feedrate);

// Move relative (mm) - async
void grbl_move_relative(float dx, float dy, float feedrate);

// Move to absolute position and wait for completion (synchronous)
bool grbl_move_absolute_sync(float x, float y, float feedrate, uint32_t timeout_ms);

// Move relative and wait for completion (synchronous)
bool grbl_move_relative_sync(float dx, float dy, float feedrate, uint32_t timeout_ms);

// Home the machine (async)
void grbl_home(void);

// Home the machine and wait for completion (synchronous)
bool grbl_home_sync(uint32_t timeout_ms);

// Soft reset the Grbl controller (sends Ctrl+X, 0x18)
void grbl_reset(void);

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
