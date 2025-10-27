#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "uart_cli.h"
#include "command_parser.h"
#include "glitch.h"
#include "config.h"
#include <stdio.h>

// Forward declarations for platform and UART modules
extern void platform_init(void);
extern void chipshot_uart_init(void);
extern void chipshot_uart_process(void);
extern void target_uart_process(void);
extern void target_init(void);

// LED pin for status indication
#define LED_PIN 25

int main() {
    // Initialize standard I/O
    stdio_init_all();

    // Small delay for USB serial to stabilize
    sleep_ms(2000);

    // Send early test message
    printf("Raiden Pico starting...\n");

    // Initialize LED
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);  // Turn on LED

    printf("LED initialized\n");

    // Initialize subsystems one by one with debug output
    printf("Initializing command parser...\n");
    command_parser_init();

    printf("Initializing UART CLI...\n");
    uart_cli_init();

    printf("Initializing glitch...\n");
    glitch_init();

    printf("Initializing platform...\n");
    platform_init();

    printf("Initializing ChipShouter UART...\n");
    chipshot_uart_init();

    printf("Initializing target subsystem...\n");
    target_init();

    printf("All systems initialized!\n");

    // Blink LED to indicate ready
    for (int i = 0; i < 3; i++) {
        gpio_put(LED_PIN, 0);
        sleep_ms(100);
        gpio_put(LED_PIN, 1);
        sleep_ms(100);
    }

    printf("Ready!\n");

    // Main loop
    while (true) {
        // Process UART CLI
        uart_cli_process();

        // Check if command is ready
        if (uart_cli_command_ready()) {
            const char *cmd = uart_cli_get_command();

            // Parse command
            cmd_parts_t parts;
            if (command_parser_parse(cmd, &parts)) {
                // Execute command
                command_parser_execute(&parts);
            }

            // Clear command buffer
            uart_cli_clear_command();
        }

        // Process ChipShouter UART
        chipshot_uart_process();

        // Process Target UART
        target_uart_process();

        // Update glitch flags
        glitch_update_flags();

        // Small delay to prevent busy waiting
        sleep_us(100);
    }

    return 0;
}
