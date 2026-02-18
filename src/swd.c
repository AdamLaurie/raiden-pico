/*
 * SWD (Serial Wire Debug) implementation for Raiden-Pico
 *
 * Bit-banged SWD interface for target debugging.
 * Based on ARM Debug Interface v5 Architecture Specification.
 */

#include "swd.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include <stdio.h>

// Clock divider for timing - lower = slower, more reliable
// 50 works well for most STM32 at 150MHz Pico clock
static uint32_t clk_divider = 50;

static bool initialized = false;
static uint8_t last_ack = 0;
static uint32_t current_select = 0xFFFFFFFF;

// Track SWDIO direction to minimize turnarounds
typedef enum {
    SWDIO_FLOAT = 0,  // Input
    SWDIO_DRIVE = 1   // Output
} swdio_dir_t;
static swdio_dir_t swdio_dir = SWDIO_FLOAT;

// JTAG-to-SWD switch sequence
#define JTAG_TO_SWD_SEQUENCE 0xE79E

// --- Low-level bit operations ---

static inline void clk_delay(void) {
    for (volatile uint32_t i = clk_divider; i > 0; --i)
        continue;
}

static inline void swclk_set(void) {
    gpio_put(SWD_SWCLK_PIN, 1);
}

static inline void swclk_clr(void) {
    gpio_put(SWD_SWCLK_PIN, 0);
}

static inline void swdio_set(void) {
    gpio_put(SWD_SWDIO_PIN, 1);
}

static inline void swdio_clr(void) {
    gpio_put(SWD_SWDIO_PIN, 0);
}

static inline bool swdio_get(void) {
    return gpio_get(SWD_SWDIO_PIN);
}

static inline void swdio_out(void) {
    gpio_set_dir(SWD_SWDIO_PIN, GPIO_OUT);
}

static inline void swdio_in(void) {
    gpio_set_dir(SWD_SWDIO_PIN, GPIO_IN);
}

// Turnaround - only clock if direction actually changes
static void swd_turnaround(swdio_dir_t dir) {
    if (dir == swdio_dir)
        return;
    swdio_dir = dir;

    if (dir == SWDIO_FLOAT) {
        swdio_in();
    } else {
        swclk_clr();
    }

    clk_delay();
    swclk_set();
    clk_delay();

    if (dir == SWDIO_DRIVE) {
        swclk_clr();
        swdio_out();
    }
}

// Output bits LSB first
static void swd_seq_out(uint32_t data, size_t bits) {
    swd_turnaround(SWDIO_DRIVE);
    for (size_t i = 0; i < bits; i++) {
        swclk_clr();
        gpio_put(SWD_SWDIO_PIN, data & 1);
        clk_delay();
        swclk_set();
        clk_delay();
        data >>= 1;
    }
    swclk_clr();
}

// Input bits LSB first
static uint32_t swd_seq_in(size_t bits) {
    swd_turnaround(SWDIO_FLOAT);
    uint32_t data = 0;
    for (size_t i = 0; i < bits; i++) {
        swclk_clr();
        if (swdio_get())
            data |= (1U << i);
        clk_delay();
        swclk_set();
        clk_delay();
    }
    swclk_clr();
    return data;
}

// Calculate odd parity
static bool calc_parity(uint32_t data) {
    data ^= data >> 16;
    data ^= data >> 8;
    data ^= data >> 4;
    data ^= data >> 2;
    data ^= data >> 1;
    return data & 1;
}

// Output 32 bits with parity
static void swd_seq_out_parity(uint32_t data) {
    swd_seq_out(data, 32);
    // Parity bit
    gpio_put(SWD_SWDIO_PIN, calc_parity(data));
    clk_delay();
    swclk_set();
    clk_delay();
    swclk_clr();
}

// Input 32 bits with parity check
static bool swd_seq_in_parity(uint32_t *data) {
    *data = swd_seq_in(32);
    // Read parity bit
    clk_delay();
    bool parity_bit = swdio_get();
    swclk_set();
    clk_delay();
    swclk_clr();
    // Verify parity
    return calc_parity(*data) == parity_bit;
}

// Build request packet (from Black Magic Probe)
static uint8_t make_request(bool APnDP, bool RnW, uint8_t addr) {
    uint8_t request = 0x81;  // Start bit + Park bit

    if (APnDP)
        request ^= 0x22;
    if (RnW)
        request ^= 0x24;

    addr &= 0xC;
    request |= (addr << 1) & 0x18;
    if (addr == 4 || addr == 8)
        request ^= 0x20;

    return request;
}

// Line reset sequence (60 cycles HIGH + 4 idle for STM32 compatibility)
static void swd_line_reset(void) {
    swd_seq_out(0xFFFFFFFF, 32);  // 32 HIGH
    swd_seq_out(0x0FFFFFFF, 32);  // 28 HIGH + 4 idle (LOW)
}

// --- Public API ---

void swd_init(void) {
    gpio_init(SWD_SWCLK_PIN);
    gpio_set_dir(SWD_SWCLK_PIN, GPIO_OUT);
    gpio_put(SWD_SWCLK_PIN, 0);

    gpio_init(SWD_SWDIO_PIN);
    gpio_set_dir(SWD_SWDIO_PIN, GPIO_OUT);
    gpio_put(SWD_SWDIO_PIN, 1);

    swdio_dir = SWDIO_DRIVE;
    initialized = true;
    current_select = 0xFFFFFFFF;
}

void swd_deinit(void) {
    gpio_set_dir(SWD_SWCLK_PIN, GPIO_IN);
    gpio_set_dir(SWD_SWDIO_PIN, GPIO_IN);
    initialized = false;
}

bool swd_connect(void) {
    if (!initialized)
        swd_init();

    // Line reset
    swd_line_reset();

    // JTAG-to-SWD switch sequence
    swd_seq_out(JTAG_TO_SWD_SEQUENCE, 16);

    // Another line reset
    swd_line_reset();

    // Try to read DPIDR
    uint32_t dpidr;
    if (!swd_read_dp(DP_DPIDR, &dpidr))
        return false;

    current_select = 0xFFFFFFFF;
    return true;
}

bool swd_read_dp(uint8_t addr, uint32_t *value) {
    if (!initialized)
        return false;

    // Send request
    uint8_t request = make_request(false, true, addr);
    swd_seq_out(request, 8);

    // Read ACK
    last_ack = swd_seq_in(3);

    if (last_ack != SWD_ACK_OK) {
        swd_turnaround(SWDIO_DRIVE);
        return false;
    }

    // Read data with parity
    if (!swd_seq_in_parity(value)) {
        swd_turnaround(SWDIO_DRIVE);
        return false;
    }

    // Turnaround and idle cycles
    swd_turnaround(SWDIO_DRIVE);
    swd_seq_out(0, 8);

    return true;
}

bool swd_write_dp(uint8_t addr, uint32_t value) {
    if (!initialized)
        return false;

    // Send request
    uint8_t request = make_request(false, false, addr);
    swd_seq_out(request, 8);

    // Read ACK
    last_ack = swd_seq_in(3);

    // Turnaround back to drive
    swd_turnaround(SWDIO_DRIVE);

    if (last_ack != SWD_ACK_OK)
        return false;

    // Write data with parity
    swd_seq_out_parity(value);

    // Idle cycles
    swd_seq_out(0, 8);

    return true;
}

// Set SELECT register for AP access
static bool swd_select_ap(uint8_t ap, uint8_t addr) {
    uint32_t select = ((uint32_t)ap << 24) | (addr & 0xF0);
    if (select != current_select) {
        if (!swd_write_dp(DP_SELECT, select))
            return false;
        current_select = select;
    }
    return true;
}

bool swd_read_ap(uint8_t ap, uint8_t addr, uint32_t *value) {
    if (!initialized)
        return false;

    if (!swd_select_ap(ap, addr))
        return false;

    // First AP read is posted - do dummy read
    uint8_t request = make_request(true, true, addr & 0xC);
    swd_seq_out(request, 8);

    last_ack = swd_seq_in(3);
    if (last_ack != SWD_ACK_OK) {
        swd_turnaround(SWDIO_DRIVE);
        return false;
    }

    uint32_t dummy;
    swd_seq_in_parity(&dummy);
    swd_turnaround(SWDIO_DRIVE);
    swd_seq_out(0, 8);

    // Read RDBUFF to get actual value
    return swd_read_dp(DP_RDBUFF, value);
}

bool swd_write_ap(uint8_t ap, uint8_t addr, uint32_t value) {
    if (!initialized)
        return false;

    if (!swd_select_ap(ap, addr))
        return false;

    uint8_t request = make_request(true, false, addr & 0xC);
    swd_seq_out(request, 8);

    last_ack = swd_seq_in(3);
    swd_turnaround(SWDIO_DRIVE);

    if (last_ack != SWD_ACK_OK)
        return false;

    swd_seq_out_parity(value);
    swd_seq_out(0, 8);

    return true;
}

// Initialize AHB-AP for memory access
static bool swd_init_ahb_ap(void) {
    // Power up debug domain: CSYSPWRUPREQ | CDBGPWRUPREQ
    if (!swd_write_dp(DP_CTRL_STAT, 0x50000000))
        return false;

    // Wait for power-up acknowledge
    uint32_t stat;
    for (int i = 0; i < 100; i++) {
        if (!swd_read_dp(DP_CTRL_STAT, &stat))
            return false;
        if ((stat & 0xA0000000) == 0xA0000000)
            break;
        sleep_us(10);
    }

    if ((stat & 0xA0000000) != 0xA0000000)
        return false;

    // Configure CSW: 32-bit access, auto-increment
    return swd_write_ap(0, AP_CSW, 0x23000012);
}

static bool ahb_initialized = false;

uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count) {
    if (!initialized || count == 0)
        return 0;

    if (!ahb_initialized) {
        if (!swd_init_ahb_ap())
            return 0;
        ahb_initialized = true;
    }

    // Set TAR
    if (!swd_write_ap(0, AP_TAR, addr))
        return 0;

    // Read words
    uint32_t read = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!swd_read_ap(0, AP_DRW, &data[i]))
            break;
        read++;
    }

    return read;
}

uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count) {
    if (!initialized || count == 0)
        return 0;

    if (!ahb_initialized) {
        if (!swd_init_ahb_ap())
            return 0;
        ahb_initialized = true;
    }

    // Set TAR
    if (!swd_write_ap(0, AP_TAR, addr))
        return 0;

    // Write words
    uint32_t written = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!swd_write_ap(0, AP_DRW, data[i]))
            break;
        written++;
    }

    return written;
}

uint8_t swd_get_last_ack(void) {
    return last_ack;
}

bool swd_clear_errors(void) {
    // Write ABORT: clear all error flags
    return swd_write_dp(DP_ABORT, 0x1E);
}

uint32_t swd_identify(void) {
    if (!swd_connect())
        return 0;

    uint32_t dpidr;
    if (!swd_read_dp(DP_DPIDR, &dpidr))
        return 0;

    return dpidr;
}
