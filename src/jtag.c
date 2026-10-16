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

/* ======================================================================
 * ARM ADIv6 JTAG-DP access
 *
 * For ARM debug ports that are an ADIv6 / DPv3 JTAG-DP (IR length 4) and do not
 * speak SWD. This is the memory-access debug path: bring the DP up, enumerate the
 * APs, and read/write target memory through a MEM-AP.
 *
 *   - power up:  write DP CTRL/STAT (0x4) = 0x50000000 (CSYSPWRUPREQ |
 *                CDBGPWRUPREQ), poll for the ACK bits (0xA0000000).
 *   - AP select: ADIv6 APs are addressed by a full register address. DP
 *                SELECT (0x8) carries ADDR[31:4]; the scan's A[3:2] field
 *                carries ADDR[3:2]. (32-bit AP addresses, so SELECT1 stays 0.)
 *   - MEM-AP registers are at the AP base: CSW=+0xd00, TAR=+0xd04, DRW=+0xd0c.
 *
 * Every transfer is the 35-bit JTAG-DP scan:
 *   in  = (DATA[31:0] << 3) | (A[3:2] << 1) | RnW
 *   out = (prevDATA[31:0] << 3) | ACK[2:0]
 * A read's data is returned on the FOLLOWING transfer, so each read issues the
 * request then clocks one more scan (RDBUFF) to shift the value out.
 * ====================================================================== */

#define DP_IR_ABORT     0x8u
#define DP_IR_DPACC     0xAu
#define DP_IR_APACC     0xBu
#define DP_ACK_WAIT     0x1u
#define DP_ACK_OK       0x2u
// Some ADI JTAG-DP implementations report the OK/FAULT ack as 0b100 instead of
// the nominal 0b010 (observed on a DPv3/ADIv6 part: reads return correct data
// with ack=0b100). Accept both so the driver works on either encoding.
#define DP_ACK_OK_ALT   0x4u

static inline bool dp_ack_ok(uint8_t a) {
    return a == DP_ACK_OK || a == DP_ACK_OK_ALT;
}

#define DP_REG_CTRLSTAT 0x4u
#define DP_REG_SELECT   0x8u
#define DP_REG_RDBUFF   0xCu

#define AP_REG_CSW      0xd00u
#define AP_REG_TAR      0xd04u
#define AP_REG_DRW      0xd0cu

// One raw 35-bit DPACC/APACC scan. `reg` is the register's byte address; its
// bits [3:2] form the A field. Returns the 32-bit data shifted out (the result
// of the PREVIOUS transfer) and the ACK of THIS one.
static uint32_t dp_xfer(uint8_t ir, bool rnw, uint32_t reg, uint32_t wdata, uint8_t *ack) {
    jtag_ir_shift(ir, 4);
    uint64_t scan = ((uint64_t)wdata << 3)
                  | (uint64_t)(((reg >> 2) & 0x3u) << 1)
                  | (rnw ? 1u : 0u);
    uint64_t out = jtag_dr_shift64(scan, 35);
    if (ack) *ack = (uint8_t)(out & 0x7u);
    return (uint32_t)(out >> 3);
}

// Retry while the DP answers WAIT.
static uint32_t dp_xfer_retry(uint8_t ir, bool rnw, uint32_t reg, uint32_t wdata, uint8_t *ack) {
    uint8_t a = 0;
    uint32_t d = 0;
    for (int i = 0; i < 32; i++) {
        d = dp_xfer(ir, rnw, reg, wdata, &a);
        if (a != DP_ACK_WAIT) break;
    }
    if (ack) *ack = a;
    return d;
}

// Diagnostic: raw DPACC reads of DPIDR (A=00, RnW=1). Fills three raw 35-bit
// scan outputs so the ACK (bits [2:0]) and data (bits [34:3]) can be inspected
// directly -- a read's data appears on the FOLLOWING scan.
void jtag_adiv6_dpraw(uint64_t out[3]) {
    jtag_reset();
    jtag_idle();
    (void)jtag_dr_shift(0, 32);          // consume the TAP IDCODE
    for (int i = 0; i < 3; i++) {
        jtag_ir_shift(DP_IR_DPACC, 4);
        out[i] = jtag_dr_shift64(1ull, 35);   // (data<<3)|(A=00<<1)|RnW=1
    }
}

bool jtag_dp_write(uint8_t ir, uint32_t reg, uint32_t val) {
    uint8_t ack;
    dp_xfer_retry(ir, false, reg, val, &ack);
    return dp_ack_ok(ack);
}

// Read a DP or AP register: push the read, then clock RDBUFF to shift the
// value out (RDBUFF's scan-out carries the previous read's data).
bool jtag_dp_read(uint8_t ir, uint32_t reg, uint32_t *out) {
    uint8_t ack;
    dp_xfer_retry(ir, true, reg, 0, &ack);
    if (!dp_ack_ok(ack)) return false;
    uint32_t d = dp_xfer_retry(DP_IR_DPACC, true, DP_REG_RDBUFF, 0, &ack);
    if (!dp_ack_ok(ack)) return false;
    if (out) *out = d;
    return true;
}

// Reset the TAP, confirm the DP, request the debug power domains.
bool jtag_adiv6_dp_init(uint32_t *tap_idcode, uint32_t *dp_idr, uint32_t *ctrlstat) {
    jtag_reset();
    jtag_idle();
    uint32_t id = jtag_dr_shift(0, 32);     // IDCODE sits in DR after TAP reset
    if (tap_idcode) *tap_idcode = id;
    if (id == 0 || id == 0xFFFFFFFF) return false;

    jtag_dp_write(DP_IR_DPACC, DP_REG_SELECT, 0);   // DPBANKSEL = 0

    uint32_t idr = 0;
    bool dpidr_ok = jtag_dp_read(DP_IR_DPACC, 0x0u /*DPIDR*/, &idr);
    if (dp_idr) *dp_idr = idr;
    // TAP is alive but the DPACC read of DP IDR did not return valid data: the
    // DP is not responding to register access, so report failure.
    if (!dpidr_ok || idr == 0 || idr == 0xFFFFFFFF) return false;

    jtag_dp_write(DP_IR_DPACC, DP_REG_CTRLSTAT, 0x50000000u);
    uint32_t cs = 0;
    for (int i = 0; i < 50; i++) {
        if (!jtag_dp_read(DP_IR_DPACC, DP_REG_CTRLSTAT, &cs)) break;
        if ((cs & 0xA0000000u) == 0xA0000000u) break;   // CSYSPWRUPACK | CDBGPWRUPACK
    }
    if (ctrlstat) *ctrlstat = cs;
    return true;
}

// Read one AP register (full ADIv6 register address = ap_base + reg).
bool jtag_adiv6_ap_read(uint32_t ap_base, uint32_t reg, uint32_t *out) {
    uint32_t full = ap_base + reg;
    if (!jtag_dp_write(DP_IR_DPACC, DP_REG_SELECT, full & 0xFFFFFFF0u)) return false;
    return jtag_dp_read(DP_IR_APACC, full, out);
}

bool jtag_adiv6_ap_write(uint32_t ap_base, uint32_t reg, uint32_t val) {
    uint32_t full = ap_base + reg;
    if (!jtag_dp_write(DP_IR_DPACC, DP_REG_SELECT, full & 0xFFFFFFF0u)) return false;
    return jtag_dp_write(DP_IR_APACC, full, val);
}

// DeviceEn (CSW bit 6) of a MEM-AP -- when 0, the MEM-AP refuses memory
// accesses. Returns 0/1, or -1 on access failure.
int jtag_adiv6_deviceen(uint32_t ap_base, uint32_t *csw_out) {
    uint32_t csw = 0;
    if (!jtag_adiv6_ap_read(ap_base, AP_REG_CSW, &csw)) return -1;
    if (csw_out) *csw_out = csw;
    return (int)((csw >> 6) & 1u);
}

// Read one 32-bit word of target memory through the MEM-AP (TAR + DRW).
bool jtag_adiv6_mem_read(uint32_t ap_base, uint32_t addr, uint32_t *out) {
    if (!jtag_adiv6_ap_write(ap_base, AP_REG_TAR, addr)) return false;
    return jtag_adiv6_ap_read(ap_base, AP_REG_DRW, out);
}

// Read DP CTRL/STAT (DPACC 0x4). Healthy = CSYSPWRUPACK|CDBGPWRUPACK (0xA0000000
// in the top nibble); if the power domains are down, AP reads return junk, so
// re-check this before trusting an AP read.
bool jtag_adiv6_ctrlstat(uint32_t *out) {
    if (!jtag_dp_write(DP_IR_DPACC, DP_REG_SELECT, 0u)) return false;   // DPBANKSEL=0
    return jtag_dp_read(DP_IR_DPACC, DP_REG_CTRLSTAT, out);
}

// True if CTRL/STAT shows any sticky flag: STICKYORUN(b1), STICKYCMP(b4),
// STICKYERR(b5), WDATAERR(b7). Any of these poisons every subsequent read.
bool jtag_adiv6_ctrlstat_sticky(uint32_t cs) {
    return (cs & ((1u << 1) | (1u << 4) | (1u << 5) | (1u << 7))) != 0u;
}

// Write the DP ABORT register (IR=ABORT, A=00, write). 0x1F clears all sticky
// flags (DAPABORT|STKCMPCLR|STKERRCLR|WDERRCLR|ORUNERRCLR) -- the standard way
// to recover the DP after a sticky error. NOTE: this does NOT clear a MEM-AP
// transfer-in-progress wedge (CSW TrInProg, bit 7); that needs a target reset.
void jtag_dp_abort(uint32_t flags) {
    jtag_ir_shift(DP_IR_ABORT, 4);
    uint64_t scan = ((uint64_t)flags << 3);   // A[3:2]=00, RnW=0
    jtag_dr_shift64(scan, 35);
}

// Clear sticky and report whether CTRL/STAT came back clean afterwards.
bool jtag_adiv6_clear_sticky(void) {
    jtag_dp_abort(0x1Fu);
    uint32_t cs = 0;
    if (!jtag_adiv6_ctrlstat(&cs)) return false;
    return !jtag_adiv6_ctrlstat_sticky(cs);
}

// Configure a MEM-AP CSW while preserving the prot/domain bits the device
// already has: Size=word (0b010), AddrInc = single (0b01) or off (0b00).
bool jtag_adiv6_csw_config(uint32_t ap_base, bool addrinc) {
    uint32_t csw = 0;
    if (!jtag_adiv6_ap_read(ap_base, AP_REG_CSW, &csw)) return false;
    csw = (csw & ~0x3Fu) | (addrinc ? 0x10u : 0x00u) | 0x02u;  // [5:4]=AddrInc, [2:0]=Size
    return jtag_adiv6_ap_write(ap_base, AP_REG_CSW, csw);
}

// Write one 32-bit word to target memory via the MEM-AP (TAR + DRW).
// Requires the MEM-AP to permit memory access (CSW DeviceEn = 1).
bool jtag_adiv6_mem_write(uint32_t ap_base, uint32_t addr, uint32_t val) {
    if (!jtag_adiv6_ap_write(ap_base, AP_REG_TAR, addr)) return false;
    return jtag_adiv6_ap_write(ap_base, AP_REG_DRW, val);
}

// Block read with TAR auto-increment. ADIv6 only guarantees the MEM-AP TAR
// auto-increment within a 1 KB (0x400) window, so TAR is re-armed at every
// boundary. Returns the number of words actually read; stops on the first failed
// transfer so a wedge/sticky truncates the dump instead of returning garbage.
uint32_t jtag_adiv6_mem_read_block(uint32_t ap_base, uint32_t addr,
                                   uint32_t *buf, uint32_t count) {
    if (!jtag_adiv6_csw_config(ap_base, true)) return 0;
    uint32_t done = 0;
    bool need_tar = true;
    while (done < count) {
        if (need_tar || (addr & 0x3FFu) == 0u) {
            if (!jtag_adiv6_ap_write(ap_base, AP_REG_TAR, addr)) break;
            need_tar = false;
        }
        uint32_t w = 0;
        if (!jtag_adiv6_ap_read(ap_base, AP_REG_DRW, &w)) break;
        buf[done++] = w;
        addr += 4u;
    }
    return done;
}
