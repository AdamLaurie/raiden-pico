/*
 * SWD (Serial Wire Debug) implementation for Raiden-Pico
 *
 * Bit-banged SWD interface for target debugging.
 * Based on ARM Debug Interface v5 Architecture Specification.
 */

#include "swd.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include <stdio.h>

// Clock half-period in microseconds (5 = ~100kHz, 2 = ~250kHz)
static uint32_t clk_delay_us = 2;

static bool initialized = false;
static bool connected = false;
static bool ahb_initialized = false;
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

// ADIv5.2 Selection Alert sequence (128 bits, LSB first)
#define SELECTION_ALERT_0 0x6209F392U
#define SELECTION_ALERT_1 0x86852D95U
#define SELECTION_ALERT_2 0xE3DDAFE9U
#define SELECTION_ALERT_3 0x19BC0EA2U

// Activation code for ARM SWD DP
#define ACTIVATION_CODE_SWD 0x1AU

// --- Low-level bit operations ---

static inline void clk_delay(void) {
    sleep_us(clk_delay_us);
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

// Turnaround - exact copy of BMP swdptap_turnaround.
// Does NOT assume a specific SWCLK state on entry.
static void swd_turnaround(swdio_dir_t dir) {
    if (dir == swdio_dir)
        return;
    swdio_dir = dir;

    if (dir == SWDIO_FLOAT) {
        // BMP: release SWDIO, SWCLK untouched
        swdio_in();
    } else {
        // BMP: ensure SWCLK LOW before turnaround clock
        // (handles both HIGH from parity read and LOW from seq_out)
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

// Output bits LSB first (matches BMP swdptap_seq_out_clk_delay)
// Ends with SWCLK LOW (trailing clr, same as BMP)
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

// Input bits LSB first (matches BMP swdptap_seq_in_clk_delay)
// Sample immediately after falling edge (same as BMP)
// Ends with SWCLK LOW (trailing clr)
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

// Output 32 bits with parity (matches BMP swdptap_seq_out_parity)
// seq_out ends SWCLK LOW. Set parity on SWDIO, then clock it in, end SWCLK LOW.
static void swd_seq_out_parity(uint32_t data) {
    swd_seq_out(data, 32);
    // SWCLK is LOW from seq_out's trailing CLR
    gpio_put(SWD_SWDIO_PIN, calc_parity(data));
    clk_delay();
    swclk_set();
    clk_delay();
    swclk_clr();
}

// Input 32 bits with parity check (matches BMP swdptap_seq_in_parity)
// Reads parity bit using same clock pattern as seq_in (CLR/read/delay/SET/delay/CLR).
// Ends SWCLK LOW, then calls turnaround(DRIVE).
static bool swd_seq_in_parity(uint32_t *data) {
    *data = swd_seq_in(32);
    // SWCLK is LOW from seq_in's trailing CLR
    // Read parity bit with same pattern as seq_in_clk_delay(1) in BMP:
    swclk_clr();  // redundant (already LOW), matches BMP
    bool parity_bit = swdio_get();
    clk_delay();
    swclk_set();
    clk_delay();
    swclk_clr();  // trailing CLR — SWCLK LOW, matches BMP
    swd_turnaround(SWDIO_DRIVE);
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

// Line reset sequence (56+ cycles with SWDIO HIGH, no trailing idle)
static void swd_line_reset(void) {
    swd_seq_out(0xFFFFFFFF, 32);  // 32 HIGH
    swd_seq_out(0x00FFFFFF, 24);  // 24 HIGH (total 56)
}

// Idle cycles (SWDIO LOW) - call after line reset before first transaction
static void swd_idle(int cycles) {
    swd_seq_out(0, cycles);
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
    connected = false;
}

bool swd_connect(void) {
    if (!initialized)
        swd_init();

    connected = false;
    ahb_initialized = false;
    current_select = 0xFFFFFFFF;

    // Force full pin init (in case another subsystem changed pin function)
    gpio_init(SWD_SWCLK_PIN);
    gpio_set_dir(SWD_SWCLK_PIN, GPIO_OUT);
    gpio_put(SWD_SWCLK_PIN, 0);
    gpio_init(SWD_SWDIO_PIN);
    gpio_set_dir(SWD_SWDIO_PIN, GPIO_OUT);
    gpio_put(SWD_SWDIO_PIN, 1);
    swdio_dir = SWDIO_DRIVE;
    sleep_ms(1);  // Let pins settle

    // === Method 1: Legacy JTAG-to-SWD ===
    swd_line_reset();
    swd_seq_out(JTAG_TO_SWD_SEQUENCE, 16);
    swd_line_reset();
    swd_idle(4);

    uint32_t dpidr;
    if (swd_read_dp(DP_DPIDR, &dpidr)) {
        swd_write_dp(DP_ABORT, 0x1E);
        printf("[SWD] Connected, DPIDR=0x%08X\r\n", (unsigned)dpidr);
        connected = true;
        return true;
    }

    // === Method 2: ADIv5.2 Dormant-to-SWD ===
    gpio_set_dir(SWD_SWDIO_PIN, GPIO_OUT);
    gpio_put(SWD_SWDIO_PIN, 1);
    swdio_dir = SWDIO_DRIVE;

    swd_seq_out(0xFF, 8);
    swd_seq_out(SELECTION_ALERT_0, 32);
    swd_seq_out(SELECTION_ALERT_1, 32);
    swd_seq_out(SELECTION_ALERT_2, 32);
    swd_seq_out(SELECTION_ALERT_3, 32);
    swd_seq_out(0, 4);
    swd_seq_out(ACTIVATION_CODE_SWD, 8);
    swd_line_reset();
    swd_idle(4);

    if (swd_read_dp(DP_DPIDR, &dpidr)) {
        swd_write_dp(DP_ABORT, 0x1E);
        printf("[SWD] Connected (dormant), DPIDR=0x%08X\r\n", (unsigned)dpidr);
        connected = true;
        return true;
    }

    printf("[SWD] Connect failed, ACK=0x%X\r\n", last_ack);
    return false;
}

bool swd_is_connected(void) {
    return connected;
}

bool swd_ensure_connected(void) {
    if (connected)
        return true;
    return swd_connect();
}

// Forward declarations for helpers used in connect_under_reset and set_rdp
static bool mem_read32(uint32_t addr, uint32_t *val);
static bool mem_write32(uint32_t addr, uint32_t val);
static bool swd_init_ahb_ap(void);

bool swd_connect_under_reset(void) {
    // BMP-style connect-under-reset with VC_CORERESET vector catch.
    // This halts the core at the reset vector BEFORE any firmware runs,
    // which is critical when the target has a watchdog that would
    // otherwise reset the core before we can halt it.

    // Step 1: Hold target in reset
    swd_nrst_assert();
    sleep_ms(10);

    // Step 2: Connect SWD while target is held in reset
    if (!swd_connect()) {
        printf("[SWD] CUR: connect failed\r\n");
        swd_nrst_release();
        return false;
    }

    // Step 3: Init AHB-AP (power cycle debug domain)
    if (!ahb_initialized) {
        if (!swd_init_ahb_ap()) {
            printf("[SWD] CUR: AHB-AP init failed\r\n");
            swd_nrst_release();
            return false;
        }
        ahb_initialized = true;
    }

    // Step 4: Enable debug (C_DEBUGEN) and request halt (C_HALT)
    mem_write32(DHCSR, DBGKEY | 0x3);

    // Step 5: Set DEMCR with VC_CORERESET to catch core on reset exit
    // DEMCR = 0xE000EDFC, VC_CORERESET = bit 0, TRCENA = bit 24
    #define DEMCR_ADDR       0xE000EDFC
    #define DEMCR_TRCENA     (1U << 24)
    #define DEMCR_VC_HARDERR (1U << 10)
    #define DEMCR_VC_CORERESET (1U << 0)
    uint32_t demcr = DEMCR_TRCENA | DEMCR_VC_HARDERR | DEMCR_VC_CORERESET;
    mem_write32(DEMCR_ADDR, demcr);

    // Step 6: Release reset — core will halt at reset vector due to VC_CORERESET
    swd_nrst_release();
    sleep_ms(10);

    // Step 7: Wait for halt (S_HALT and S_RESET_ST to clear)
    uint32_t dhcsr = 0;
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < 500) {
        if (!mem_read32(DHCSR, &dhcsr))
            continue;

        // Filter invalid reads
        if (dhcsr == 0xFFFFFFFF || (dhcsr & 0xF000FFF0) != 0)
            continue;

        // Wait for S_RESET_ST to clear (core has exited reset)
        if (dhcsr & (1U << 25))
            continue;

        // Check S_HALT and C_DEBUGEN
        if ((dhcsr & ((1U << 17) | (1U << 0))) == ((1U << 17) | (1U << 0))) {
            printf("[SWD] CUR: Halted at reset vector, DHCSR=0x%08X\r\n", (unsigned)dhcsr);

            // Freeze IWDG and WWDG in debug mode via DBGMCU_CR
            #define DBGMCU_CR 0xE0042004
            uint32_t dbg_cr;
            mem_read32(DBGMCU_CR, &dbg_cr);
            dbg_cr |= (1U << 8) | (1U << 9);  // DBG_IWDG_STOP | DBG_WWDG_STOP
            mem_write32(DBGMCU_CR, dbg_cr);

            return true;
        }
    }

    printf("[SWD] CUR: Halt timeout, DHCSR=0x%08X\r\n", (unsigned)dhcsr);
    return false;
}

void swd_nrst_assert(void) {
    gpio_init(SWD_NRST_PIN);
    gpio_set_dir(SWD_NRST_PIN, GPIO_OUT);
    gpio_put(SWD_NRST_PIN, 0);
}

void swd_nrst_release(void) {
    gpio_init(SWD_NRST_PIN);
    gpio_set_dir(SWD_NRST_PIN, GPIO_IN);  // High-Z, target pull-up
    gpio_disable_pulls(SWD_NRST_PIN);
}

void swd_nrst_pulse(uint32_t ms) {
    swd_nrst_assert();
    sleep_ms(ms);
    swd_nrst_release();
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

    // Read data with parity (seq_in_parity calls turnaround(DRIVE) internally)
    bool parity_ok = swd_seq_in_parity(value);

    // Idle cycles
    swd_seq_out(0, 8);

    return parity_ok;
}

bool swd_write_dp(uint8_t addr, uint32_t value) {
    if (!initialized)
        return false;

    // Send request
    uint8_t request = make_request(false, false, addr);
    swd_seq_out(request, 8);

    // Read ACK
    last_ack = swd_seq_in(3);

    if (last_ack != SWD_ACK_OK) {
        swd_turnaround(SWDIO_DRIVE);
        return false;
    }

    // Write data with parity (seq_out_parity->seq_out handles turnaround internally)
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

    if (last_ack != SWD_ACK_OK) {
        swd_turnaround(SWDIO_DRIVE);
        return false;
    }

    swd_seq_out_parity(value);
    swd_seq_out(0, 8);

    return true;
}

// Base CSW value — read from AP defaults during init, like BMP's adiv5_new_ap.
// Only size/addrinc are modified per-operation; all other bits preserved from AP default.
static uint32_t ap_csw_base = 0;

// CSW bit masks (from BMP adiv5.h)
#define CSW_SIZE_MASK     (7U << 0)
#define CSW_ADDRINC_MASK  (3U << 4)
#define CSW_SIZE_HALFWORD (1U << 0)
#define CSW_SIZE_WORD     (2U << 0)
#define CSW_ADDRINC_SINGLE (1U << 4)
#define CSW_DBGSWENABLE   (1U << 31)
#define CSW_MTE           (1U << 15)
#define CSW_HNOSEC        (1U << 30)

// Initialize AHB-AP for memory access.
// Matches BMP's adiv5_dp_init (power cycle) + adiv5_new_ap (CSW setup).
static bool swd_init_ahb_ap(void) {
    uint32_t stat;

    // Clear sticky errors first (BMP: adiv5_dp_abort before dp_init)
    swd_write_dp(DP_ABORT, 0x1E);

    // Step 1: Power DOWN debug domain (BMP: adiv5_dp_write(dp, CTRLSTAT, 0))
    if (!swd_write_dp(DP_CTRL_STAT, 0))
        return false;

    // Wait for power-down acknowledge (ACK bits clear)
    for (int i = 0; i < 250; i++) {
        if (!swd_read_dp(DP_CTRL_STAT, &stat))
            return false;
        if (!(stat & 0xA0000000))
            break;
        sleep_ms(1);
    }

    // Step 2: Power UP (BMP: CSYSPWRUPREQ | CDBGPWRUPREQ)
    if (!swd_write_dp(DP_CTRL_STAT, 0x50000000))
        return false;

    // Wait for power-up acknowledge (BMP: polls with 10ms delay)
    for (int i = 0; i < 200; i++) {
        sleep_ms(1);
        if (!swd_read_dp(DP_CTRL_STAT, &stat))
            return false;
        if ((stat & 0xA0000000) == 0xA0000000)
            break;
    }

    if ((stat & 0xA0000000) != 0xA0000000)
        return false;

    // Clear sticky errors after power cycle
    swd_write_dp(DP_ABORT, 0x1E);

    // BMP: adiv5_new_ap reads IDR, BASE, CSW from AP defaults
    uint32_t ap_idr;
    swd_read_ap(0, AP_IDR, &ap_idr);

    uint32_t csw_default;
    swd_read_ap(0, AP_CSW, &csw_default);

    // BMP: csw &= ~(SIZE_MASK | ADDRINC_MASK | MTE | HNOSEC); csw |= DBGSWENABLE
    ap_csw_base = csw_default;
    ap_csw_base &= ~(CSW_SIZE_MASK | CSW_ADDRINC_MASK | CSW_MTE | CSW_HNOSEC);
    ap_csw_base |= CSW_DBGSWENABLE;

    return true;
}

// BMP: ap_mem_access_setup — writes CSW + TAR before every memory operation
static bool swd_mem_access_setup(uint32_t addr) {
    uint32_t csw = ap_csw_base | CSW_SIZE_WORD | CSW_ADDRINC_SINGLE;

    // Write CSW (BMP: adiv5_ap_write(ap, CSW, csw))
    if (!swd_write_ap(0, AP_CSW, csw))
        return false;

    // Write TAR (BMP: adiv5_dp_write(dp, TAR, addr) — uses AP write since TAR is in same bank)
    if (!swd_write_ap(0, AP_TAR, addr))
        return false;

    return true;
}

uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count) {
    if (!initialized || count == 0)
        return 0;

    if (!ahb_initialized) {
        if (!swd_init_ahb_ap())
            return 0;
        ahb_initialized = true;
    }

    // BMP: setup CSW + TAR before access
    if (!swd_mem_access_setup(addr))
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

    // BMP: setup CSW + TAR before access
    if (!swd_mem_access_setup(addr))
        return 0;

    // Write words
    uint32_t written = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!swd_write_ap(0, AP_DRW, data[i]))
            break;
        written++;
    }

    // BMP: flush write buffer by reading RDBUFF
    uint32_t dummy;
    swd_read_dp(DP_RDBUFF, &dummy);

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

// --- Cortex-M debug operations ---

// Helper: read a single 32-bit word from target memory
static bool mem_read32(uint32_t addr, uint32_t *val) {
    return swd_read_mem(addr, val, 1) == 1;
}

// Helper: write a single 32-bit word to target memory
static bool mem_write32(uint32_t addr, uint32_t val) {
    return swd_write_mem(addr, &val, 1) == 1;
}

// Helper: write a 16-bit halfword to target memory (needed for F1 option bytes)
static bool mem_write16(uint32_t addr, uint16_t val) {
    if (!ahb_initialized) {
        if (!swd_init_ahb_ap())
            return false;
        ahb_initialized = true;
    }

    // Set CSW for halfword access
    uint32_t csw = ap_csw_base | CSW_SIZE_HALFWORD | CSW_ADDRINC_SINGLE;
    if (!swd_write_ap(0, AP_CSW, csw))
        return false;

    // Write TAR
    if (!swd_write_ap(0, AP_TAR, addr))
        return false;

    // Write DRW — data must be lane-aligned for halfword
    // For halfword at addr & 2 == 0: data in bits [15:0]
    // For halfword at addr & 2 == 2: data in bits [31:16]
    uint32_t drw = (addr & 2) ? ((uint32_t)val << 16) : (uint32_t)val;
    if (!swd_write_ap(0, AP_DRW, drw))
        return false;

    // Flush
    uint32_t dummy;
    swd_read_dp(DP_RDBUFF, &dummy);

    // Restore word-size CSW
    csw = ap_csw_base | CSW_SIZE_WORD | CSW_ADDRINC_SINGLE;
    swd_write_ap(0, AP_CSW, csw);

    return true;
}

bool swd_halt(void) {
    // BMP-style robust halt: loop until S_HALT is confirmed.
    // Step 1: Enable debug first (C_DEBUGEN without C_HALT)
    if (!mem_write32(DHCSR, DBGKEY | 0x1))
        return false;

    // Step 2: Read back to flush
    uint32_t dhcsr;
    mem_read32(DHCSR, &dhcsr);

    // Step 3: Loop - write C_DEBUGEN|C_HALT, read back, check S_HALT
    bool reset_seen = false;
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < 500) {
        // Write halt request
        if (!mem_write32(DHCSR, DBGKEY | 0x3))
            return false;

        // Read back DHCSR
        if (!mem_read32(DHCSR, &dhcsr))
            return false;

        // Filter invalid reads (errata on some STM32)
        if (dhcsr == 0xFFFFFFFF || (dhcsr & 0xF000FFF0) != 0)
            continue;

        // Check for reset - need to see it clear before accepting halt
        if ((dhcsr & (1 << 25)) && !reset_seen) {  // S_RESET_ST
            reset_seen = true;
            continue;
        }

        // Check if halt succeeded
        if ((dhcsr & ((1 << 17) | (1 << 0))) == ((1 << 17) | (1 << 0)))  // S_HALT | C_DEBUGEN
            return true;
    }

    printf("[SWD] Halt timeout, DHCSR=0x%08X\r\n", (unsigned)dhcsr);
    return false;
}

bool swd_resume(void) {
    // C_DEBUGEN only (clear C_HALT)
    return mem_write32(DHCSR, DBGKEY | 0x1);
}

bool swd_read_core_reg(uint8_t reg, uint32_t *value) {
    // Write register index to DCRSR (REGWnR=0 for read)
    if (!mem_write32(DCRSR, (uint32_t)reg))
        return false;

    // Wait for S_REGRDY
    for (int i = 0; i < 100; i++) {
        uint32_t dhcsr;
        if (!mem_read32(DHCSR, &dhcsr))
            return false;
        if (dhcsr & (1 << 16))  // S_REGRDY
            return mem_read32(DCRDR, value);
        sleep_us(10);
    }
    return false;
}

bool swd_detect(uint32_t *cpuid_out, uint32_t *dbg_idcode_out) {
    if (!ahb_initialized) {
        if (!swd_init_ahb_ap())
            return false;
        ahb_initialized = true;
    }

    if (cpuid_out)
        mem_read32(CPUID, cpuid_out);

    if (dbg_idcode_out)
        mem_read32(DBG_IDCODE, dbg_idcode_out);

    return true;
}

// --- STM32 flash operations ---

bool swd_stm32_flash_wait(const stm32_target_info_t *info, uint32_t timeout_ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < timeout_ms) {
        uint32_t sr;
        if (!mem_read32(info->flash_sr, &sr))
            return false;
        if (!(sr & 0x1))  // BSY bit
            return true;
        sleep_us(100);
    }
    return false;
}

bool swd_stm32_flash_unlock(const stm32_target_info_t *info) {
    // Clear sticky errors
    swd_write_dp(DP_ABORT, 0x1E);

    // Write key sequence to FLASH_KEYR
    if (!mem_write32(info->flash_keyr, info->flash_key1))
        return false;
    if (!mem_write32(info->flash_keyr, info->flash_key2))
        return false;

    // Verify unlock by reading CR — LOCK bit should be clear
    uint32_t cr;
    if (!mem_read32(info->flash_cr, &cr))
        return false;

    // F1/F3: LOCK is bit 7; L4: bit 31; F4: bit 31
    if (info->flash_base == 0x40022000)
        return !(cr & (1u << 7));    // F1/F3
    else
        return !(cr & (1u << 31));   // L4/F4
}

static bool stm32_opt_unlock(const stm32_target_info_t *info) {
    if (!swd_stm32_flash_unlock(info))
        return false;

    // Write option key sequence
    if (!mem_write32(info->flash_optkeyr, info->opt_key1))
        return false;
    if (!mem_write32(info->flash_optkeyr, info->opt_key2))
        return false;

    // Verify unlock
    uint32_t cr;
    if (!mem_read32(info->flash_cr, &cr))
        return false;

    // F1/F3: OPTWRE (bit 9) should be SET after unlock
    // L4/F4: OPTLOCK (bit 30) should be CLEAR after unlock
    if (info->flash_base == 0x40022000)
        return !!(cr & (1u << 9));    // F1/F3: OPTWRE set = unlocked
    else
        return !(cr & (1u << 30));    // L4/F4: OPTLOCK clear = unlocked
}

int swd_stm32_read_rdp(const stm32_target_info_t *info) {
    uint32_t optr;
    if (!mem_read32(info->flash_optr, &optr))
        return -1;

    uint8_t rdp_byte;

    // F1/F3: RDP is in FLASH_OBR register, bit 1 = RDPRT
    if (info->flash_base == 0x40022000 &&
        info->flash_optr == 0x4002201C) {
        // Try raw option bytes first (works at RDP0)
        uint32_t raw_opt;
        if (mem_read32(info->opt_base, &raw_opt)) {
            rdp_byte = raw_opt & 0xFF;
        } else {
            // Raw opt bytes unreadable (RDP1) — use OBR RDPRT flag
            swd_clear_errors();
            return (optr & (1u << 1)) ? 1 : 0;
        }
    }
    // F4: RDP in OPTCR bits [15:8]
    else if (info->flash_optr == 0x40023C14) {
        rdp_byte = (optr >> 8) & 0xFF;
    }
    // L4: RDP in OPTR bits [7:0]
    else {
        rdp_byte = optr & 0xFF;
    }

    if (rdp_byte == info->rdp_level0)
        return 0;
    if (rdp_byte == info->rdp_level2)
        return 2;
    return 1;
}

// Decode STM32L4 FLASH_OPTR register
static void decode_l4_optr(uint32_t optr) {
    uint8_t rdp = optr & 0xFF;
    printf("  RDP        = 0x%02X (Level %s)\r\n", rdp,
           rdp == 0xAA ? "0" : rdp == 0xCC ? "2 PERMANENT" : "1");

    uint8_t bor = (optr >> 8) & 0x7;
    const char *bor_str[] = {
        "~1.7V", "~2.0V", "~2.2V", "~2.5V",
        "~2.8V", "rsvd", "rsvd", "rsvd"
    };
    printf("  BOR_LEV    = %u (%s)\r\n", bor, bor_str[bor]);

    printf("  nRST_STOP  = %u (%s)\r\n", (optr >> 12) & 1,
           (optr >> 12) & 1 ? "no reset on Stop" : "reset on Stop");
    printf("  nRST_STDBY = %u (%s)\r\n", (optr >> 13) & 1,
           (optr >> 13) & 1 ? "no reset on Standby" : "reset on Standby");
    printf("  nRST_SHDW  = %u (%s)\r\n", (optr >> 14) & 1,
           (optr >> 14) & 1 ? "no reset on Shutdown" : "reset on Shutdown");
    printf("  IWDG_SW    = %u (%s)\r\n", (optr >> 16) & 1,
           (optr >> 16) & 1 ? "software IWDG" : "hardware IWDG");
    printf("  IWDG_STOP  = %u (%s)\r\n", (optr >> 17) & 1,
           (optr >> 17) & 1 ? "IWDG runs in Stop" : "IWDG frozen in Stop");
    printf("  IWDG_STDBY = %u (%s)\r\n", (optr >> 18) & 1,
           (optr >> 18) & 1 ? "IWDG runs in Standby" : "IWDG frozen in Standby");
    printf("  WWDG_SW    = %u (%s)\r\n", (optr >> 19) & 1,
           (optr >> 19) & 1 ? "software WWDG" : "hardware WWDG");
    printf("  BFB2       = %u (%s)\r\n", (optr >> 20) & 1,
           (optr >> 20) & 1 ? "boot from bank 2" : "boot from bank 1");
    printf("  DUALBANK   = %u (%s)\r\n", (optr >> 21) & 1,
           (optr >> 21) & 1 ? "dual-bank" : "single-bank");
    printf("  nBOOT1     = %u\r\n", (optr >> 23) & 1);
    printf("  SRAM2_PE   = %u (%s)\r\n", (optr >> 24) & 1,
           (optr >> 24) & 1 ? "SRAM2 parity check enabled" : "SRAM2 parity disabled");
    printf("  SRAM2_RST  = %u (%s)\r\n", (optr >> 25) & 1,
           (optr >> 25) & 1 ? "SRAM2 not erased on reset" : "SRAM2 erased on reset");
    printf("  nSWBOOT0   = %u (%s)\r\n", (optr >> 26) & 1,
           (optr >> 26) & 1 ? "BOOT0 from pin" : "BOOT0 from nBOOT0 bit");
    printf("  nBOOT0     = %u\r\n", (optr >> 27) & 1);
}

// Decode STM32F4 FLASH_OPTCR register
static void decode_f4_optcr(uint32_t optcr) {
    uint8_t rdp = (optcr >> 8) & 0xFF;
    printf("  RDP        = 0x%02X (Level %s)\r\n", rdp,
           rdp == 0xAA ? "0" : rdp == 0xCC ? "2 PERMANENT" : "1");

    uint8_t bor = (optcr >> 2) & 0x3;
    const char *bor_str[] = {"3 (~2.7V)", "2 (~2.4V)", "1 (~2.1V)", "0 (off)"};
    printf("  BOR_LEV    = %u (%s)\r\n", bor, bor_str[bor]);

    printf("  WDG_SW     = %u (%s)\r\n", (optcr >> 5) & 1,
           (optcr >> 5) & 1 ? "software WDG" : "hardware WDG");
    printf("  nRST_STOP  = %u\r\n", (optcr >> 6) & 1);
    printf("  nRST_STDBY = %u\r\n", (optcr >> 7) & 1);

    uint16_t nwrp = (optcr >> 16) & 0xFFF;
    printf("  nWRP       = 0x%03X (%s)\r\n", nwrp,
           nwrp == 0xFFF ? "no write protect" : "sectors protected");
}

// Decode STM32F1/F3 option bytes
static void decode_f1_options(uint32_t obr, uint32_t raw_opt) {
    uint8_t rdp = raw_opt & 0xFF;
    printf("  RDP        = 0x%02X (Level %s)\r\n", rdp,
           rdp == 0xA5 ? "0" : "1");
    printf("  RDPRT      = %u\r\n", (obr >> 1) & 1);
    printf("  WDG_SW     = %u (%s)\r\n", (obr >> 2) & 1,
           (obr >> 2) & 1 ? "software WDG" : "hardware WDG");
    printf("  nRST_STOP  = %u\r\n", (obr >> 3) & 1);
    printf("  nRST_STDBY = %u\r\n", (obr >> 4) & 1);

    uint8_t data0 = (raw_opt >> 16) & 0xFF;
    uint8_t data1 = (raw_opt >> 24) & 0xFF;
    printf("  Data0      = 0x%02X\r\n", data0);
    printf("  Data1      = 0x%02X\r\n", data1);
}

// Decode WRP register (L4)
static void decode_l4_wrp(const char *name, uint32_t val) {
    uint8_t start = val & 0xFF;
    uint8_t end = (val >> 16) & 0xFF;
    if (start > end)
        printf("  %s: disabled (start > end)\r\n", name);
    else
        printf("  %s: pages %u-%u protected\r\n", name, start, end);
}

bool swd_stm32_read_options(const stm32_target_info_t *info) {
    uint32_t optr;
    if (!mem_read32(info->flash_optr, &optr))
        return false;
    printf("FLASH_OPTR (0x%08X) = 0x%08X\r\n",
           (unsigned)info->flash_optr, (unsigned)optr);

    // L4: decode OPTR + WRP/PCROP
    if (info->flash_optr == 0x40022020) {
        decode_l4_optr(optr);

        uint32_t val;
        if (mem_read32(0x40022024, &val))
            printf("PCROP1SR = 0x%08X (start page %u)\r\n",
                   (unsigned)val, val & 0xFFFF);
        if (mem_read32(0x40022028, &val)) {
            printf("PCROP1ER = 0x%08X (end page %u, PCROP_RDP=%u)\r\n",
                   (unsigned)val, val & 0xFFFF, (val >> 31) & 1);
        }
        if (mem_read32(0x4002202C, &val)) {
            printf("WRP1AR   = 0x%08X\r\n", (unsigned)val);
            decode_l4_wrp("WRP1A", val);
        }
        if (mem_read32(0x40022030, &val)) {
            printf("WRP1BR   = 0x%08X\r\n", (unsigned)val);
            decode_l4_wrp("WRP1B", val);
        }
    }
    // F4: decode OPTCR
    else if (info->flash_optr == 0x40023C14) {
        decode_f4_optcr(optr);
    }
    // F1/F3: decode OBR + raw option bytes
    else if (info->flash_optr == 0x4002201C) {
        uint32_t raw_opt;
        if (mem_read32(info->opt_base, &raw_opt)) {
            decode_f1_options(optr, raw_opt);
        } else {
            // Raw opt bytes unreadable (RDP1) — decode from OBR only
            swd_clear_errors();
            printf("  RDPRT      = %u (%s)\r\n", (optr >> 1) & 1,
                   (optr >> 1) & 1 ? "RDP Level 1" : "RDP Level 0");
            printf("  WDG_SW     = %u (%s)\r\n", (optr >> 2) & 1,
                   (optr >> 2) & 1 ? "software WDG" : "hardware WDG");
            printf("  nRST_STOP  = %u\r\n", (optr >> 3) & 1);
            printf("  nRST_STDBY = %u\r\n", (optr >> 4) & 1);
            printf("  (raw option bytes not readable at RDP1)\r\n");
        }
    }

    return true;
}

bool swd_stm32_set_rdp(const stm32_target_info_t *info, uint8_t level) {
    uint8_t rdp_val;
    if (level == 0)
        rdp_val = info->rdp_level0;
    else if (level == 1)
        rdp_val = info->rdp_level1;
    else if (level == 2)
        rdp_val = info->rdp_level2;
    else
        return false;

    printf("[SWD] set_rdp: level=%u, rdp_val=0x%02X\r\n", level, rdp_val);

    // F1/F3 path: SWD-only option byte programming via connect-under-reset.
    // Halt at reset vector before firmware runs, then program option bytes.
    if (info->flash_base == 0x40022000) {
        #define F1_CR_OPTWRE (1u << 9)
        #define F1_CR_OPTER  (1u << 5)
        #define F1_CR_STRT   (1u << 6)
        #define F1_CR_OPTPG  (1u << 4)

        // Connect under reset to halt before firmware/watchdog runs
        if (!swd_connect_under_reset()) {
            printf("[SWD] F1: connect-under-reset failed\r\n");
            return false;
        }

        // Unlock flash + option bytes
        mem_write32(info->flash_keyr, info->flash_key1);
        mem_write32(info->flash_keyr, info->flash_key2);
        mem_write32(info->flash_optkeyr, info->opt_key1);
        mem_write32(info->flash_optkeyr, info->opt_key2);

        uint32_t cr;
        mem_read32(info->flash_cr, &cr);
        if (!(cr & F1_CR_OPTWRE)) {
            printf("[SWD] F1: option unlock failed (CR=0x%08X)\r\n", (unsigned)cr);
            return false;
        }

        // Erase option bytes: set OPTER, then STRT
        mem_write32(info->flash_cr, F1_CR_OPTWRE | F1_CR_OPTER);
        mem_write32(info->flash_cr, F1_CR_OPTWRE | F1_CR_OPTER | F1_CR_STRT);
        swd_stm32_flash_wait(info, 2000);

        // Clear OPTER
        mem_write32(info->flash_cr, F1_CR_OPTWRE);

        // Program RDP value using halfword write
        mem_write32(info->flash_cr, F1_CR_OPTWRE | F1_CR_OPTPG);
        uint16_t opt_val = rdp_val | ((uint16_t)(~rdp_val & 0xFF) << 8);
        mem_write16(info->opt_base, opt_val);
        swd_stm32_flash_wait(info, 100);

        // Program USER byte defaults (WDG_SW=1, nRST_STOP=1, nRST_STDBY=1)
        mem_write16(info->opt_base + 2, 0x00FF);
        swd_stm32_flash_wait(info, 100);

        // Clear programming mode
        mem_write32(info->flash_cr, 0);

        // Reset to apply — RDP0 transition triggers mass erase
        swd_nrst_pulse(10);
        if (level == 0) {
            printf("[SWD] F1: waiting for mass erase...\r\n");
            sleep_ms(5000);
        } else {
            sleep_ms(500);
        }

        // Reconnect and verify
        connected = false;
        ahb_initialized = false;
        sleep_ms(100);
        if (swd_connect()) {
            int new_rdp = swd_stm32_read_rdp(info);
            printf("[SWD] F1: RDP level = %d\r\n", new_rdp);
            if (new_rdp == level) {
                printf("[SWD] F1: RDP change successful!\r\n");
                return true;
            }
        }
        printf("[SWD] F1: RDP change failed\r\n");
        return false;
    }

    // Non-F1 path: normal unlock with verification
    swd_halt();
    sleep_ms(10);

    // Unlock flash + option bytes
    if (!stm32_opt_unlock(info)) {
        printf("[SWD] set_rdp: opt_unlock failed\r\n");
        return false;
    }

    // Wait for any ongoing operation
    if (!swd_stm32_flash_wait(info, 1000)) {
        printf("[SWD] set_rdp: flash_wait failed\r\n");
        return false;
    }

    // Family-specific option byte programming
    // F4: modify OPTCR register directly
    if (info->flash_optr == 0x40023C14) {
        uint32_t optcr;
        if (!mem_read32(info->flash_optr, &optcr))
            return false;
        optcr = (optcr & ~0xFF00) | ((uint32_t)rdp_val << 8);
        optcr |= (1 << 1);  // OPTSTRT
        if (!mem_write32(info->flash_optr, optcr))
            return false;
    }
    // L4: modify OPTR register, then fire OPTSTRT in CR
    else if (info->flash_optr == 0x40022020) {
        uint32_t optr;
        if (!mem_read32(info->flash_optr, &optr))
            return false;
        optr = (optr & ~0xFF) | rdp_val;
        if (!mem_write32(info->flash_optr, optr))
            return false;
        // Set OPTSTRT in CR
        uint32_t cr;
        if (!mem_read32(info->flash_cr, &cr))
            return false;
        cr |= (1 << 17);  // OPTSTRT
        if (!mem_write32(info->flash_cr, cr))
            return false;
    }

    // Wait for completion
    if (!swd_stm32_flash_wait(info, 5000))
        return false;

    // L4: launch option byte reload (triggers reset)
    if (info->flash_optr == 0x40022020) {
        uint32_t cr;
        if (!mem_read32(info->flash_cr, &cr))
            return false;
        cr |= (1 << 27);  // OBL_LAUNCH
        mem_write32(info->flash_cr, cr);  // Target will reset, may fail
    }

    return true;
}

bool swd_stm32_flash_erase_page(const stm32_target_info_t *info, uint32_t page) {
    if (!swd_stm32_flash_unlock(info))
        return false;

    if (!swd_stm32_flash_wait(info, 1000))
        return false;

    // L4: PER | PNB | STRT
    if (info->flash_optr == 0x40022020) {
        uint32_t cr = (1 << 1)          // PER
                    | (page << 3)       // PNB[7:0] shifted to bits [10:3]
                    | (1 << 16);        // STRT
        if (!mem_write32(info->flash_cr, cr))
            return false;
    }
    // F1/F3: set PER, write page address to AR, set STRT
    else if (info->flash_optr == 0x4002201C) {
        uint32_t cr = (1 << 1);  // PER
        if (!mem_write32(info->flash_cr, cr))
            return false;
        // FLASH_AR is at flash_base + 0x14
        uint32_t page_addr = 0x08000000 + page * info->page_size;
        if (!mem_write32(info->flash_base + 0x14, page_addr))
            return false;
        cr |= (1 << 6);  // STRT
        if (!mem_write32(info->flash_cr, cr))
            return false;
    }
    // F4: sector erase via OPTCR — different model, use SNB field
    else {
        uint32_t cr = (page << 3)       // SNB
                    | (1 << 1)          // SER
                    | (1 << 16);        // STRT
        if (!mem_write32(info->flash_cr, cr))
            return false;
    }

    return swd_stm32_flash_wait(info, 5000);
}

uint32_t swd_stm32_flash_write(const stm32_target_info_t *info, uint32_t addr,
                                const uint8_t *data, uint32_t len) {
    if (!swd_stm32_flash_unlock(info))
        return 0;

    if (!swd_stm32_flash_wait(info, 1000))
        return 0;

    uint32_t written = 0;

    // L4: double-word (64-bit) programming
    if (info->flash_optr == 0x40022020) {
        // Set PG bit
        if (!mem_write32(info->flash_cr, (1 << 0)))  // PG
            return 0;

        // Write 8 bytes at a time (two 32-bit words)
        while (written + 8 <= len) {
            uint32_t w0 = data[written] | (data[written+1] << 8) |
                         (data[written+2] << 16) | (data[written+3] << 24);
            uint32_t w1 = data[written+4] | (data[written+5] << 8) |
                         (data[written+6] << 16) | (data[written+7] << 24);
            if (!mem_write32(addr + written, w0))
                break;
            if (!mem_write32(addr + written + 4, w1))
                break;
            if (!swd_stm32_flash_wait(info, 1000))
                break;
            written += 8;
        }

        // Clear PG bit
        mem_write32(info->flash_cr, 0);
    }
    // F1/F3: halfword (16-bit) programming — requires 16-bit bus access
    else if (info->flash_optr == 0x4002201C) {
        if (!mem_write32(info->flash_cr, (1 << 0)))  // PG
            return 0;

        while (written + 2 <= len) {
            uint16_t hw = data[written] | (data[written+1] << 8);
            if (!mem_write16(addr + written, hw))
                break;
            if (!swd_stm32_flash_wait(info, 1000))
                break;
            written += 2;
        }

        mem_write32(info->flash_cr, 0);
    }
    // F4: word (32-bit) programming with PSIZE
    else {
        // PG | PSIZE=2 (32-bit)
        if (!mem_write32(info->flash_cr, (1 << 0) | (2 << 8)))
            return 0;

        while (written + 4 <= len) {
            uint32_t w = data[written] | (data[written+1] << 8) |
                        (data[written+2] << 16) | (data[written+3] << 24);
            if (!mem_write32(addr + written, w))
                break;
            if (!swd_stm32_flash_wait(info, 1000))
                break;
            written += 4;
        }

        mem_write32(info->flash_cr, 0);
    }

    return written;
}
