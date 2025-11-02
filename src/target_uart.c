#include "config.h"
#include "uart_cli.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>

// Hardware UART for target (UART1)
#define TARGET_UART_ID uart1
#define TARGET_UART_TX_PIN 4  // GP4
#define TARGET_UART_RX_PIN 5  // GP5

// UART configuration
static uint32_t target_baud = 115200;
static bool target_initialized = false;

// Target type
static target_type_t current_target_type = TARGET_NONE;

// Response buffer
#define TARGET_RESPONSE_SIZE 512
static char target_response[TARGET_RESPONSE_SIZE];
static uint16_t target_response_pos = 0;
static uint16_t target_response_count = 0;

// Sent data tracking for echo removal
static char sent_data[TARGET_RESPONSE_SIZE];
static uint16_t sent_data_len = 0;

// Reset configuration
static uint8_t reset_pin = 15;
static uint32_t reset_period_ms = 300;  // Default 300ms reset period
static bool reset_active_high = false;
static bool reset_pin_initialized = false;

// Debug mode
static bool debug_mode = false;

// Forward declarations (implemented below)
void target_uart_init(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud);
void target_uart_send_byte(uint8_t byte);
void target_reset_config(uint8_t pin, uint32_t period_ms, bool active_high);
void target_uart_clear_response(void);
void target_uart_print_response_hex(void);

// UART RX interrupt handler for immediate trigger response
void target_uart_irq_handler(void) {
    extern void glitch_check_uart_trigger(uint8_t byte);
    extern void uart_cli_printf(const char *format, ...);
    extern void uart_cli_send(const char *str);

    while (uart_is_readable(TARGET_UART_ID)) {
        uint8_t byte = uart_getc(TARGET_UART_ID);

        // Check for trigger IMMEDIATELY in interrupt context for minimal latency
        // This must be called before any other processing for lowest latency
        glitch_check_uart_trigger(byte);

        // Display received byte in debug mode (after trigger check)
        if (debug_mode) {
            uart_cli_printf("[RX] %02X", byte);
            if (byte >= 32 && byte < 127) {
                uart_cli_printf(" '%c'", byte);
            }
            uart_cli_send("\r\n");
        }

        // Store in response buffer
        if (target_response_pos < TARGET_RESPONSE_SIZE - 1) {
            target_response[target_response_pos++] = byte;
            target_response_count++;
        }
    }
}

void target_init(void) {
    // Initialize reset pin with defaults on startup
    // This ensures TARGET RESET works without explicit configuration
    target_reset_config(reset_pin, reset_period_ms, reset_active_high);

    // Pre-initialize and deinit UART1 to ensure clean state after Pico boot
    // This works around an issue where first UART TX after boot fails
    gpio_init(TARGET_UART_TX_PIN);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);
    sleep_ms(10);
    uart_deinit(TARGET_UART_ID);
    gpio_deinit(TARGET_UART_TX_PIN);
    gpio_deinit(TARGET_UART_RX_PIN);
}

// Helper function to wait for and read a response with timeout
static bool wait_for_response(const char *expected, uint32_t timeout_ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());
    size_t expected_len = strlen(expected);
    size_t match_pos = 0;

    while (to_ms_since_boot(get_absolute_time()) - start < timeout_ms) {
        if (uart_is_readable(TARGET_UART_ID)) {
            uint8_t byte = uart_getc(TARGET_UART_ID);

            // Display received byte in debug mode
            if (debug_mode) {
                uart_cli_printf("[RX] %02X", byte);
                if (byte >= 32 && byte < 127) {
                    uart_cli_printf(" '%c'", byte);
                }
                uart_cli_send("\r\n");
            }

            // Store in response buffer
            if (target_response_pos < TARGET_RESPONSE_SIZE - 1) {
                target_response[target_response_pos++] = byte;
                target_response_count++;
            }

            // Check if it matches expected response
            if (byte == expected[match_pos]) {
                match_pos++;
                if (match_pos == expected_len) {
                    return true;  // Found complete match
                }
            } else {
                match_pos = 0;  // Reset match position
            }
        }
        sleep_us(100);  // Small delay to avoid busy waiting
    }

    return false;  // Timeout
}

void target_set_type(target_type_t type) {
    current_target_type = type;
}

target_type_t target_get_type(void) {
    return current_target_type;
}

bool target_enter_bootloader(uint32_t baud, uint32_t crystal_khz) {
    if (current_target_type == TARGET_NONE) {
        uart_cli_send("ERROR: No target type set. Use TARGET <LPC|STM32> first\r\n");
        return false;
    }

    // Initialize UART1 on GP4/GP5 with specified baud
    target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, baud);

    // Disable UART interrupt during bootloader communication to avoid race conditions
    // We use blocking reads during bootloader sync, so interrupts would interfere
    uart_set_irq_enables(TARGET_UART_ID, false, false);

    // Clear any stale data from UART RX FIFO that may have accumulated
    while (uart_is_readable(TARGET_UART_ID)) {
        uart_getc(TARGET_UART_ID);
    }

    // Target-specific bootloader entry
    switch (current_target_type) {
        case TARGET_LPC:
            uart_cli_send("Entering LPC ISP bootloader mode...\r\n");

            // Clear response buffer
            target_uart_clear_response();

            // Small delay to ensure target UART is fully ready after reset and UART init
            sleep_ms(10);

            // 1. Send '?' (0x3F) - sync character
            uart_cli_send("Sending '?'...\r\n");
            target_uart_send_byte('?');

            // 2. Wait for 'Synchronized\r\n' response
            uart_cli_send("Waiting for 'Synchronized'...\r\n");
            if (!wait_for_response("Synchronized\r\n", 1000)) {
                uart_cli_send("ERROR: Timeout waiting for 'Synchronized'\r\n");
                return false;
            }

            // 3. Send "Synchronized\r\n"
            uart_cli_send("Sending 'Synchronized'...\r\n");
            const char *sync_msg = "Synchronized\r\n";
            for (int i = 0; sync_msg[i] != '\0'; i++) {
                target_uart_send_byte(sync_msg[i]);
            }

            // 4. Wait for "OK\r\n" response
            uart_cli_send("Waiting for OK...\r\n");
            if (!wait_for_response("OK\r\n", 1000)) {
                uart_cli_send("ERROR: Timeout waiting for first OK\r\n");
                return false;
            }

            // 5. Send crystal frequency
            char freq_msg[16];
            snprintf(freq_msg, sizeof(freq_msg), "%u\r\n", crystal_khz);
            uart_cli_printf("Sending crystal frequency (%u kHz)...\r\n", crystal_khz);
            for (int i = 0; freq_msg[i] != '\0'; i++) {
                target_uart_send_byte(freq_msg[i]);
            }

            // 6. Wait for second "OK\r\n"
            uart_cli_send("Waiting for final OK...\r\n");
            if (!wait_for_response("OK\r\n", 1000)) {
                uart_cli_send("ERROR: Timeout waiting for second OK\r\n");
                return false;
            }

            // 7. Enable echo with "A 1\r\n" (needed for UART glitch triggering)
            uart_cli_send("Enabling echo mode...\r\n");
            const char *echo_on = "A 1\r\n";
            for (int i = 0; echo_on[i] != '\0'; i++) {
                target_uart_send_byte(echo_on[i]);
            }

            // Wait for echo OK
            if (!wait_for_response("0\r\n", 1000)) {
                uart_cli_send("WARNING: Timeout waiting for echo confirmation\r\n");
            }

            uart_cli_send("LPC ISP sync complete. Echo mode enabled.\r\n");
            break;

        case TARGET_STM32:
            uart_cli_send("Entering STM32 bootloader mode...\r\n");
            uart_cli_send("Sending 0x7F for STM32 sync...\r\n");
            // Send 0x7F for STM32 bootloader sync
            target_uart_send_byte(0x7F);
            sleep_ms(100);
            break;

        default:
            uart_cli_send("ERROR: Unknown target type\r\n");
            return false;
    }

    // Re-enable UART RX interrupts for normal trigger operation
    uart_set_irq_enables(TARGET_UART_ID, true, false);

    uart_cli_printf("OK: Bootloader mode active at %u baud on GP4/GP5\r\n", baud);
    return true;
}

void target_uart_init(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud) {
    target_baud = baud;

    // Always deinitialize first to ensure clean state
    // After Pico boot, UART peripheral and GPIO may be in undefined state
    if (target_initialized) {
        uart_set_irq_enables(TARGET_UART_ID, false, false);
        irq_set_enabled(UART1_IRQ, false);
    }
    uart_deinit(TARGET_UART_ID);  // Always deinit, even on first use

    // Deinitialize GPIO pins to clear any previous state
    // Critical for first use after Pico reboot
    gpio_deinit(tx_pin);
    gpio_deinit(rx_pin);

    // Initialize UART1
    uart_init(TARGET_UART_ID, baud);

    // Set UART format explicitly (8 data bits, 1 stop bit, no parity)
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);

    // Set TX and RX pins for UART1
    gpio_set_function(tx_pin, GPIO_FUNC_UART);
    gpio_set_function(rx_pin, GPIO_FUNC_UART);

    // Enable UART FIFO
    uart_set_fifo_enabled(TARGET_UART_ID, true);

    // Small delay to allow UART hardware to stabilize after initialization
    sleep_us(100);

    // Enable UART RX interrupt for minimal trigger latency
    irq_set_exclusive_handler(UART1_IRQ, target_uart_irq_handler);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irq_enables(TARGET_UART_ID, true, false);  // RX interrupt enabled, TX disabled

    target_initialized = true;

    // Clear response buffer
    memset(target_response, 0, TARGET_RESPONSE_SIZE);
    target_response_pos = 0;
    target_response_count = 0;

    uart_cli_printf("OK: Target UART1 initialized on GP%u (TX), GP%u (RX) at %u baud\r\n",
                    tx_pin, rx_pin, baud);
}

void target_uart_send_byte(uint8_t byte) {
    // Auto-initialize UART with defaults if not already initialized
    if (!target_initialized) {
        target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, target_baud);
    }

    uart_putc_raw(TARGET_UART_ID, byte);
    // Wait for TX FIFO to actually transmit the byte
    uart_tx_wait_blocking(TARGET_UART_ID);

    // Display sent byte in debug mode
    if (debug_mode) {
        uart_cli_printf("[TX] %02X", byte);
        if (byte >= 32 && byte < 127) {
            uart_cli_printf(" '%c'", byte);
        }
        uart_cli_send("\r\n");
    }
}

void target_uart_send_string(const char *str) {
    // Auto-initialize UART with defaults if not already initialized
    if (!target_initialized) {
        target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, target_baud);
    }

    // Disable UART RX interrupt to prevent it from bypassing echo filtering
    uart_set_irq_enables(TARGET_UART_ID, false, false);

    // Clear response buffer before sending
    target_uart_clear_response();

    // Track what we're sending for echo removal
    sent_data_len = 0;
    const char *p = str;
    while (*p && sent_data_len < TARGET_RESPONSE_SIZE - 1) {
        sent_data[sent_data_len++] = *p;
        p++;
    }
    sent_data[sent_data_len++] = '\r';  // Add the \r we'll append

    // Send each character in the string
    p = str;
    while (*p) {
        target_uart_send_byte(*p);
        p++;
    }

    // Always append \r
    target_uart_send_byte('\r');

    uart_cli_send("OK: String sent to target\r\n");

    // External trigger check function
    extern void glitch_check_uart_trigger(uint8_t byte);

    // Wait for response (500ms timeout) - actively poll UART
    uint32_t start = to_ms_since_boot(get_absolute_time());
    uint16_t echo_match_pos = 0;
    bool echo_skipped = false;

    while (to_ms_since_boot(get_absolute_time()) - start < 500) {
        // Read any incoming data
        while (uart_is_readable(TARGET_UART_ID)) {
            uint8_t byte = uart_getc(TARGET_UART_ID);

            // Check for UART trigger byte
            glitch_check_uart_trigger(byte);

            // Display received byte in debug mode
            if (debug_mode) {
                uart_cli_printf("[RX] %02X", byte);
                if (byte >= 32 && byte < 127) {
                    uart_cli_printf(" '%c'", byte);
                }
                uart_cli_send("\r\n");
            }

            // Check if this is part of the echo
            if (!echo_skipped && echo_match_pos < sent_data_len) {
                if (byte == (uint8_t)sent_data[echo_match_pos]) {
                    echo_match_pos++;
                    if (echo_match_pos == sent_data_len) {
                        echo_skipped = true;  // Echo complete, start collecting real response
                    }
                    continue;  // Skip storing echo bytes
                } else {
                    // Not matching echo, must be actual response
                    // Store any previously skipped bytes that weren't echo
                    for (uint16_t i = 0; i < echo_match_pos; i++) {
                        if (target_response_pos < TARGET_RESPONSE_SIZE - 1) {
                            target_response[target_response_pos++] = sent_data[i];
                            target_response_count++;
                        }
                    }
                    echo_skipped = true;  // Stop trying to match echo
                }
            }

            // Store in response buffer (after echo is skipped)
            if (echo_skipped && target_response_pos < TARGET_RESPONSE_SIZE - 1) {
                target_response[target_response_pos++] = byte;
                target_response_count++;
            }
        }
        sleep_us(100);  // Small delay to avoid busy waiting
    }

    // Re-enable UART RX interrupt for trigger detection
    uart_set_irq_enables(TARGET_UART_ID, true, false);

    // Display response
    target_uart_print_response_hex();
}

void target_uart_send_hex(const char *hex_str) {
    // Auto-initialize UART with defaults if not already initialized
    if (!target_initialized) {
        target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, target_baud);
    }

    // Disable UART RX interrupt to prevent it from bypassing echo filtering
    uart_set_irq_enables(TARGET_UART_ID, false, false);

    // Clear response buffer before sending
    target_uart_clear_response();

    // Track what we're sending for echo removal
    sent_data_len = 0;

    // Parse hex string and send bytes
    const char *p = hex_str;
    while (*p) {
        // Skip spaces and 0x prefix
        if (*p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) {
            p += 2;
            continue;
        }

        // Parse hex byte
        if (*p >= '0' && *p <= '9') {
            uint8_t byte = (*p - '0') << 4;
            p++;
            if (*p >= '0' && *p <= '9') {
                byte |= (*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                byte |= (*p - 'a' + 10);
            } else if (*p >= 'A' && *p <= 'F') {
                byte |= (*p - 'A' + 10);
            }
            if (sent_data_len < TARGET_RESPONSE_SIZE - 1) {
                sent_data[sent_data_len++] = byte;
            }
            target_uart_send_byte(byte);
            p++;
        } else if (*p >= 'a' && *p <= 'f') {
            uint8_t byte = (*p - 'a' + 10) << 4;
            p++;
            if (*p >= '0' && *p <= '9') {
                byte |= (*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                byte |= (*p - 'a' + 10);
            } else if (*p >= 'A' && *p <= 'F') {
                byte |= (*p - 'A' + 10);
            }
            if (sent_data_len < TARGET_RESPONSE_SIZE - 1) {
                sent_data[sent_data_len++] = byte;
            }
            target_uart_send_byte(byte);
            p++;
        } else if (*p >= 'A' && *p <= 'F') {
            uint8_t byte = (*p - 'A' + 10) << 4;
            p++;
            if (*p >= '0' && *p <= '9') {
                byte |= (*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                byte |= (*p - 'a' + 10);
            } else if (*p >= 'A' && *p <= 'F') {
                byte |= (*p - 'A' + 10);
            }
            if (sent_data_len < TARGET_RESPONSE_SIZE - 1) {
                sent_data[sent_data_len++] = byte;
            }
            target_uart_send_byte(byte);
            p++;
        } else {
            p++;
        }
    }

    // Always append \r
    if (sent_data_len < TARGET_RESPONSE_SIZE - 1) {
        sent_data[sent_data_len++] = '\r';
    }
    target_uart_send_byte('\r');

    uart_cli_send("OK: Data sent to target\r\n");

    // External trigger check function
    extern void glitch_check_uart_trigger(uint8_t byte);

    // Wait for response (500ms timeout) - actively poll UART
    uint32_t start = to_ms_since_boot(get_absolute_time());
    uint16_t echo_match_pos = 0;
    bool echo_skipped = false;

    while (to_ms_since_boot(get_absolute_time()) - start < 500) {
        // Read any incoming data
        while (uart_is_readable(TARGET_UART_ID)) {
            uint8_t byte = uart_getc(TARGET_UART_ID);

            // Check for UART trigger byte
            glitch_check_uart_trigger(byte);

            // Display received byte in debug mode
            if (debug_mode) {
                uart_cli_printf("[RX] %02X", byte);
                if (byte >= 32 && byte < 127) {
                    uart_cli_printf(" '%c'", byte);
                }
                uart_cli_send("\r\n");
            }

            // Check if this is part of the echo
            if (!echo_skipped && echo_match_pos < sent_data_len) {
                if (byte == (uint8_t)sent_data[echo_match_pos]) {
                    echo_match_pos++;
                    if (echo_match_pos == sent_data_len) {
                        echo_skipped = true;  // Echo complete, start collecting real response
                    }
                    continue;  // Skip storing echo bytes
                } else {
                    // Not matching echo, must be actual response
                    // Store any previously skipped bytes that weren't echo
                    for (uint16_t i = 0; i < echo_match_pos; i++) {
                        if (target_response_pos < TARGET_RESPONSE_SIZE - 1) {
                            target_response[target_response_pos++] = sent_data[i];
                            target_response_count++;
                        }
                    }
                    echo_skipped = true;  // Stop trying to match echo
                }
            }

            // Store in response buffer (after echo is skipped)
            if (echo_skipped && target_response_pos < TARGET_RESPONSE_SIZE - 1) {
                target_response[target_response_pos++] = byte;
                target_response_count++;
            }
        }
        sleep_us(100);  // Small delay to avoid busy waiting
    }

    // Re-enable UART RX interrupt for trigger detection
    uart_set_irq_enables(TARGET_UART_ID, true, false);

    // Display response
    target_uart_print_response_hex();
}

void target_uart_process(void) {
    // UART RX is now handled by interrupt for minimal trigger latency
    // This function is kept for compatibility but does nothing
    // All RX processing happens in target_uart_irq_handler()
}

uint16_t target_uart_get_response_count(void) {
    return target_response_count;
}

const char* target_uart_get_response(void) {
    target_response[target_response_pos] = '\0';
    return target_response;
}

void target_uart_clear_response(void) {
    memset(target_response, 0, TARGET_RESPONSE_SIZE);
    target_response_pos = 0;
    target_response_count = 0;
}

void target_uart_print_response_hex(void) {
    if (target_response_count == 0) {
        uart_cli_send("No response data\r\n");
        return;
    }

    uart_cli_printf("Response (%u bytes):\r\n", target_response_count);

    // Print each line as received (line-delimited by \n)
    // Output full hex line without breaking into 16-byte chunks
    uint16_t line_start = 0;

    for (uint16_t i = 0; i < target_response_pos; i++) {
        uint8_t byte = (uint8_t)target_response[i];

        // Check for line ending
        if (byte == '\n') {
            // Print all hex bytes for this line
            for (uint16_t j = line_start; j < i; j++) {
                uint8_t line_byte = (uint8_t)target_response[j];
                if (line_byte != '\r') {  // Skip CR
                    uart_cli_printf("%02X ", line_byte);
                }
            }
            uart_cli_send("\r\n");
            line_start = i + 1;
        }
    }

    // Print any remaining bytes after last newline
    if (line_start < target_response_pos) {
        for (uint16_t j = line_start; j < target_response_pos; j++) {
            uint8_t line_byte = (uint8_t)target_response[j];
            if (line_byte != '\r') {  // Skip CR
                uart_cli_printf("%02X ", line_byte);
            }
        }
        uart_cli_send("\r\n");
    }
}

void target_reset_config(uint8_t pin, uint32_t period_ms, bool active_high) {
    // Check if anything has changed
    bool pin_changed = (reset_pin != pin) || !reset_pin_initialized;
    bool polarity_changed = (reset_active_high != active_high);
    bool period_changed = (reset_period_ms != period_ms);
    bool config_changed = pin_changed || polarity_changed || period_changed;

    reset_pin = pin;
    reset_period_ms = period_ms;
    reset_active_high = active_high;

    // Only initialize GPIO if pin or polarity changed
    if (pin_changed || polarity_changed) {
        gpio_init(reset_pin);
        gpio_set_dir(reset_pin, GPIO_OUT);

        // Set pull resistor based on active state
        // Active high: pull down when inactive (default state is LOW)
        // Active low:  pull up when inactive (default state is HIGH)
        if (active_high) {
            gpio_pull_down(reset_pin);
        } else {
            gpio_pull_up(reset_pin);
        }

        gpio_put(reset_pin, reset_active_high ? 0 : 1);  // Inactive state

        // Critical: After Pico reboot, reset pin may have been floating/LOW
        // Give target time to come out of reset before first reset pulse
        sleep_ms(100);

        reset_pin_initialized = true;
    }

    // Only print message if configuration actually changed
    if (config_changed) {
        uart_cli_printf("OK: Reset configured on pin %u, period %u ms, active %s\r\n",
                        pin, period_ms, active_high ? "HIGH" : "LOW");
    }
}

void target_reset_execute(void) {
    // Pulse reset pin
    gpio_put(reset_pin, reset_active_high ? 1 : 0);  // Active state
    sleep_ms(reset_period_ms);
    gpio_put(reset_pin, reset_active_high ? 0 : 1);  // Inactive state

    uart_cli_send("OK: Target reset executed\r\n");
}

bool target_is_initialized(void) {
    return target_initialized;
}

void target_set_debug(bool enable) {
    debug_mode = enable;
}

bool target_get_debug(void) {
    return debug_mode;
}
