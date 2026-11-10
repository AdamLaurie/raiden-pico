#include "config.h"
#include "uart_cli.h"
#include "uart_mux.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>

// ChipShouter UART configuration - use values from config.h
// UART0 on GP0/GP1
#include "config.h"
// Using CHIPSHOT_UART_ID, CHIPSHOT_UART_TX_PIN, CHIPSHOT_UART_RX_PIN from config.h

// Response buffer
#define RESPONSE_BUFFER_SIZE 256
static char response_buffer[RESPONSE_BUFFER_SIZE];
static uint16_t response_pos = 0;
static bool response_ready = false;
static uint32_t last_rx_time = 0;  // Track last character received time

// ChipShouter state
static bool chipshot_armed = false;

void chipshot_uart_init(void) {
    // Claim UART0 on GP0/GP1 through uart_mux (8N1, FIFO on, no flow control).
    // ChipSHOUTER is UART0's only owner today; going through the mux keeps UART0
    // under the same rules as UART1 if another UART0 alternate is ever added.
    uart_mux_set_line(UART_OWNER_CHIPSHOT, CHIPSHOT_UART_BAUD, 8, UART_PARITY_NONE, 1);
    uart_mux_reclaim(UART_OWNER_CHIPSHOT);

    // Clear any stale data in RX FIFO
    while (uart_is_readable(CHIPSHOT_UART_ID)) {
        uart_getc(CHIPSHOT_UART_ID);
    }

    // Clear response buffer
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);
    response_pos = 0;
    response_ready = false;

    chipshot_armed = false;
}

void chipshot_uart_send(const char *data) {
    uart_mux_claim(UART_OWNER_CHIPSHOT);   // no-op while ChipSHOUTER owns UART0
    uart_puts(CHIPSHOT_UART_ID, data);
    // Wait for TX FIFO to drain to ensure command is fully sent
    uart_tx_wait_blocking(CHIPSHOT_UART_ID);
}

void chipshot_uart_process(void) {
    // Read incoming data from ChipShouter
    // ChipShouter sends multi-line responses, so we need to collect all lines
    // A response is complete when no new data arrives for 50ms

    // Only drain UART0 while ChipSHOUTER owns it (it always does today; the
    // check keeps a future UART0 owner's bytes from being eaten here).
    if (!uart_mux_is_owner(UART_OWNER_CHIPSHOT)) return;

    // Don't read new data if response is already ready (prevents corruption)
    if (!response_ready) {
        while (uart_is_readable(CHIPSHOT_UART_ID)) {
            char c = uart_getc(CHIPSHOT_UART_ID);
            last_rx_time = to_ms_since_boot(get_absolute_time());

            // Accumulate characters into buffer
            if (response_pos < RESPONSE_BUFFER_SIZE - 1) {
                // Keep newlines in the response to preserve multi-line structure
                response_buffer[response_pos++] = c;
            }
        }
    }

    // Check if we have data and enough time has passed since last character
    if (response_pos > 0 && !response_ready && last_rx_time > 0) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_rx_time >= 200) {  // 200ms timeout for slower responses
            // Null terminate
            response_buffer[response_pos] = '\0';

            // Strip ChipSHOUTER prompt lines ("# armed:" or "# disarmed:")
            // Search backwards for the last line
            char *last_line = NULL;
            for (int i = response_pos - 1; i >= 0; i--) {
                if (response_buffer[i] == '\n' || i == 0) {
                    last_line = (i == 0) ? response_buffer : &response_buffer[i + 1];
                    break;
                }
            }

            // If last line is a prompt, remove it
            if (last_line && (strstr(last_line, "# armed:") == last_line ||
                             strstr(last_line, "# disarmed:") == last_line)) {
                // Truncate at the newline before the prompt
                if (last_line != response_buffer) {
                    *(last_line - 1) = '\0';
                    response_pos = last_line - response_buffer - 1;
                } else {
                    // Prompt is the only content, clear it
                    response_buffer[0] = '\0';
                    response_pos = 0;
                }
            }

            response_ready = true;
        }
    }
}

bool chipshot_uart_response_ready(void) {
    return response_ready;
}

const char* chipshot_uart_get_response(void) {
    return response_buffer;
}

void chipshot_uart_clear_response(void) {
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);
    response_pos = 0;
    response_ready = false;
    last_rx_time = 0;
}

const char* chipshot_uart_read_response_blocking(uint32_t timeout_ms) {
    // Clear any previous response
    chipshot_uart_clear_response();

    uint32_t start_time = to_ms_since_boot(get_absolute_time());
    uint32_t last_char_time = 0;

    // Continuously read UART in a tight loop
    while (1) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        // Check for overall timeout
        if (now - start_time >= timeout_ms) {
            return NULL;  // Timeout - no response
        }

        // Read all available characters
        while (uart_is_readable(CHIPSHOT_UART_ID)) {
            char c = uart_getc(CHIPSHOT_UART_ID);
            last_char_time = to_ms_since_boot(get_absolute_time());

            // Accumulate characters into buffer
            if (response_pos < RESPONSE_BUFFER_SIZE - 1) {
                response_buffer[response_pos++] = c;
            }
        }

        // Check if we have data and enough time has passed since last character
        if (response_pos > 0 && last_char_time > 0) {
            now = to_ms_since_boot(get_absolute_time());
            if (now - last_char_time >= 200) {  // 200ms timeout for complete response
                // Null terminate
                response_buffer[response_pos] = '\0';

                // Strip ChipSHOUTER prompt lines ("# armed:" or "# disarmed:")
                char *last_line = NULL;
                for (int i = response_pos - 1; i >= 0; i--) {
                    if (response_buffer[i] == '\n' || i == 0) {
                        last_line = (i == 0) ? response_buffer : &response_buffer[i + 1];
                        break;
                    }
                }

                // If last line is a prompt, remove it
                if (last_line && (strstr(last_line, "# armed:") == last_line ||
                                 strstr(last_line, "# disarmed:") == last_line)) {
                    // Truncate at the newline before the prompt
                    if (last_line != response_buffer) {
                        *(last_line - 1) = '\0';
                        response_pos = last_line - response_buffer - 1;
                    } else {
                        // Prompt is the only content, clear it
                        response_buffer[0] = '\0';
                        response_pos = 0;
                    }
                }

                response_ready = true;
                return response_buffer;
            }
        }
    }
}

void chipshot_arm(void) {
    chipshot_uart_send("arm\n");
    chipshot_armed = true;
}

void chipshot_disarm(void) {
    chipshot_uart_send("disarm\n");
    chipshot_armed = false;
}

void chipshot_fire(void) {
    chipshot_uart_send("pulse\n");
}

void chipshot_set_voltage(uint32_t voltage) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "set voltage %u\n", voltage);
    chipshot_uart_send(cmd);
}

void chipshot_set_pulse(uint32_t pulse_ns) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "set pulse width %u\n", pulse_ns);
    chipshot_uart_send(cmd);
}

void chipshot_get_status(void) {
    chipshot_uart_send("get state\n");
}

void chipshot_get_hv_out(void) {
    // "get voltage" reports the capacitor-bank charge voltage: the set value
    // plus the actual MEASURED HV when armed (invalid/off when disarmed).
    // (Was wrongly sending "help", which dumped the console command list.)
    chipshot_uart_send("get voltage\n");
}

void chipshot_get_faults(void) {
    // ChipSHOUTER serial console uses "fault" (singular), not the Python API's
    // "faults_current". "get fault" prints the current state of all faults.
    // (See the CW520 manual serial command list.)
    chipshot_uart_send("get fault\n");
}

bool chipshot_is_armed(void) {
    return chipshot_armed;
}

void chipshot_reset(void) {
    chipshot_uart_send("reset\n");
}

void chipshot_set_trigger_hw(bool active_high) {
    extern glitch_config_t* glitch_get_config(void);
    glitch_config_t *cfg = glitch_get_config();

    // High-impedance (~2KOhm) trigger input so the Pico's 3.3V GP2 GPIO can cross
    // the ChipSHOUTER's 2V trigger threshold. The 50Ohm default (which a CS RESET
    // restores) pulls a weak GPIO below threshold, so the CS arms + charges but
    // never fires. hwtrig_term takes 1/0; 0 = high-impedance.
    chipshot_uart_send("set hwtrig_term 0\r\n");
    sleep_ms(50);

    // Configure pull resistor on glitch output pin based on active high/low
    if (active_high) {
        // Active high: configure pull-down so pin idles low
        gpio_pull_down(PIN_GLITCH_OUT);
        chipshot_uart_send("set hwtrig_mode 1\r\n");
    } else {
        // Active low: configure pull-up so pin idles high
        gpio_pull_up(PIN_GLITCH_OUT);
        chipshot_uart_send("set hwtrig_mode 0\r\n");
    }
}

void chipshot_set_trigger_sw(void) {
    // Software-triggered mode: fire via the "pulse" command (chipshot_fire).
    // Terminate the unused hardware-trigger SMB input at 50Ohm so a floating
    // input can't trigger stray pulses. hwtrig_term takes 1/0 (1 = 50Ohm term).
    // (Was sending "set hwtrig_term True" — wrong, takes 1/0 — and "set emode
    //  True", which the ChipSHOUTER console does not recognize.)
    chipshot_uart_send("set hwtrig_term 1\r\n");
    // Note: In SW mode, glitches must be fired by calling chipshot_fire().
}
