/*
 * JTAG interface implementation for Raiden-Pico
 *
 * Bit-banged JTAG for ARM7TDMI and other targets.
 */

#include "jtag.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include <stdio.h>

// Clock divider for timing (higher = slower, more reliable)
static uint32_t clk_divider = 100;

// Adaptive clocking via RTCK (optional - falls back to fixed timing if not connected)
static bool use_rtck = true;
static uint32_t rtck_timeout = 1000;  // Max iterations to wait for RTCK
static bool rtck_available = false;   // Set true if RTCK responds during init

static bool initialized = false;
static jtag_tap_state_t tap_state = TAP_RESET;
static uint8_t ir_length = 4;  // Default for ARM7TDMI

// --- Low-level operations ---

static inline void clk_delay(void) {
    for (volatile uint32_t i = clk_divider; i > 0; --i)
        continue;
}

static inline void tck_high(void) {
    gpio_put(JTAG_TCK_PIN, 1);
}

static inline void tck_low(void) {
    gpio_put(JTAG_TCK_PIN, 0);
}

static inline void tms_high(void) {
    gpio_put(JTAG_TMS_PIN, 1);
}

static inline void tms_low(void) {
    gpio_put(JTAG_TMS_PIN, 0);
}

static inline void tdi_high(void) {
    gpio_put(JTAG_TDI_PIN, 1);
}

static inline void tdi_low(void) {
    gpio_put(JTAG_TDI_PIN, 0);
}

static inline bool tdo_read(void) {
    return gpio_get(JTAG_TDO_PIN);
}

static inline bool rtck_read(void) {
    return gpio_get(JTAG_RTCK_PIN);
}

// Wait for RTCK to match expected level (with timeout)
static bool wait_rtck(bool level) {
    if (!use_rtck || !rtck_available)
        return true;
    for (uint32_t i = 0; i < rtck_timeout; i++) {
        if (rtck_read() == level)
            return true;
    }
    // Timeout - disable RTCK for this session
    rtck_available = false;
    return false;
}

// Clock one bit with optional adaptive clocking via RTCK
// Standard JTAG: TMS/TDI sampled on rising edge, TDO changes on falling edge
static bool jtag_clock_bit(bool tms, bool tdi) {
    // Set TMS and TDI while TCK is low
    gpio_put(JTAG_TMS_PIN, tms);
    gpio_put(JTAG_TDI_PIN, tdi);

    bool tdo;

    if (use_rtck && rtck_available) {
        // Adaptive clocking
        clk_delay();
        tck_high();
        wait_rtck(true);
        tdo = tdo_read();  // Read TDO while TCK high
        tck_low();
        wait_rtck(false);
    } else {
        // Fixed timing mode
        clk_delay();
        tck_high();
        clk_delay();
        tdo = tdo_read();  // Read TDO while TCK high
        tck_low();
        clk_delay();
    }

    return tdo;
}

// Clock multiple bits with TMS low, shifting data LSB first
static uint32_t jtag_shift_bits(uint32_t data, uint8_t bits, bool exit) {
    uint32_t result = 0;

    for (uint8_t i = 0; i < bits; i++) {
        bool is_last = (i == bits - 1);
        // On last bit, set TMS high if we want to exit shift state
        bool tms = is_last && exit;
        bool tdi = (data >> i) & 1;
        bool tdo = jtag_clock_bit(tms, tdi);
        if (tdo)
            result |= (1U << i);
    }

    return result;
}

// Clock multiple bits for 64-bit shifts
static uint64_t jtag_shift_bits64(uint64_t data, uint8_t bits, bool exit) {
    uint64_t result = 0;

    for (uint8_t i = 0; i < bits; i++) {
        bool is_last = (i == bits - 1);
        bool tms = is_last && exit;
        bool tdi = (data >> i) & 1;
        bool tdo = jtag_clock_bit(tms, tdi);
        if (tdo)
            result |= (1ULL << i);
    }

    return result;
}

// Navigate TAP state machine
static void jtag_goto_state(jtag_tap_state_t target) {
    // From any state, 5 TMS=1 clocks go to RESET
    if (target == TAP_RESET) {
        for (int i = 0; i < 5; i++)
            jtag_clock_bit(true, false);
        tap_state = TAP_RESET;
        return;
    }

    // From RESET, TMS=0 goes to IDLE
    if (tap_state == TAP_RESET && target == TAP_IDLE) {
        jtag_clock_bit(false, false);
        tap_state = TAP_IDLE;
        return;
    }

    // From IDLE to SELECT_DR
    if (tap_state == TAP_IDLE && target == TAP_SELECT_DR) {
        jtag_clock_bit(true, false);
        tap_state = TAP_SELECT_DR;
        return;
    }

    // Navigate to SHIFT_DR
    if (target == TAP_SHIFT_DR) {
        if (tap_state != TAP_SELECT_DR) {
            // Go through IDLE -> SELECT_DR
            if (tap_state == TAP_RESET)
                jtag_goto_state(TAP_IDLE);
            if (tap_state == TAP_IDLE)
                jtag_goto_state(TAP_SELECT_DR);
            if (tap_state == TAP_UPDATE_DR || tap_state == TAP_UPDATE_IR) {
                jtag_clock_bit(true, false);  // -> SELECT_DR
                tap_state = TAP_SELECT_DR;
            }
        }
        // SELECT_DR -> CAPTURE_DR -> SHIFT_DR
        jtag_clock_bit(false, false);  // -> CAPTURE_DR
        jtag_clock_bit(false, false);  // -> SHIFT_DR
        tap_state = TAP_SHIFT_DR;
        return;
    }

    // Navigate to SHIFT_IR
    if (target == TAP_SHIFT_IR) {
        if (tap_state != TAP_SELECT_IR) {
            // Go through SELECT_DR -> SELECT_IR
            if (tap_state == TAP_RESET)
                jtag_goto_state(TAP_IDLE);
            if (tap_state == TAP_IDLE) {
                jtag_clock_bit(true, false);  // -> SELECT_DR
                tap_state = TAP_SELECT_DR;
            }
            if (tap_state == TAP_SELECT_DR) {
                jtag_clock_bit(true, false);  // -> SELECT_IR
                tap_state = TAP_SELECT_IR;
            }
            if (tap_state == TAP_UPDATE_DR || tap_state == TAP_UPDATE_IR) {
                jtag_clock_bit(true, false);  // -> SELECT_DR
                jtag_clock_bit(true, false);  // -> SELECT_IR
                tap_state = TAP_SELECT_IR;
            }
        }
        // SELECT_IR -> CAPTURE_IR -> SHIFT_IR
        jtag_clock_bit(false, false);  // -> CAPTURE_IR
        jtag_clock_bit(false, false);  // -> SHIFT_IR
        tap_state = TAP_SHIFT_IR;
        return;
    }

    // Navigate to UPDATE_DR (from EXIT1_DR)
    if (target == TAP_UPDATE_DR && tap_state == TAP_EXIT1_DR) {
        jtag_clock_bit(true, false);  // -> UPDATE_DR
        tap_state = TAP_UPDATE_DR;
        return;
    }

    // Navigate to UPDATE_IR (from EXIT1_IR)
    if (target == TAP_UPDATE_IR && tap_state == TAP_EXIT1_IR) {
        jtag_clock_bit(true, false);  // -> UPDATE_IR
        tap_state = TAP_UPDATE_IR;
        return;
    }

    // Navigate to IDLE from UPDATE states
    if (target == TAP_IDLE) {
        if (tap_state == TAP_UPDATE_DR || tap_state == TAP_UPDATE_IR) {
            jtag_clock_bit(false, false);  // -> IDLE
            tap_state = TAP_IDLE;
            return;
        }
    }
}

// --- Public API ---

void jtag_init(void) {
    // TRST - output, drive HIGH to release TAP from reset
    // (shares pin with target reset, active low)
    gpio_init(JTAG_TRST_PIN);
    gpio_set_dir(JTAG_TRST_PIN, GPIO_OUT);
    gpio_put(JTAG_TRST_PIN, 1);

    // TCK - output, idle low
    gpio_init(JTAG_TCK_PIN);
    gpio_set_dir(JTAG_TCK_PIN, GPIO_OUT);
    gpio_put(JTAG_TCK_PIN, 0);

    // TMS - output, idle high (keeps TAP in reset state)
    gpio_init(JTAG_TMS_PIN);
    gpio_set_dir(JTAG_TMS_PIN, GPIO_OUT);
    gpio_put(JTAG_TMS_PIN, 1);

    // TDI - output, idle high
    gpio_init(JTAG_TDI_PIN);
    gpio_set_dir(JTAG_TDI_PIN, GPIO_OUT);
    gpio_put(JTAG_TDI_PIN, 1);

    // TDO - input with pull-up (in case target tri-states)
    gpio_init(JTAG_TDO_PIN);
    gpio_set_dir(JTAG_TDO_PIN, GPIO_IN);
    gpio_pull_up(JTAG_TDO_PIN);

    // RTCK - input for adaptive clocking (optional)
    gpio_init(JTAG_RTCK_PIN);
    gpio_set_dir(JTAG_RTCK_PIN, GPIO_IN);
    gpio_disable_pulls(JTAG_RTCK_PIN);

    // Small delay for signals to settle
    sleep_us(100);

    // Detect if RTCK is connected by checking if it follows TCK
    rtck_available = false;
    if (use_rtck) {
        // Test RTCK response
        tck_low();
        sleep_us(100);
        bool rtck_low = rtck_read();
        tck_high();
        sleep_us(100);
        bool rtck_high = rtck_read();
        tck_low();
        if (!rtck_low && rtck_high) {
            rtck_available = true;
        }
    }

    initialized = true;
    tap_state = TAP_RESET;

    // Reset TAP state machine via TMS
    jtag_reset();
}

void jtag_deinit(void) {
    gpio_set_dir(JTAG_TCK_PIN, GPIO_IN);
    gpio_set_dir(JTAG_TMS_PIN, GPIO_IN);
    gpio_set_dir(JTAG_TDI_PIN, GPIO_IN);
    gpio_set_dir(JTAG_TDO_PIN, GPIO_IN);
    // Leave TRST high (don't tri-state, keep target out of reset)
    initialized = false;
}

void jtag_reset(void) {
    if (!initialized)
        jtag_init();

    // Reset via TMS only (5 clocks with TMS=1)
    // Don't pulse TRST as it may reset more than just the TAP
    jtag_goto_state(TAP_RESET);
}

void jtag_idle(void) {
    if (!initialized)
        jtag_init();
    jtag_goto_state(TAP_IDLE);
}

void jtag_set_ir_length(uint8_t bits) {
    ir_length = bits;
}

jtag_tap_state_t jtag_get_state(void) {
    return tap_state;
}

uint32_t jtag_ir_shift(uint32_t ir, uint8_t bits) {
    if (!initialized)
        jtag_init();

    jtag_goto_state(TAP_SHIFT_IR);

    // Shift IR, exit to EXIT1_IR on last bit
    uint32_t result = jtag_shift_bits(ir, bits, true);
    tap_state = TAP_EXIT1_IR;

    // Go to UPDATE_IR then IDLE
    jtag_goto_state(TAP_UPDATE_IR);
    jtag_goto_state(TAP_IDLE);

    return result;
}

uint32_t jtag_dr_shift(uint32_t dr, uint8_t bits) {
    if (!initialized)
        jtag_init();

    jtag_goto_state(TAP_SHIFT_DR);

    // Shift DR, exit to EXIT1_DR on last bit
    uint32_t result = jtag_shift_bits(dr, bits, true);
    tap_state = TAP_EXIT1_DR;

    // Go to UPDATE_DR then IDLE
    jtag_goto_state(TAP_UPDATE_DR);
    jtag_goto_state(TAP_IDLE);

    return result;
}

uint64_t jtag_dr_shift64(uint64_t dr, uint8_t bits) {
    if (!initialized)
        jtag_init();

    jtag_goto_state(TAP_SHIFT_DR);

    // Shift DR, exit to EXIT1_DR on last bit
    uint64_t result = jtag_shift_bits64(dr, bits, true);
    tap_state = TAP_EXIT1_DR;

    // Go to UPDATE_DR then IDLE
    jtag_goto_state(TAP_UPDATE_DR);
    jtag_goto_state(TAP_IDLE);

    return result;
}

uint32_t jtag_read_idcode(void) {
    if (!initialized)
        jtag_init();

    // Reset puts IDCODE in DR by default on most devices
    jtag_reset();
    jtag_idle();

    // Shift out 32-bit IDCODE
    return jtag_dr_shift(0, 32);
}

uint8_t jtag_scan_chain(uint32_t *idcodes, uint8_t max_devices) {
    if (!initialized)
        jtag_init();

    // Reset TAP
    jtag_reset();
    jtag_idle();

    // After reset, DR contains IDCODE or BYPASS (1 bit)
    // Shift in zeros and count how many IDCODEs we get
    jtag_goto_state(TAP_SHIFT_DR);

    uint8_t count = 0;
    uint32_t current = 0;
    uint8_t bit_count = 0;

    // Shift through looking for IDCODEs
    // IDCODE always starts with bit 0 = 1
    // BYPASS is a single 0 bit
    for (int i = 0; i < 256 && count < max_devices; i++) {
        bool tdo = jtag_clock_bit(false, false);  // Stay in SHIFT_DR

        if (bit_count == 0) {
            if (tdo) {
                // Start of IDCODE (bit 0 = 1)
                current = 1;
                bit_count = 1;
            }
            // else BYPASS bit, skip
        } else {
            if (tdo)
                current |= (1U << bit_count);
            bit_count++;

            if (bit_count == 32) {
                // Complete IDCODE
                if (idcodes)
                    idcodes[count] = current;
                count++;
                bit_count = 0;
                current = 0;
            }
        }
    }

    // Exit shift state
    jtag_clock_bit(true, false);  // -> EXIT1_DR
    tap_state = TAP_EXIT1_DR;
    jtag_goto_state(TAP_IDLE);

    return count;
}

bool jtag_rtck_available(void) {
    return rtck_available;
}
