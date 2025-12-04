#include "grbl.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "uart_cli.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

// Pin configuration - GP8/GP9 using UART1 alternate function
#define GRBL_TX_PIN 8   // GP8 (UART1_TX alternate)
#define GRBL_RX_PIN 9   // GP9 (UART1_RX alternate)
#define GRBL_BAUD 115200
#define GRBL_UART_ID uart1

// Response buffer
#define GRBL_RESP_SIZE 256
static char grbl_response[GRBL_RESP_SIZE];
static volatile uint16_t grbl_resp_len = 0;
static volatile bool grbl_resp_ready = false;

// Current position (cached from status reports)
static float current_x = 0.0f;
static float current_y = 0.0f;
static float current_z = 0.0f;

// UART state
static bool grbl_uart_active = false;

void grbl_init(void) {
    // If target UART is active, deinitialize it first
    // Target uses UART1 on GP4/GP5, we need UART1 on GP8/GP9
    extern bool target_is_initialized(void);
    if (target_is_initialized()) {
        // Target UART deinit will be called by target_uart_init when needed
        // Just deinit the UART1 peripheral and GPIO pins
        uart_deinit(GRBL_UART_ID);
        gpio_deinit(4);
        gpio_deinit(5);
    } else {
        // Deinitialize UART1 anyway (may be in unknown state)
        uart_deinit(GRBL_UART_ID);
        gpio_deinit(4);
        gpio_deinit(5);
    }

    // Deinitialize our GPIO pins
    gpio_deinit(GRBL_TX_PIN);
    gpio_deinit(GRBL_RX_PIN);

    // Small delay to let hardware settle
    sleep_us(100);

    // Initialize pins BEFORE UART (like SDK examples)
    gpio_init(GRBL_TX_PIN);
    gpio_init(GRBL_RX_PIN);

    // Initialize UART1
    uart_init(GRBL_UART_ID, GRBL_BAUD);
    uart_set_format(GRBL_UART_ID, 8, 1, UART_PARITY_NONE);

    // Use UART_FUNCSEL_NUM macro for RP2350 alternate function (F11)
    gpio_set_function(GRBL_TX_PIN, UART_FUNCSEL_NUM(GRBL_UART_ID, GRBL_TX_PIN));
    gpio_set_function(GRBL_RX_PIN, UART_FUNCSEL_NUM(GRBL_UART_ID, GRBL_RX_PIN));

    // Enable UART FIFO
    uart_set_fifo_enabled(GRBL_UART_ID, true);

    // Small delay for UART to stabilize
    sleep_us(100);

    grbl_uart_active = true;

    uart_cli_printf("OK: Grbl UART initialized on GP%u (TX), GP%u (RX) at %u baud\r\n",
                    GRBL_TX_PIN, GRBL_RX_PIN, GRBL_BAUD);

    // Clear any buffered messages from the Grbl controller
    grbl_clear_response();
}

void grbl_deinit(void) {
    if (grbl_uart_active) {
        uart_deinit(GRBL_UART_ID);
        gpio_deinit(GRBL_TX_PIN);
        gpio_deinit(GRBL_RX_PIN);
        grbl_uart_active = false;
    }
}

void grbl_send(const char *gcode) {
    if (!grbl_uart_active) {
        uart_cli_send("ERROR: Grbl UART not initialized\r\n");
        return;
    }

    // Send G-code string followed by \n
    uart_puts(GRBL_UART_ID, gcode);
    uart_putc(GRBL_UART_ID, '\n');
    uart_tx_wait_blocking(GRBL_UART_ID);

    uart_cli_printf("[Grbl TX] %s\r\n", gcode);
}

void grbl_move_absolute(float x, float y, float feedrate) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "G90 G0 X%.3f Y%.3f F%.0f", x, y, feedrate);
    grbl_send(cmd);
}

void grbl_move_relative(float dx, float dy, float feedrate) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "G91 G0 X%.3f Y%.3f F%.0f", dx, dy, feedrate);
    grbl_send(cmd);
}

void grbl_home(void) {
    uart_cli_send("[Grbl] Homing...\r\n");
    grbl_send("$H");
    grbl_wait_idle(60000);  // 60 second timeout for homing
}

bool grbl_get_position(float *x, float *y, float *z) {
    if (!grbl_uart_active) {
        return false;
    }

    // Request position report
    grbl_send("?");

    // Wait for response
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < 1000) {
        if (grbl_response_ready()) {
            const char *resp = grbl_get_response();
            // Parse status report: <Idle|MPos:0.000,0.000,0.000|...>
            if (resp[0] == '<') {
                const char *mpos = strstr(resp, "MPos:");
                if (mpos) {
                    if (sscanf(mpos + 5, "%f,%f,%f", &current_x, &current_y, &current_z) == 3) {
                        if (x) *x = current_x;
                        if (y) *y = current_y;
                        if (z) *z = current_z;
                        grbl_clear_response();
                        return true;
                    }
                }
            }
            grbl_clear_response();
        }
        sleep_ms(10);
    }
    return false;
}

bool grbl_wait_idle(uint32_t timeout_ms) {
    if (!grbl_uart_active) {
        return false;
    }

    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (to_ms_since_boot(get_absolute_time()) - start < timeout_ms) {
        grbl_send("?");
        sleep_ms(100);

        if (grbl_response_ready()) {
            const char *resp = grbl_get_response();
            if (strstr(resp, "<Idle") || strstr(resp, "<Check")) {
                grbl_clear_response();
                return true;
            }
            grbl_clear_response();
        }
    }

    uart_cli_send("[Grbl] Timeout waiting for idle\r\n");
    return false;
}

const char* grbl_get_response(void) {
    return grbl_response;
}

bool grbl_response_ready(void) {
    if (!grbl_uart_active) {
        return false;
    }

    // Check if data available from UART RX FIFO
    while (uart_is_readable(GRBL_UART_ID) && grbl_resp_len < GRBL_RESP_SIZE - 1) {
        char c = uart_getc(GRBL_UART_ID);
        grbl_response[grbl_resp_len++] = c;

        // Check for end of line
        if (c == '\n' || c == '\r' || c == '>') {
            grbl_response[grbl_resp_len] = '\0';
            grbl_resp_ready = true;
            uart_cli_printf("[Grbl RX] %s\r\n", grbl_response);
            return true;
        }
    }
    return false;
}

void grbl_clear_response(void) {
    grbl_resp_len = 0;
    grbl_resp_ready = false;
    grbl_response[0] = '\0';

    if (!grbl_uart_active) {
        return;
    }

    // Drain RX FIFO
    while (uart_is_readable(GRBL_UART_ID)) {
        uart_getc(GRBL_UART_ID);
    }
}

int grbl_debug_rx_fifo(char *buffer, int max_len) {
    if (!grbl_uart_active) {
        return 0;
    }

    int count = 0;

    // Read all available bytes from RX FIFO
    while (uart_is_readable(GRBL_UART_ID) && count < max_len - 1) {
        char c = uart_getc(GRBL_UART_ID);
        buffer[count++] = c;
    }

    buffer[count] = '\0';
    return count;
}

bool grbl_is_active(void) {
    return grbl_uart_active;
}

bool grbl_test_loopback(void) {
    if (!grbl_uart_active) {
        uart_cli_send("ERROR: Grbl UART not initialized\r\n");
        return false;
    }

    uart_cli_send("Testing Grbl UART loopback (GP8->GP9)...\r\n");

    // Clear RX buffer
    grbl_clear_response();

    // Send test byte
    const char test_char = 'U';  // 0x55 - alternating bits pattern
    uart_putc(GRBL_UART_ID, test_char);
    uart_tx_wait_blocking(GRBL_UART_ID);

    uart_cli_printf("Sent: 0x%02X '%c'\r\n", test_char, test_char);

    // Wait for echo
    sleep_ms(100);

    // Check if received
    if (uart_is_readable(GRBL_UART_ID)) {
        char received = uart_getc(GRBL_UART_ID);
        uart_cli_printf("Received: 0x%02X '%c'\r\n", received, received);

        if (received == test_char) {
            uart_cli_send("OK: Loopback test passed\r\n");
            return true;
        } else {
            uart_cli_send("ERROR: Loopback data mismatch\r\n");
            return false;
        }
    } else {
        uart_cli_send("ERROR: No data received\r\n");
        return false;
    }
}
