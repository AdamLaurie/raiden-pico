#ifndef UART_CLI_H
#define UART_CLI_H

#include <stdint.h>
#include <stdbool.h>

// Initialize the CLI UART interface
void uart_cli_init(void);

// Send a string to the CLI
void uart_cli_send(const char *str);

// Send a formatted string to the CLI
void uart_cli_printf(const char *format, ...);

// Process incoming characters (call from main loop)
void uart_cli_process(void);

// Check if a command is ready
bool uart_cli_command_ready(void);

// Get the current command buffer
const char* uart_cli_get_command(void);

// Clear the command buffer
void uart_cli_clear_command(void);

#endif // UART_CLI_H
