#include "config.h"
#include "uart_cli.h"
#include "swd.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
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

// Power configuration - 3 pins ganged for ~36mA total (3x 12mA drive)
#define POWER_PIN1  10  // GP10
#define POWER_PIN2  11  // GP11
#define POWER_PIN3  12  // GP12
#define POWER_MASK  ((1u << POWER_PIN1) | (1u << POWER_PIN2) | (1u << POWER_PIN3))
static uint32_t power_cycle_time_ms = 300;  // Default 300ms cycle time
static bool power_pin_initialized = false;

// Debug mode
static bool debug_mode = false;

// Transparent bridge timeout (milliseconds)
static uint32_t bridge_timeout_ms = 50;

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

    // Initialize power pins - ganged for higher current, default ON
    const uint8_t power_pins[] = {POWER_PIN1, POWER_PIN2, POWER_PIN3};
    for (int i = 0; i < 3; i++) {
        gpio_init(power_pins[i]);
        gpio_set_dir(power_pins[i], GPIO_OUT);
        gpio_set_drive_strength(power_pins[i], GPIO_DRIVE_STRENGTH_12MA);
    }
    gpio_set_mask(POWER_MASK);  // All ON
    power_pin_initialized = true;

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

        case TARGET_STM32F1:
        case TARGET_STM32F3:
        case TARGET_STM32F4:
        case TARGET_STM32L4: {
            uart_cli_send("Entering STM32 bootloader mode...\r\n");
            uart_cli_send("Sending 0x7F for STM32 sync...\r\n");
            // Send 0x7F for STM32 bootloader sync
            target_uart_send_byte(0x7F);

            // Wait for ACK (0x79) or NACK (0x1F) with timeout
            uint32_t start = to_ms_since_boot(get_absolute_time());
            uint32_t timeout_ms = 1000;  // 1 second timeout for bootloader response
            bool got_response = false;

            while (to_ms_since_boot(get_absolute_time()) - start < timeout_ms) {
                if (uart_is_readable(TARGET_UART_ID)) {
                    uint8_t byte = uart_getc(TARGET_UART_ID);
                    if (debug_mode) {
                        uart_cli_printf("[RX] %02X", byte);
                        if (byte >= 32 && byte < 127) {
                            uart_cli_printf(" '%c'", byte);
                        }
                        uart_cli_send("\r\n");
                    }

                    if (byte == 0x79) {
                        uart_cli_send("ACK received\r\n");
                        got_response = true;
                        break;
                    } else if (byte == 0x1F) {
                        uart_cli_send("ERROR: NACK received - bootloader rejected sync\r\n");
                        return false;
                    }
                }
                tight_loop_contents();
            }

            if (!got_response) {
                uart_cli_send("ERROR: No response from bootloader (check BOOT0 pin and connections)\r\n");
                return false;
            }
            break;
        }

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

    // If Grbl UART is active, deinitialize it first
    // Grbl uses UART1 on GP8/GP9, we need UART1 on GP4/GP5
    extern bool grbl_is_active(void);
    extern void grbl_deinit(void);
    if (grbl_is_active()) {
        grbl_deinit();
    }

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

    // Initialize GPIO pins before UART (like SDK examples)
    gpio_init(tx_pin);
    gpio_init(rx_pin);

    // Initialize UART1
    uart_init(TARGET_UART_ID, baud);

    // Set UART format: STM32 bootloader requires EVEN parity, others use no parity
    if (target_is_stm32(current_target_type)) {
        uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_EVEN);
    } else {
        uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    }

    // Set TX and RX pins for UART1 (GP4/GP5 are default UART1 pins)
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

    // Disable UART RX interrupt
    uart_set_irq_enables(TARGET_UART_ID, false, false);

    // Send command to target
    const char *p = str;
    while (*p) {
        uart_putc_raw(TARGET_UART_ID, *p);
        p++;
    }
    // Append \r
    uart_putc_raw(TARGET_UART_ID, '\r');
    uart_tx_wait_blocking(TARGET_UART_ID);

    // External trigger check function
    extern void glitch_check_uart_trigger(uint8_t byte);

    // Transparent bridge: forward ALL bytes from target to host (raw, no processing)
    // Timeout resets on each received byte
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (to_ms_since_boot(get_absolute_time()) - start < bridge_timeout_ms) {
        // Read any incoming data from target
        while (uart_is_readable(TARGET_UART_ID)) {
            uint8_t byte = uart_getc(TARGET_UART_ID);

            // Reset timeout on any data received
            start = to_ms_since_boot(get_absolute_time());

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

            // Forward raw byte directly to host (transparent bridge)
            putchar_raw(byte);
        }
    }

    // Re-enable UART RX interrupt for trigger detection
    uart_set_irq_enables(TARGET_UART_ID, true, false);
}

void target_uart_send_hex(const char *hex_str) {
    // Auto-initialize UART with defaults if not already initialized
    if (!target_initialized) {
        target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, target_baud);
    }

    // Disable UART RX interrupt
    uart_set_irq_enables(TARGET_UART_ID, false, false);

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
            uart_putc_raw(TARGET_UART_ID, byte);
            if (debug_mode) {
                uart_cli_printf("[TX] %02X\r\n", byte);
            }
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
            uart_putc_raw(TARGET_UART_ID, byte);
            if (debug_mode) {
                uart_cli_printf("[TX] %02X\r\n", byte);
            }
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
            uart_putc_raw(TARGET_UART_ID, byte);
            if (debug_mode) {
                uart_cli_printf("[TX] %02X\r\n", byte);
            }
            p++;
        } else {
            p++;
        }
    }

    // Wait for TX to complete (no \r append for raw hex - STM32 bootloader needs exact bytes)
    uart_tx_wait_blocking(TARGET_UART_ID);

    // External trigger check function
    extern void glitch_check_uart_trigger(uint8_t byte);

    // Transparent bridge: forward ALL bytes from target to host (raw, no processing)
    // Timeout resets on each received byte
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (to_ms_since_boot(get_absolute_time()) - start < bridge_timeout_ms) {
        // Read any incoming data from target
        while (uart_is_readable(TARGET_UART_ID)) {
            uint8_t byte = uart_getc(TARGET_UART_ID);

            // Reset timeout on any data received
            start = to_ms_since_boot(get_absolute_time());

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

            // Forward raw byte directly to host (transparent bridge)
            putchar_raw(byte);
        }
    }

    // Re-enable UART RX interrupt for trigger detection
    uart_set_irq_enables(TARGET_UART_ID, true, false);
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

        // Disable pull resistors to avoid interfering with edge detection
        gpio_disable_pulls(reset_pin);

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

void target_set_timeout(uint32_t timeout_ms) {
    bridge_timeout_ms = timeout_ms;
}

uint32_t target_get_timeout(void) {
    return bridge_timeout_ms;
}

static void power_ensure_init(void) {
    if (!power_pin_initialized) {
        const uint8_t pins[] = {POWER_PIN1, POWER_PIN2, POWER_PIN3};
        for (int i = 0; i < 3; i++) {
            gpio_init(pins[i]);
            gpio_set_dir(pins[i], GPIO_OUT);
            gpio_set_drive_strength(pins[i], GPIO_DRIVE_STRENGTH_12MA);
        }
        gpio_set_mask(POWER_MASK);  // Default ON
        power_pin_initialized = true;
    }
}

void target_power_on(void) {
    power_ensure_init();
    gpio_set_mask(POWER_MASK);
    uart_cli_send("OK: Target power ON\r\n");
}

void target_power_off(void) {
    power_ensure_init();
    gpio_clr_mask(POWER_MASK);
    uart_cli_send("OK: Target power OFF\r\n");
}

void target_power_cycle(uint32_t time_ms) {
    power_ensure_init();
    gpio_clr_mask(POWER_MASK);
    uart_cli_printf("OK: Target power cycling (OFF for %u ms)...\r\n", time_ms);
    sleep_ms(time_ms);
    gpio_set_mask(POWER_MASK);
    uart_cli_send("OK: Target power ON\r\n");
}

bool target_power_get_state(void) {
    power_ensure_init();
    return gpio_get(POWER_PIN1);
}

// ADC configuration for voltage monitoring
#define ADC_POWER_PIN   26  // GP26 = ADC0, connected to target VDD
#define ADC_POWER_CHAN  0

// SRAM test pattern
#define SRAM_TEST_WORDS 256
#define SRAM_TEST_PATTERN 0xDEAD0000u

static void adc_power_init(void) {
    adc_init();
    adc_gpio_init(ADC_POWER_PIN);
    adc_select_input(ADC_POWER_CHAN);
}

static float adc_read_voltage(void) {
    adc_select_input(ADC_POWER_CHAN);
    uint16_t raw = adc_read();
    return raw * 3.3f / 4095.0f;
}

static int sram_test_write_pattern(uint32_t sram_base) {
    uint32_t pattern[SRAM_TEST_WORDS];
    for (int i = 0; i < SRAM_TEST_WORDS; i++)
        pattern[i] = SRAM_TEST_PATTERN | (uint32_t)i;

    // Write pattern
    uint32_t written = swd_write_mem(sram_base, pattern, SRAM_TEST_WORDS);
    if (written != SRAM_TEST_WORDS) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%d words)\r\n", written, SRAM_TEST_WORDS);
        return -1;
    }

    // Verify write
    uint32_t readback[SRAM_TEST_WORDS];
    uint32_t read = swd_read_mem(sram_base, readback, SRAM_TEST_WORDS);
    if (read != SRAM_TEST_WORDS) {
        uart_cli_send("ERROR: SRAM verify read failed\r\n");
        return -1;
    }

    int bad = 0;
    for (int i = 0; i < SRAM_TEST_WORDS; i++) {
        if (readback[i] != pattern[i]) bad++;
    }
    if (bad) {
        uart_cli_printf("ERROR: SRAM verify failed: %d/%d words bad\r\n", bad, SRAM_TEST_WORDS);
        return -1;
    }

    return 0;
}

static int sram_test_read_pattern(uint32_t sram_base) {
    uint32_t readback[SRAM_TEST_WORDS];
    uint32_t read = swd_read_mem(sram_base, readback, SRAM_TEST_WORDS);
    if (read != SRAM_TEST_WORDS)
        return -1;

    int good = 0;
    for (int i = 0; i < SRAM_TEST_WORDS; i++) {
        uint32_t expected = SRAM_TEST_PATTERN | (uint32_t)i;
        if (readback[i] == expected) good++;
    }
    return good;
}

/*
 * SRAM retention sweep — ADC-threshold version (from stimpik).
 *
 * For each step:
 *   1. Write test pattern to SRAM via SWD
 *   2. Float SWD pins, set NRST as input
 *   3. Drop power (clear all power GPIOs)
 *   4. Poll ADC in tight loop until voltage drops to threshold
 *   5. Immediately restore all power GPIOs
 *   6. Monitor NRST for BOR detection
 *   7. Re-attach SWD, read back SRAM, count retained words
 *
 * Sweep threshold from ~2.5V down in ~0.08V steps.
 * High threshold = shallow dip (SRAM survives).
 * Low threshold = deep dip (eventually corrupts SRAM or triggers BOR).
 */
void target_power_sweep(void) {
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern void swd_init(void);
    extern void swd_deinit(void);

    const stm32_target_info_t *info = stm32_get_target_info(current_target_type);
    if (!info) {
        uart_cli_send("ERROR: No target type set. Use TARGET or SWD IDCODE first\r\n");
        return;
    }

    uint32_t sram_base = info->sram_base;
    uart_cli_printf("SRAM retention sweep on %s (SRAM @ 0x%08lX)\r\n", info->name, sram_base);
    uart_cli_send("Sweep: drop power, poll ADC to threshold, restore, check SRAM\r\n");

    adc_power_init();
    power_ensure_init();

    #define SWEEP_MAX 64
    #define ADC_LOG_SIZE 256

    struct {
        uint32_t thresh;
        uint16_t vmin_raw;
        int16_t good;           // -1 = read failed
        uint32_t glitch_us;
        bool nrst;
        uint16_t bor_adc;
    } results[SWEEP_MAX];
    uint32_t result_count = 0;

    uint16_t adc_log[ADC_LOG_SIZE];

    /* Sweep from ~2.5V down in steps of ~0.08V */
    for (uint32_t thresh = 3103; thresh >= 100; thresh -= 100) {
        uart_cli_printf("\r\n--- Threshold: %lu (%.2fV) ---\r\n",
                        thresh, thresh * 3.3f / 4095.0f);

        // Connect and halt target
        swd_init();
        if (!swd_connect()) {
            uart_cli_send("ERROR: SWD connect failed\r\n");
            swd_deinit();
            break;
        }
        swd_halt();

        // Write test pattern
        if (sram_test_write_pattern(sram_base) != 0) {
            uart_cli_send("Write failed, aborting sweep\r\n");
            swd_deinit();
            break;
        }

        // Float SWD pins before glitch
        swd_deinit();

        // Ensure NRST is input with pull-up
        gpio_init(reset_pin);
        gpio_set_dir(reset_pin, GPIO_IN);
        gpio_pull_up(reset_pin);

        // --- GLITCH: float ALL power pins, let target drain its own caps ---
        // Like stimpik: set all power GPIOs to INPUT (high-Z).
        // Target's quiescent current slowly drains decoupling caps.
        // ADC polls until voltage drops to threshold, then restore.
        gpio_set_dir(POWER_PIN1, GPIO_IN);
        gpio_set_dir(POWER_PIN2, GPIO_IN);
        gpio_set_dir(POWER_PIN3, GPIO_IN);
        gpio_disable_pulls(POWER_PIN1);
        gpio_disable_pulls(POWER_PIN2);
        gpio_disable_pulls(POWER_PIN3);

        adc_select_input(ADC_POWER_CHAN);
        uint32_t adc_log_count = 0;

        // Pre-set output latch low (doesn't drive — pins are INPUT)
        uint64_t t0 = time_us_64();
        gpio_clr_mask(POWER_MASK);

        // Poll ADC until voltage drops below threshold
        // Timeout after 500ms if threshold can't be reached
        bool thresh_reached = true;
        while (true) {
            uint16_t val = adc_read();
            if (adc_log_count < ADC_LOG_SIZE)
                adc_log[adc_log_count++] = val;
            if (val <= thresh)
                break;
            if (time_us_64() - t0 > 500000) {
                thresh_reached = false;
                break;
            }
        }

        // Immediately restore ALL power pins (direction back to OUTPUT + drive HIGH)
        gpio_set_dir(POWER_PIN1, GPIO_OUT);
        gpio_set_dir(POWER_PIN2, GPIO_OUT);
        gpio_set_dir(POWER_PIN3, GPIO_OUT);
        gpio_set_mask(POWER_MASK);
        uint32_t glitch_us_actual = (uint32_t)(time_us_64() - t0);

        // Monitor NRST IMMEDIATELY after restore (before any other processing)
        // Matches stimpik: NRST check must happen first to catch BOR pulse
        bool nrst_went_low = false;
        uint32_t nrst_low_us = 0;
        uint16_t bor_adc = 0;
        uint64_t t_glitch = time_us_64();
        for (int i = 0; i < 5000; i++) {
            if (!gpio_get(reset_pin)) {
                if (!nrst_went_low) {
                    nrst_low_us = (uint32_t)(time_us_64() - t_glitch);
                    bor_adc = adc_read();
                    nrst_went_low = true;
                }
            }
            sleep_us(10);
        }

        // Now find actual Vmin from ADC log
        uint16_t vmin_raw = 4095;
        for (uint32_t i = 0; i < adc_log_count; i++) {
            if (adc_log[i] < vmin_raw)
                vmin_raw = adc_log[i];
        }

        // Report glitch stats
        // Report glitch stats
        if (!thresh_reached) {
            uart_cli_printf("Timeout: Vmin=%.2fV (can't reach %.2fV), NRST: %s\r\n",
                            vmin_raw * 3.3f / 4095.0f,
                            thresh * 3.3f / 4095.0f,
                            nrst_went_low ? "TRIGGERED" : "no reset");
        } else {
            uart_cli_printf("Glitch: %luus, Vmin=%.2fV, NRST: %s",
                            glitch_us_actual,
                            vmin_raw * 3.3f / 4095.0f,
                            nrst_went_low ? "TRIGGERED" : "no reset");
            if (nrst_went_low)
                uart_cli_printf(" (after %luus, BOR@%.2fV)",
                                nrst_low_us, bor_adc * 3.3f / 4095.0f);
            uart_cli_send("\r\n");
        }

        // Wait for target to stabilize
        sleep_ms(50);

        // Re-attach via SWD and read back
        swd_init();
        int good = -1;
        if (swd_connect()) {
            swd_halt();
            good = sram_test_read_pattern(sram_base);
        }
        swd_deinit();

        if (good < 0) {
            uart_cli_send("Read failed (can't attach after glitch)\r\n");
        } else {
            uart_cli_printf("Result: %d/%d words retained at %.2fV threshold\r\n",
                            good, SRAM_TEST_WORDS, thresh * 3.3f / 4095.0f);
        }

        // Record result
        if (result_count < SWEEP_MAX) {
            results[result_count].thresh = thresh;
            results[result_count].vmin_raw = vmin_raw;
            results[result_count].good = (int16_t)good;
            results[result_count].glitch_us = glitch_us_actual;
            results[result_count].nrst = nrst_went_low;
            results[result_count].bor_adc = bor_adc;
            result_count++;
        }

        // Stop if threshold not reached or corruption/failure
        if (!thresh_reached) {
            uart_cli_printf("*** Voltage floor reached at %.2fV, stopping sweep ***\r\n",
                            vmin_raw * 3.3f / 4095.0f);
            break;
        }
        if (good < SRAM_TEST_WORDS) {
            uart_cli_send("*** Corruption detected, stopping sweep ***\r\n");
            break;
        }

        sleep_ms(200);  // Let target fully recover
    }

    // Print summary table
    uart_cli_send("\r\n=== SRAM Retention Sweep Summary ===\r\n");
    uart_cli_send("Thresh(V)  Vmin(V)  Glitch(us)  NRST  BOR(V)  Retained\r\n");
    uart_cli_send("---------  ------   ----------  ----  ------  --------\r\n");
    for (uint32_t i = 0; i < result_count; i++) {
        uart_cli_printf("  %.2fV     %.2fV    %5lu       %s   ",
                        results[i].thresh * 3.3f / 4095.0f,
                        results[i].vmin_raw * 3.3f / 4095.0f,
                        results[i].glitch_us,
                        results[i].nrst ? "Y" : "N");
        if (results[i].nrst)
            uart_cli_printf("%.2fV  ", results[i].bor_adc * 3.3f / 4095.0f);
        else
            uart_cli_send("  --   ");
        if (results[i].good < 0)
            uart_cli_send("FAIL\r\n");
        else
            uart_cli_printf("%d/%d\r\n", results[i].good, SRAM_TEST_WORDS);
    }

    // Report BOR threshold if detected
    float bor_lo = 0, bor_hi = 0;
    for (uint32_t i = 0; i < result_count; i++) {
        if (results[i].nrst) {
            bor_lo = results[i].bor_adc * 3.3f / 4095.0f;
            if (i > 0)
                bor_hi = results[i - 1].vmin_raw * 3.3f / 4095.0f;
            break;
        }
    }
    if (bor_lo > 0) {
        uart_cli_printf("\r\nBOR threshold: ~%.2fV", bor_lo);
        if (bor_hi > 0)
            uart_cli_printf(" (last no-reset Vmin: %.2fV)", bor_hi);
        uart_cli_send("\r\n");
    } else {
        uart_cli_send("\r\nBOR not triggered during sweep\r\n");
    }

    // Ensure power is restored and reset pin back to normal
    gpio_set_mask(POWER_MASK);
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_disable_pulls(reset_pin);
    gpio_put(reset_pin, reset_active_high ? 0 : 1);

    uart_cli_send("Sweep complete, power restored\r\n");
}
