#include "psu.h"
#include "config.h"
#include "uart_cli.h"

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// External PSU driver — TENMA 72-2540 / Korad ASCII protocol over UART1 on
// GP10/GP11. See psu.h for the mutual-exclusion contract with the power group.
//
// Protocol verified against a real TENMA 72-2540 V5.9 (via USB-RS232): 9600 8N1,
// commands sent with NO terminator, queries reply with no terminator after a
// short latency (*IDN? variable; VSET1?/ISET1?/VOUT1?/IOUT1? are 5 chars;
// STATUS? is 1 byte). Write format VSET1:NN.NN / ISET1:N.NNN accepted as-is.
// PSU_CMD_TERM is left configurable for Korad clones that need a CR.
// ---------------------------------------------------------------------------

#define PSU_CMD_TERM      ""     // TENMA 72-2540: no terminator (verified). Some clones want "\r".
#define PSU_CMD_GAP_MS    60     // min gap the unit needs between commands
#define PSU_READ_IDLE_MS  120    // stop reading after this idle gap
#define PSU_READ_TOTAL_MS 400    // hard cap on a single query read

static bool psu_active = false;

// Provided by target_uart.c: current supply on/off state, and a release of the
// GP10/11/12 power-group GPIOs so we can retask GP10/11 as UART.
extern bool target_power_get_state(void);
extern void power_group_release(void);

bool psu_is_active(void) { return psu_active; }

bool psu_begin(void) {
    if (psu_active)
        return true;

    // GP10/11 are shared with the target power group — refuse if it's live.
    if (target_power_get_state()) {
        uart_cli_send("ERROR: target power is ON (GP10/11/12); TARGET POWER OFF "
                      "before using the PSU\r\n");
        return false;
    }

    // Hand the power-group pins back to a clean state, then retask GP10/11 as
    // UART1. Release the other UART1 alternates (target GP4/5, GRBL GP8/9) so
    // only GP10/11 route to the peripheral.
    power_group_release();
    uart_deinit(PSU_UART_ID);
    const uint8_t drop[] = {4, 5, 8, 9};
    for (unsigned i = 0; i < sizeof(drop); i++) {
        gpio_set_function(drop[i], GPIO_FUNC_SIO);
        gpio_set_dir(drop[i], GPIO_IN);
    }

    gpio_init(PSU_UART_TX_PIN);
    gpio_init(PSU_UART_RX_PIN);
    uart_init(PSU_UART_ID, PSU_UART_BAUD);
    uart_set_format(PSU_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(PSU_UART_TX_PIN, UART_FUNCSEL_NUM(PSU_UART_ID, PSU_UART_TX_PIN));
    gpio_set_function(PSU_UART_RX_PIN, UART_FUNCSEL_NUM(PSU_UART_ID, PSU_UART_RX_PIN));

    psu_active = true;
    uart_cli_printf("PSU UART claimed: UART1 on GP%d(TX)/GP%d(RX) @ %d 8N1 "
                    "(power group released)\r\n",
                    PSU_UART_TX_PIN, PSU_UART_RX_PIN, PSU_UART_BAUD);
    return true;
}

void psu_release(void) {
    if (!psu_active)
        return;
    uart_deinit(PSU_UART_ID);
    gpio_deinit(PSU_UART_TX_PIN);
    gpio_deinit(PSU_UART_RX_PIN);
    psu_active = false;
    // Power-group pins are re-initialised lazily by the next TARGET POWER command.
    uart_cli_send("PSU UART released: GP10/11 returned to the target power group\r\n");
}

// Drain any stale RX bytes.
static void psu_flush_rx(void) {
    while (uart_is_readable(PSU_UART_ID))
        (void)uart_getc(PSU_UART_ID);
}

// Send a raw command (no terminator by default).
static void psu_send(const char *cmd) {
    uart_puts(PSU_UART_ID, cmd);
    if (PSU_CMD_TERM[0])
        uart_puts(PSU_UART_ID, PSU_CMD_TERM);
    sleep_ms(PSU_CMD_GAP_MS);
}

// Send a query, then read the ASCII response into buf (NUL-terminated). Returns
// the byte count. Reads until an idle gap or the total cap.
static int psu_do_query(const char *cmd, char *buf, size_t buflen) {
    if (buflen == 0) return 0;
    psu_flush_rx();
    uart_puts(PSU_UART_ID, cmd);
    if (PSU_CMD_TERM[0])
        uart_puts(PSU_UART_ID, PSU_CMD_TERM);

    size_t n = 0;
    uint64_t start = time_us_64();
    uint64_t last = start;
    while (n < buflen - 1) {
        if (uart_is_readable(PSU_UART_ID)) {
            buf[n++] = (char)uart_getc(PSU_UART_ID);
            last = time_us_64();
        } else {
            uint64_t now = time_us_64();
            if (n > 0 && (now - last) > (uint64_t)PSU_READ_IDLE_MS * 1000) break;
            if ((now - start) > (uint64_t)PSU_READ_TOTAL_MS * 1000) break;
        }
    }
    buf[n] = '\0';
    return (int)n;
}

bool psu_set_voltage_mv(uint32_t mv) {
    if (!psu_begin()) return false;
    // VSET1:NN.NN (volts, 2 decimals)
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "VSET1:%lu.%02lu",
             (unsigned long)(mv / 1000), (unsigned long)((mv % 1000) / 10));
    psu_send(cmd);
    return true;
}

bool psu_set_current_ma(uint32_t ma) {
    if (!psu_begin()) return false;
    // ISET1:N.NNN (amps, 3 decimals)
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "ISET1:%lu.%03lu",
             (unsigned long)(ma / 1000), (unsigned long)(ma % 1000));
    psu_send(cmd);
    return true;
}

bool psu_output(bool on) {
    if (!psu_begin()) return false;
    psu_send(on ? "OUT1" : "OUT0");
    return true;
}

int psu_id(char *buf, size_t buflen) {
    if (!psu_begin()) { if (buflen) buf[0] = '\0'; return 0; }
    return psu_do_query("*IDN?", buf, buflen);
}

int psu_read_voltage(char *buf, size_t buflen) {
    if (!psu_begin()) { if (buflen) buf[0] = '\0'; return 0; }
    return psu_do_query("VOUT1?", buf, buflen);
}

int psu_read_current(char *buf, size_t buflen) {
    if (!psu_begin()) { if (buflen) buf[0] = '\0'; return 0; }
    return psu_do_query("IOUT1?", buf, buflen);
}

bool psu_read_status(uint8_t *status_out) {
    if (!psu_begin()) return false;
    char b[4];
    int n = psu_do_query("STATUS?", b, sizeof(b));
    if (n < 1) return false;
    if (status_out) *status_out = (uint8_t)b[0];
    return true;
}
