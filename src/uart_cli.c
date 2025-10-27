#include "uart_cli.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define CLI_BUFFER_SIZE 256
#define HISTORY_SIZE 10

static char command_buffer[CLI_BUFFER_SIZE];
static uint16_t buffer_pos = 0;
static bool command_ready = false;

// Command history
static char history[HISTORY_SIZE][CLI_BUFFER_SIZE];
static uint8_t history_count = 0;  // Total commands in history (max HISTORY_SIZE)
static uint8_t history_pos = 0;    // Current position when browsing
static bool browsing_history = false;

// ANSI escape sequence state machine
static enum {
    STATE_NORMAL,
    STATE_ESC,
    STATE_CSI
} escape_state = STATE_NORMAL;

void uart_cli_init(void) {
    // Note: stdio is already initialized by stdio_init_all() in main
    // We just need to initialize our buffers

    // Initialize buffer
    memset(command_buffer, 0, CLI_BUFFER_SIZE);
    buffer_pos = 0;
    command_ready = false;

    // Initialize history
    memset(history, 0, sizeof(history));
    history_count = 0;
    history_pos = 0;
    browsing_history = false;
    escape_state = STATE_NORMAL;

    // Send welcome message
    printf("\r\n");
    printf("=================================\r\n");
    printf("  Raiden Pico - Fault Injection\r\n");
    printf("  C/C++ Edition\r\n");
    printf("=================================\r\n");
    printf("> ");
    fflush(stdout);
}

void uart_cli_send(const char *str) {
    printf("%s", str);
    fflush(stdout);
}

void uart_cli_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    fflush(stdout);
}

// Helper function to clear current line and redraw
static void redraw_line(void) {
    // Move cursor to start of line, clear it
    printf("\r\033[K> %s", command_buffer);
    fflush(stdout);
}

// Helper function to add command to history
static void add_to_history(const char *cmd) {
    if (cmd == NULL || cmd[0] == '\0') {
        return;  // Don't add empty commands
    }

    // Don't add if same as last command
    if (history_count > 0 && strcmp(history[0], cmd) == 0) {
        return;
    }

    // Shift history down
    if (history_count < HISTORY_SIZE) {
        history_count++;
    }
    for (int i = history_count - 1; i > 0; i--) {
        strcpy(history[i], history[i-1]);
    }

    // Add new command at position 0
    strncpy(history[0], cmd, CLI_BUFFER_SIZE - 1);
    history[0][CLI_BUFFER_SIZE - 1] = '\0';
}

void uart_cli_process(void) {
    // Check if command is already ready (not yet processed)
    if (command_ready) {
        return;
    }

    // Read all available characters using stdio (works with both USB and UART)
    int c;
    while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {

        // ANSI escape sequence state machine for arrow keys
        // Arrow keys send: ESC [ A (up), ESC [ B (down), ESC [ C (right), ESC [ D (left)
        if (escape_state == STATE_NORMAL && c == 27) {  // ESC
            escape_state = STATE_ESC;
            continue;
        } else if (escape_state == STATE_ESC && c == '[') {  // CSI
            escape_state = STATE_CSI;
            continue;
        } else if (escape_state == STATE_CSI) {
            // Process CSI sequence
            escape_state = STATE_NORMAL;

            if (c == 'A') {  // Up arrow
                if (history_count > 0) {
                    if (!browsing_history) {
                        browsing_history = true;
                        history_pos = 0;
                    } else if (history_pos < history_count - 1) {
                        history_pos++;
                    }

                    // Load history entry
                    strncpy(command_buffer, history[history_pos], CLI_BUFFER_SIZE - 1);
                    command_buffer[CLI_BUFFER_SIZE - 1] = '\0';
                    buffer_pos = strlen(command_buffer);
                    redraw_line();
                }
                continue;
            } else if (c == 'B') {  // Down arrow
                if (browsing_history) {
                    if (history_pos > 0) {
                        history_pos--;
                        strncpy(command_buffer, history[history_pos], CLI_BUFFER_SIZE - 1);
                        command_buffer[CLI_BUFFER_SIZE - 1] = '\0';
                        buffer_pos = strlen(command_buffer);
                    } else {
                        // Back to empty line
                        browsing_history = false;
                        command_buffer[0] = '\0';
                        buffer_pos = 0;
                    }
                    redraw_line();
                }
                continue;
            }
            // Ignore other CSI sequences (right/left arrows, etc.)
            continue;
        }

        // Reset escape state if we get here with non-escape character
        escape_state = STATE_NORMAL;

        // Handle special characters
        if (c == '\r' || c == '\n') {
            // End of command
            if (buffer_pos > 0) {
                command_buffer[buffer_pos] = '\0';
                add_to_history(command_buffer);
                command_ready = true;
                browsing_history = false;
                uart_cli_send("\r\n");
            } else {
                // Empty line, just show prompt
                uart_cli_send("\r\n> ");
            }
            return;
        } else if (c == '\b' || c == 127) {
            // Backspace or DEL
            if (buffer_pos > 0) {
                buffer_pos--;
                command_buffer[buffer_pos] = '\0';
                // Echo backspace sequence
                uart_cli_send("\b \b");
                browsing_history = false;
            }
        } else if (c == 3) {
            // Ctrl+C - clear buffer
            buffer_pos = 0;
            command_buffer[0] = '\0';
            browsing_history = false;
            uart_cli_send("^C\r\n> ");
        } else if (c >= 32 && c < 127) {
            // Printable character
            if (buffer_pos < CLI_BUFFER_SIZE - 1) {
                command_buffer[buffer_pos++] = (char)c;
                command_buffer[buffer_pos] = '\0';
                // Echo character
                putchar(c);
                fflush(stdout);
                browsing_history = false;
            }
        }
    }
}

bool uart_cli_command_ready(void) {
    return command_ready;
}

const char* uart_cli_get_command(void) {
    return command_buffer;
}

void uart_cli_clear_command(void) {
    memset(command_buffer, 0, CLI_BUFFER_SIZE);
    buffer_pos = 0;
    command_ready = false;
    uart_cli_send("> ");
}
