// Hardware-UART ownership arbiter — see uart_mux.h for the model and how to add
// an owner.

#include "uart_mux.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/padsbank0.h"

#include <stdio.h>

typedef struct {
    const char  *name;
    uint8_t      uart_num;   // 0 = uart0, 1 = uart1
    uint8_t      tx_pin;
    uint8_t      rx_pin;
    uart_park_t  park;
} uart_owner_desc_t;

typedef struct {
    uint32_t       baud;
    uint8_t        data_bits;
    uint8_t        stop_bits;
    uart_parity_t  parity;
} uart_line_t;

// Owner table. ADD NEW OWNERS HERE (and in uart_owner_t) — see uart_mux.h.
static const uart_owner_desc_t owners[UART_OWNER_COUNT] = {
    [UART_OWNER_NONE]     = { "none",        0, 0xFF, 0xFF, UART_PARK_HIZ },
    [UART_OWNER_CHIPSHOT] = { "ChipSHOUTER", 0, CHIPSHOT_UART_TX_PIN, CHIPSHOT_UART_RX_PIN, UART_PARK_HIZ },
    [UART_OWNER_TARGET]   = { "Target",      1, 4,  5,  UART_PARK_HIZ },
    [UART_OWNER_GRBL]     = { "GRBL",        1, 8,  9,  UART_PARK_HIZ },
    // PSU TX feeds a MAX3232 input: hold it at the idle (mark) level, never float.
    [UART_OWNER_PSU]      = { "PSU",         1, PSU_UART_TX_PIN, PSU_UART_RX_PIN, UART_PARK_TX_HIGH },
};

static uart_line_t lines[UART_OWNER_COUNT] = {
    [UART_OWNER_CHIPSHOT] = { CHIPSHOT_UART_BAUD, 8, 1, UART_PARITY_NONE },
    [UART_OWNER_TARGET]   = { 115200,             8, 1, UART_PARITY_NONE },
    [UART_OWNER_GRBL]     = { 115200,             8, 1, UART_PARITY_NONE },
    [UART_OWNER_PSU]      = { PSU_UART_BAUD,      8, 1, UART_PARITY_NONE },
};

static uart_mux_hook_t attach_hook[UART_OWNER_COUNT];
static uart_mux_hook_t detach_hook[UART_OWNER_COUNT];

static uart_owner_t current[2] = { UART_OWNER_NONE, UART_OWNER_NONE };

static inline bool valid(uart_owner_t who) {
    return who > UART_OWNER_NONE && who < UART_OWNER_COUNT;
}

static inline uart_inst_t *inst(uart_owner_t who) {
    return uart_get_instance(owners[who].uart_num);
}

void uart_mux_set_hooks(uart_owner_t who, uart_mux_hook_t attach, uart_mux_hook_t detach) {
    if (!valid(who)) return;
    attach_hook[who] = attach;
    detach_hook[who] = detach;
}

bool uart_mux_is_owner(uart_owner_t who) {
    return valid(who) && current[owners[who].uart_num] == who;
}

uart_owner_t uart_mux_owner(uint uart_num) {
    return uart_num < 2 ? current[uart_num] : UART_OWNER_NONE;
}

uart_inst_t *uart_mux_uart(uart_owner_t who) {
    return valid(who) ? inst(who) : NULL;
}

const char *uart_mux_name(uart_owner_t who) {
    return (who >= 0 && who < UART_OWNER_COUNT) ? owners[who].name : "?";
}

void uart_mux_pins(uart_owner_t who, uint8_t *tx, uint8_t *rx) {
    if (tx) *tx = valid(who) ? owners[who].tx_pin : 0xFF;
    if (rx) *rx = valid(who) ? owners[who].rx_pin : 0xFF;
}

uart_owner_t uart_mux_pin_owner(uint pin) {
    for (int w = UART_OWNER_NONE + 1; w < UART_OWNER_COUNT; w++) {
        if (owners[w].tx_pin == pin || owners[w].rx_pin == pin) return (uart_owner_t)w;
    }
    return UART_OWNER_NONE;
}

uint32_t uart_mux_get_baud(uart_owner_t who) {
    return valid(who) ? lines[who].baud : 0;
}

const char *uart_mux_line_str(uart_owner_t who) {
    static char buf[8];
    if (!valid(who)) return "-";
    char p = lines[who].parity == UART_PARITY_EVEN ? 'E'
           : lines[who].parity == UART_PARITY_ODD  ? 'O' : 'N';
    snprintf(buf, sizeof(buf), "%u%c%u", lines[who].data_bits, p, lines[who].stop_bits);
    return buf;
}

void uart_mux_set_line(uart_owner_t who, uint32_t baud, uint data_bits,
                       uart_parity_t parity, uint stop_bits) {
    if (!valid(who)) return;
    lines[who] = (uart_line_t){ baud, (uint8_t)data_bits, (uint8_t)stop_bits, parity };
    if (uart_mux_is_owner(who)) {
        uart_set_baudrate(inst(who), baud);
        uart_set_format(inst(who), data_bits, stop_bits, parity);
    }
}

void uart_mux_set_baud(uart_owner_t who, uint32_t baud) {
    if (!valid(who)) return;
    lines[who].baud = baud;
    if (uart_mux_is_owner(who)) uart_set_baudrate(inst(who), baud);
}

void uart_mux_set_format(uart_owner_t who, uint data_bits, uart_parity_t parity, uint stop_bits) {
    if (!valid(who)) return;
    lines[who].data_bits = (uint8_t)data_bits;
    lines[who].stop_bits = (uint8_t)stop_bits;
    lines[who].parity = parity;
    if (uart_mux_is_owner(who)) uart_set_format(inst(who), data_bits, stop_bits, parity);
}

// Pins of an owner that does not hold its UART: never left selected to UART
// (a still-selected TX keeps transmitting, a still-selected RX keeps feeding the
// shared receiver).
static void park_pins(uart_owner_t who) {
    const uart_owner_desc_t *d = &owners[who];
    gpio_init(d->rx_pin);                       // SIO input
    gpio_disable_pulls(d->rx_pin);
    gpio_init(d->tx_pin);
    gpio_disable_pulls(d->tx_pin);
    if (d->park == UART_PARK_TX_HIGH) {
        gpio_put(d->tx_pin, 1);                 // latch HIGH before enabling the driver
        gpio_set_dir(d->tx_pin, GPIO_OUT);
    }
}

void uart_mux_park(uart_owner_t who) {
    if (valid(who) && !uart_mux_is_owner(who)) park_pins(who);
}

static void detach_current(uint uart_num) {
    uart_owner_t prev = current[uart_num];
    if (prev == UART_OWNER_NONE) return;
    uart_inst_t *u = uart_get_instance(uart_num);
    if (detach_hook[prev]) detach_hook[prev]();
    uart_set_irq_enables(u, false, false);
    irq_set_enabled(UART_IRQ_NUM(u), false);
    current[uart_num] = UART_OWNER_NONE;
    park_pins(prev);
}

static void attach(uart_owner_t who) {
    const uart_owner_desc_t *d = &owners[who];
    uart_inst_t *u = inst(who);
    const uart_line_t *l = &lines[who];

    uart_deinit(u);
    gpio_init(d->tx_pin);
    gpio_init(d->rx_pin);
    uart_init(u, l->baud);
    uart_set_format(u, l->data_bits, l->stop_bits, l->parity);
    uart_set_hw_flow(u, false, false);
    uart_set_fifo_enabled(u, true);
    gpio_set_function(d->tx_pin, UART_FUNCSEL_NUM(u, d->tx_pin));
    gpio_set_function(d->rx_pin, UART_FUNCSEL_NUM(u, d->rx_pin));
    // Keep the pads readable by PIO (TRIGGER UART snoops GP4/5).
    hw_clear_bits(&padsbank0_hw->io[d->tx_pin], PADS_BANK0_GPIO0_ISO_BITS);
    hw_clear_bits(&padsbank0_hw->io[d->rx_pin], PADS_BANK0_GPIO0_ISO_BITS);
    sleep_us(100);                                  // let the peripheral settle
    while (uart_is_readable(u)) (void)uart_getc(u); // no stale bytes from the last owner
    current[d->uart_num] = who;
    if (attach_hook[who]) attach_hook[who]();
}

bool uart_mux_claim(uart_owner_t who) {
    if (!valid(who) || uart_mux_is_owner(who)) return false;
    detach_current(owners[who].uart_num);
    attach(who);
    return true;
}

void uart_mux_reclaim(uart_owner_t who) {
    if (!valid(who)) return;
    detach_current(owners[who].uart_num);
    attach(who);
}

void uart_mux_release(uart_owner_t who) {
    if (!uart_mux_is_owner(who)) return;
    uint n = owners[who].uart_num;
    detach_current(n);
    uart_deinit(uart_get_instance(n));
}

uart_owner_t uart_mux_borrow(uart_owner_t who) {
    if (!valid(who) || uart_mux_is_owner(who)) return UART_OWNER_NONE;
    uart_owner_t prev = current[owners[who].uart_num];
    uart_mux_claim(who);
    return prev;
}

void uart_mux_restore(uart_owner_t prev) {
    if (valid(prev)) uart_mux_claim(prev);
}
