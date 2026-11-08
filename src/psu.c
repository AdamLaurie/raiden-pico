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
// Protocol verified against a real TENMA 72-2540 / Multicomp Pro 72-2540 (via
// USB-RS232 and via a MAX3232 on GP10/11): 9600 8N1 (baud is PSU_UART_BAUD in
// config.h, runtime-tunable via PSU BAUD),
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
// Last known output state: -1 unknown, 0 off, 1 on. Set by every OUT we send and
// every STATUS? we read, so the TARGET POWER routing (repeated auto power-on per
// SYNC) does not pay a STATUS? round trip each time. A front-panel toggle is not
// seen until the next STATUS? read (PSU STATUS / an OUT confirmation).
static int8_t psu_out_cached = -1;

// Runtime-tunable UART baud (default from config.h). TENMA/Korad units vary and
// some menus can be set to a non-default rate, so `PSU BAUD <rate>` lets the
// bench pick it without a reflash. Applied at psu_begin(), or live if claimed.
static uint32_t psu_baud = PSU_UART_BAUD;

uint32_t psu_get_baud(void) { return psu_baud; }

bool psu_set_baud(uint32_t baud) {
    if (baud < 1200 || baud > 921600)
        return false;
    psu_baud = baud;
    if (psu_active)
        uart_set_baudrate(PSU_UART_ID, psu_baud);  // re-apply to the live UART
    return true;
}

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
    uart_init(PSU_UART_ID, psu_baud);
    uart_set_format(PSU_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(PSU_UART_TX_PIN, UART_FUNCSEL_NUM(PSU_UART_ID, PSU_UART_TX_PIN));
    gpio_set_function(PSU_UART_RX_PIN, UART_FUNCSEL_NUM(PSU_UART_ID, PSU_UART_RX_PIN));

    psu_active = true;
    psu_out_cached = -1;
    uart_cli_printf("PSU UART claimed: UART1 on GP%d(TX)/GP%d(RX) @ %lu 8N1 "
                    "(power group released)\r\n",
                    PSU_UART_TX_PIN, PSU_UART_RX_PIN, (unsigned long)psu_baud);
    return true;
}

void psu_release(void) {
    if (!psu_active)
        return;
    uart_deinit(PSU_UART_ID);
    gpio_deinit(PSU_UART_TX_PIN);
    gpio_deinit(PSU_UART_RX_PIN);
    psu_active = false;
    psu_out_cached = -1;
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
    psu_out_cached = on ? 1 : 0;
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

int psu_read_setpoint_voltage(char *buf, size_t buflen) {
    if (!psu_begin()) { if (buflen) buf[0] = '\0'; return 0; }
    return psu_do_query("VSET1?", buf, buflen);
}

int psu_read_setpoint_current(char *buf, size_t buflen) {
    if (!psu_begin()) { if (buflen) buf[0] = '\0'; return 0; }
    return psu_do_query("ISET1?", buf, buflen);
}

bool psu_read_status(uint8_t *status_out) {
    if (!psu_begin()) return false;
    char b[4];
    int n = psu_do_query("STATUS?", b, sizeof(b));
    if (n < 1) return false;
    psu_out_cached = (b[0] & 0x40) ? 1 : 0;   // bit6 = output on
    if (status_out) *status_out = (uint8_t)b[0];
    return true;
}

// ---- Target-supply helpers (TARGET POWER routing while the PSU is claimed) ----

// Parse a Korad fixed-point reply ("03.30", "0.125") into thousandths
// (mV / mA). Returns -1 if there are no digits.
static int32_t psu_parse_milli(const char *s) {
    int32_t whole = 0, frac = 0, fdigits = 0;
    bool any = false, dot = false;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            any = true;
            if (!dot) whole = whole * 10 + (*s - '0');
            else if (fdigits < 3) { frac = frac * 10 + (*s - '0'); fdigits++; }
        } else if (*s == '.' && !dot) {
            dot = true;
        } else if (any) {
            break;
        }
    }
    if (!any) return -1;
    while (fdigits < 3) { frac *= 10; fdigits++; }
    return whole * 1000 + frac;
}

bool psu_supply_state(bool *on) {
    if (psu_out_cached < 0 && !psu_read_status(NULL)) return false;
    *on = psu_out_cached == 1;
    return true;
}

bool psu_supply_set(bool on) {
    if (!psu_output(on)) return false;
    if (!psu_read_status(NULL)) { psu_out_cached = -1; return false; }
    return psu_out_cached == (on ? 1 : 0);
}

int32_t psu_setpoint_mv(void) {
    char b[16];
    if (psu_read_setpoint_voltage(b, sizeof(b)) <= 0) return -1;
    return psu_parse_milli(b);
}

int32_t psu_wait_vout(bool rising, int32_t threshold_mv, uint32_t timeout_ms) {
    char b[16];
    int32_t mv = -1;
    absolute_time_t end = make_timeout_time_ms(timeout_ms);
    do {
        if (psu_read_voltage(b, sizeof(b)) > 0) mv = psu_parse_milli(b);
        if (mv >= 0 && (rising ? mv >= threshold_mv : mv <= threshold_mv)) break;
    } while (!time_reached(end));
    return mv;
}
