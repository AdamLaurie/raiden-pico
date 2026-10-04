// STM32 system bootloader over I2C (AN4221), bit-banged on the target UART pins.
// See i2c_bootloader.h and stm32_payloads/f4/stm32f401_bootrom_analysis.md.

#include "i2c_bootloader.h"
#include "config.h"
#include "uart_cli.h"
#include "swd.h"          // swd_nrst_pulse
#include "target_uart.h"  // target_power_ensure_on
#include "hardware/gpio.h"
#include "pico/stdlib.h"

// Pins — reuse target UART1 (mutually exclusive with UART boot).
#define I2C_SCL_PIN 4   // GP4 (= TARGET_UART TX)
#define I2C_SDA_PIN 5   // GP5 (= TARGET_UART RX)

#define I2C_DLY_US       100    // ~5 kHz half-bit (slow, for weak/internal pull-ups only)
#define I2C_STRETCH_US   80000  // max clock-stretch wait. Measured worst case on F401:
                                // ~17 ms (read) / ~34 ms (write-commit); 80 ms is safe headroom.
#define BL_ACK           0x79
#define BL_NACK          0x1F

// ---- open-drain bit-bang primitives (idle = released/high via pull-ups) ----
static inline void dly(void) { sleep_us(I2C_DLY_US); }
static inline void sda_lo(void) { gpio_set_dir(I2C_SDA_PIN, GPIO_OUT); gpio_put(I2C_SDA_PIN, 0); }
static inline void sda_hi(void) { gpio_set_dir(I2C_SDA_PIN, GPIO_IN); }   // pull-up -> 1
static inline int  sda_get(void) { return gpio_get(I2C_SDA_PIN); }
static inline void scl_lo(void) { gpio_set_dir(I2C_SCL_PIN, GPIO_OUT); gpio_put(I2C_SCL_PIN, 0); }
static inline bool scl_hi(void) {                 // release + honour clock-stretch
    gpio_set_dir(I2C_SCL_PIN, GPIO_IN);
    uint64_t t = time_us_64();
    while (!gpio_get(I2C_SCL_PIN)) {
        if (time_us_64() - t > I2C_STRETCH_US) return false;  // stretch timed out
    }
    return true;
}

static void i2c_pins_init(void);
void i2c_pins_reinit(void);

// Re-init GP4/GP5 as the bit-banged I2C master without touching target state —
// used by ROMFPB, which drives the boot sequence itself and must NOT call
// i2c_bl_enter() (that would POR away the FPB patch it just armed).
void i2c_pins_reinit(void) {
    i2c_pins_init();
}

static void i2c_pins_init(void) {    gpio_deinit(I2C_SCL_PIN);
    gpio_deinit(I2C_SDA_PIN);
    gpio_init(I2C_SCL_PIN);
    gpio_init(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    gpio_pull_up(I2C_SDA_PIN);
    sda_hi();
    scl_hi();
    dly();
}

static void i2c_start(void) { sda_hi(); scl_hi(); dly(); sda_lo(); dly(); scl_lo(); dly(); }
static void i2c_stop(void)  { sda_lo(); dly(); scl_hi(); dly(); sda_hi(); dly(); }

// write one byte, return true if slave ACKed
static bool i2c_wr(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) sda_hi(); else sda_lo();
        dly();
        scl_hi(); dly();
        scl_lo(); dly();
    }
    sda_hi();                 // release for ACK
    dly();
    scl_hi(); dly();
    int ack = sda_get();      // 0 = ACK
    scl_lo(); dly();
    return ack == 0;
}

// read one byte; send_ack=true -> ACK (more to come), false -> NACK (last)
static uint8_t i2c_rd(bool send_ack) {
    uint8_t b = 0;
    sda_hi();
    for (int i = 0; i < 8; i++) {
        dly();
        scl_hi(); dly();
        b = (b << 1) | (sda_get() & 1);
        scl_lo();
    }
    if (send_ack) sda_lo(); else sda_hi();
    dly();
    scl_hi(); dly();
    scl_lo(); dly();
    sda_hi();
    return b;
}

#define ADDR_W(a) ((uint8_t)((a) << 1))
#define ADDR_R(a) ((uint8_t)(((a) << 1) | 1))

// Between transfers the bus must idle high (open-drain + pull-ups). If SCL won't
// rise, the slave is stuck/absent with the line held low and every remaining bit
// would burn the full 80ms stretch timeout — and a low SDA during the ACK slot
// reads as a false "ACK", so the retry loop would spin ~5min per command instead
// of failing. Fail fast instead.
static bool i2c_scl_free(void) {
    uint64_t t = time_us_64();
    while (!gpio_get(I2C_SCL_PIN)) {
        // must exceed the F401's measured worst-case legit stretch (~17ms) or we
        // false-flag a healthy stretching slave; still bounds a silent-slave
        // bl_wait_ack loop to ~8s instead of ~5min.
        if (time_us_64() - t > 25000) return false;  // 25ms grace, then declare stuck
    }
    return true;
}

// AN4221: after a command the device is polled by repeating (START + read-addr);
// it NACKs its address while busy, ACKs when ready, then returns the status byte.
static bool bl_wait_ack(uint8_t a, int retries) {
    for (int r = 0; r < retries; r++) {
        if (!i2c_scl_free()) {
            uart_cli_send("    (I2C bus stuck: SCL held low — aborting wait)\r\n");
            return false;
        }
        i2c_start();
        bool addr_ok = i2c_wr(ADDR_R(a));
        if (addr_ok) {
            uint8_t s = i2c_rd(false);
            i2c_stop();
            return s == BL_ACK;
        }
        i2c_stop();
        sleep_us(200);
    }
    return false;
}

// Send the command frame with a hook fired AFTER the command byte's byte-level
// ACK and BEFORE the complement byte is sent — i.e. while the ROM is about to
// validate cmd/~cmd and run its post-command processing (the RDP check for
// gated commands). The hook is where I2CGATE fires its power glitch so the dip
// overlaps the check instead of landing after the verdict has latched.
bool i2c_bl_cmd_with_hook(uint8_t a, uint8_t cmd, void (*hook)(void), void (*hook2)(void)) {
    i2c_start();
    bool addr_ok = i2c_wr(ADDR_W(a));
    sleep_us(1000);
    i2c_wr(cmd);
    if (hook) hook();                  // glitch window 1: after cmd byte
    sleep_us(200);
    i2c_wr((uint8_t)(cmd ^ 0xFF));
    if (hook2) hook2();                // glitch window 2: after ~cmd, before STOP
    i2c_stop();
    return addr_ok;
}

// Fire a hook at the *ACK-slot edge* of a byte — the moment SCL rises for the
// ACK clock. This is the earliest trustworthy anchor: at the cmd byte it is
// t=0 of the ROM's ~0.6us dispatch+rdp_locked()+verdict sequence (the whole
// verdict latches ~600ns after this edge), whereas the post-ACK hooks in
// i2c_bl_cmd_with_hook run ~400us later (after the trailing dly()s) — far too
// late to touch the verdict. The ACK slot itself is dly()-paced (~100us), so
// the hook lands INSIDE the slave's stretch — the CPU is still running.
// NOTE: this is a low-level primitive; callers wanting hooks per byte use
// i2c_bl_cmd_with_hook (post-byte hooks) or a custom frame builder.
static bool i2c_wr_ack_hook(uint8_t b, void (*ack_hook)(void)) {
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) sda_hi(); else sda_lo();
        dly();
        scl_hi(); dly();
        scl_lo(); dly();
    }
    sda_hi();                 // release for ACK
    dly();
    if (ack_hook) ack_hook(); // fire BEFORE the ACK rise: at RDP1 the slave
                              // clock-stretches through the verdict, so firing
                              // after scl_hi() returns would be after the verdict
    scl_hi();                 // ACK clock rises (honouring any stretch)
    dly();
    int ack = sda_get();      // 0 = ACK
    scl_lo(); dly();
    return ack == 0;
}

// Send the cmd frame with a hook fired at the cmd byte's ACK-slot SCL rise —
// t=0 of the ROM's dispatch+rdp_locked()+verdict sequence (~540ns @ 48MHz).
// The hook fires while SCL is high in the ACK slot; the rest of the frame
// (~cmd + STOP) is clocked out normally afterwards (a rail dip doesn't stop
// the master from driving the bus).
bool i2c_bl_send_cmd_ackhook(uint8_t a, uint8_t cmd, void (*ack_hook)(void)) {
    i2c_start();
    bool addr_ok = i2c_wr(ADDR_W(a));
    sleep_us(1000);
    i2c_wr_ack_hook(cmd, ack_hook);
    sleep_us(200);
    i2c_wr((uint8_t)(cmd ^ 0xFF));
    i2c_stop();
    return addr_ok;
}

// Send the command frame and return immediately (no verdict wait) — leaves the
// ROM inside its post-command processing, the window I2CGATE glitches into.
bool i2c_bl_send_cmd_raw(uint8_t a, uint8_t cmd) {
    i2c_start();
    bool addr_ok = i2c_wr(ADDR_W(a));
    sleep_us(1000);
    i2c_wr(cmd);
    sleep_us(200);
    i2c_wr((uint8_t)(cmd ^ 0xFF));
    i2c_stop();
    return addr_ok;
}

// Poll the device's post-command status: repeat (START + read-addr); when the
// address ACKs, read the status byte. *status_ack = (byte == 0x79). Returns
// false if the bus went stuck or the device never answered within retries
// (typically a BOR reboot from a glitch). Used by I2CGATE to classify the
// post-glitch verdict.
bool i2c_bl_wait_status(uint8_t a, int retries, bool *status_ack) {
    for (int r = 0; r < retries; r++) {
        if (!i2c_scl_free()) return false;
        i2c_start();
        bool ok = i2c_wr(ADDR_R(a));
        if (ok) {
            uint8_t s = i2c_rd(false);
            i2c_stop();
            if (status_ack) *status_ack = (s == BL_ACK);
            return true;
        }
        i2c_stop();
        sleep_us(200);
    }
    return false;
}

// Send a command frame: START, W-addr, cmd, ~cmd, STOP; then wait for ACK.
static bool bl_cmd(uint8_t a, uint8_t cmd) {
    i2c_start();
    bool a1 = i2c_wr(ADDR_W(a));
    sleep_us(1000);                    // let the STM32 slave clear ADDR after the match
    bool a2 = i2c_wr(cmd);
    sleep_us(200);
    bool a3 = i2c_wr((uint8_t)(cmd ^ 0xFF));
    i2c_stop();
    (void)a2; (void)a3;                // cmd/xor byte-level ACKs unused (STM32 checks at wait)
    if (!a1) return false;             // address itself NACKed -> device not present/ready
    return bl_wait_ack(a, 400);
}

void i2c_bl_enter(void) {
    extern void swd_deinit(void);
    extern bool swd_is_connected(void);
    extern void target_power_cycle(uint32_t time_ms);

    // Release SWD so the reset produces a clean, free-running boot. On Cortex-M an
    // nRST pulse alone does NOT clear a debug halt left by an attached SWD session,
    // so if SWD was live we power-cycle (POR) to fully clear debug state — same
    // approach as TARGET SYNC. Without this the bootloader can come up debug-held
    // and never run its interface detection (no I2C ACK).
    bool was_swd = swd_is_connected();
    swd_deinit();

    target_power_ensure_on();
    gpio_init(PIN_BOOT0); gpio_set_dir(PIN_BOOT0, GPIO_OUT); gpio_put(PIN_BOOT0, 1); // system memory
    gpio_init(PIN_BOOT1); gpio_set_dir(PIN_BOOT1, GPIO_OUT); gpio_put(PIN_BOOT1, 0);
    i2c_pins_init();

    if (was_swd) {
        target_power_cycle(150);   // POR with BOOT0=1 -> clean bootloader boot
    } else {
        swd_nrst_pulse(20);        // plain reset into the bootloader
    }
    sleep_ms(60);                  // ROM comes up + interface detection arms
    uart_cli_send("OK: target reset into bootloader; I2C master on GP4=SCL/GP5=SDA\r\n");
}

bool i2c_bl_scan(uint8_t *found) {
    bool any = false;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        i2c_start();
        bool ack = i2c_wr(ADDR_W(a));
        i2c_stop();
        if (ack) {
            uart_cli_printf("  I2C ACK at 0x%02X (7-bit)\r\n", a);
            if (!any && found) *found = a;
            any = true;
        }
        sleep_us(200);
    }
    if (!any) uart_cli_send("  no I2C device ACKed (0x08..0x77)\r\n");
    return any;
}

bool i2c_bl_get(uint8_t a) {
    if (!bl_cmd(a, 0x00)) { uart_cli_send("ERROR: I2C Get: no ACK\r\n"); return false; }
    // Read the data frame: N, version, N command bytes, trailing ACK.
    i2c_start();
    if (!i2c_wr(ADDR_R(a))) { i2c_stop(); uart_cli_send("ERROR: I2C Get: read-addr NACK\r\n"); return false; }
    uint8_t n = i2c_rd(true);          // number of commands (N)
    uint8_t ver = i2c_rd(true);        // bootloader version
    uint8_t cmds[32];
    uint32_t got = (n > sizeof(cmds)) ? sizeof(cmds) : n;
    for (uint32_t i = 0; i < got; i++) cmds[i] = i2c_rd(true);
    i2c_rd(false);                     // trailing status (NACK last)
    i2c_stop();
    // Same format as the UART bootloader's TARGET BL GET (stm32_bl_get).
    uart_cli_printf("Bootloader version: %u.%u\r\n", (ver >> 4) & 0xF, ver & 0xF);
    uart_cli_printf("Supported commands: %lu\r\n", (unsigned long)n);
    for (uint32_t i = 0; i < got; i++) {
        uint8_t c = cmds[i];
        // AN4221 names; -NS/-EXT variants are I2C-only (undocumented in AN3155)
        const char *name = "???";
        switch (c) {
            case 0x00: name = "GET"; break;
            case 0x01: name = "Get Version"; break;
            case 0x02: name = "Get ID"; break;
            case 0x11: name = "Read Memory"; break;
            case 0x21: name = "Go"; break;
            case 0x31: name = "Write Memory"; break;
            case 0x32: name = "Write Memory (no stretch)"; break;
            case 0x44: name = "Erase"; break;
            case 0x45: name = "Extended Erase"; break;
            case 0x63: name = "Write Protect"; break;
            case 0x64: name = "Write Protect (no stretch)"; break;
            case 0x73: name = "Write Unprotect"; break;
            case 0x74: name = "Write Unprotect (no stretch)"; break;
            case 0x82: name = "Readout Protect"; break;
            case 0x83: name = "Readout Protect (extended)"; break;
            case 0x92: name = "Write Unprotect (alias)"; break;
            case 0x93: name = "Readout Unprotect"; break;
        }
        uart_cli_printf("  0x%02X  %s\r\n", c, name);
    }
    return true;
}

bool i2c_bl_get_id(uint8_t a) {
    if (!bl_cmd(a, 0x02)) { uart_cli_send("ERROR: I2C Get ID: no ACK\r\n"); return false; }
    i2c_start();
    if (!i2c_wr(ADDR_R(a))) { i2c_stop(); uart_cli_send("ERROR: I2C Get ID: read-addr NACK\r\n"); return false; }
    uint8_t n = i2c_rd(true);          // N = 1 (2 bytes of PID follow)
    uint16_t pid = 0;
    for (int i = 0; i <= n; i++) pid = (pid << 8) | i2c_rd(true);
    i2c_rd(false);
    i2c_stop();
    uart_cli_printf("I2C Get ID: PID=0x%04X\r\n", pid);
    return true;
}

bool i2c_bl_read(uint8_t a, uint32_t address, uint32_t len) {
    if (len == 0 || len > 256) { uart_cli_send("ERROR: len must be 1..256\r\n"); return false; }
    if (!bl_cmd(a, 0x11)) { uart_cli_send("ERROR: I2C Read: cmd no ACK (RDP? wrong iface?)\r\n"); return false; }

    // Address frame + XOR checksum.
    uint8_t ab[4] = { (uint8_t)(address >> 24), (uint8_t)(address >> 16),
                      (uint8_t)(address >> 8),  (uint8_t)address };
    uint8_t xsum = ab[0] ^ ab[1] ^ ab[2] ^ ab[3];
    i2c_start();
    if (!i2c_wr(ADDR_W(a))) { i2c_stop(); uart_cli_send("ERROR: I2C Read: addr-frame NACK\r\n"); return false; }
    i2c_wr(ab[0]); i2c_wr(ab[1]); i2c_wr(ab[2]); i2c_wr(ab[3]); i2c_wr(xsum);
    i2c_stop();
    if (!bl_wait_ack(a, 200)) { uart_cli_send("ERROR: I2C Read: address rejected (NACK)\r\n"); return false; }

    // Length frame: (N-1) and its complement.
    uint8_t nm1 = (uint8_t)(len - 1);
    i2c_start();
    if (!i2c_wr(ADDR_W(a))) { i2c_stop(); uart_cli_send("ERROR: I2C Read: len-frame NACK\r\n"); return false; }
    i2c_wr(nm1); i2c_wr((uint8_t)(nm1 ^ 0xFF));
    i2c_stop();
    if (!bl_wait_ack(a, 200)) { uart_cli_send("ERROR: I2C Read: length rejected (NACK)\r\n"); return false; }

    // Data frame.
    i2c_start();
    if (!i2c_wr(ADDR_R(a))) { i2c_stop(); uart_cli_send("ERROR: I2C Read: data read-addr NACK\r\n"); return false; }
    uint8_t line[16]; uint32_t got = 0;
    while (got < len) {
        uint8_t b = i2c_rd(got < len - 1);   // ACK all but last
        line[got % 16] = b;
        got++;
        if (got % 16 == 0 || got == len) {
            uint32_t base = address + got - ((got % 16) ? (got % 16) : 16);
            uint32_t nl = (got % 16) ? (got % 16) : 16;
            uart_cli_printf("0x%08lX:", (unsigned long)base);
            for (uint32_t j = 0; j < nl; j++) uart_cli_printf(" %02X", line[j]);
            uart_cli_send("\r\n");
        }
    }
    i2c_stop();
    uart_cli_printf("I2C Read complete: %lu bytes\r\n", (unsigned long)len);
    return true;
}

// Readout Unprotect (No-Stretch variant 0x93): mass-erase + remove RDP + reset.
// No-Stretch is used because the erase is long — the device NACKs its address
// while busy and we poll (bit-bang can't hold through a multi-second stretch).
// Destructive: erases all flash. Target resets to RDP0 afterwards.
bool i2c_bl_ru(uint8_t a) {
    i2c_start();
    bool a1 = i2c_wr(ADDR_W(a));
    sleep_us(1000);
    i2c_wr(0x93);
    sleep_us(200);
    i2c_wr((uint8_t)(0x93 ^ 0xFF));
    i2c_stop();
    if (!a1) { uart_cli_send("ERROR: I2C RU: no addr-ACK (device not in bootloader)\r\n"); return false; }
    uart_cli_send("I2C RU (0x93): mass-erase + remove RDP in progress...\r\n");
    // Poll for the completion ACK; erase can take a while. Big retry budget.
    bool done = bl_wait_ack(a, 30000);
    if (done) uart_cli_send("OK: I2C Readout-Unprotect complete (target now RDP0)\r\n");
    else      uart_cli_send("ERROR: I2C RU: no completion ACK\r\n");
    return done;
}

// --- Additional AN4221 commands -------------------------------------------
// Generic gate-probe: send just a command byte + complement, report whether the
// device ACKs it. Safe for multi-frame commands (Read/Write/Go/Erase/WP/GV need
// more frames before they act) — maps RDP gating without executing anything.
// NOTE: do NOT probe RU/RP (0x92/0x93/0x82/0x83): the command byte alone triggers
// the action (mass-erase / re-lock). Probe only the multi-frame commands.
bool i2c_bl_probe(uint8_t a, uint8_t cmd) {
    i2c_start();
    bool addr_ok = i2c_wr(ADDR_W(a));
    sleep_us(1000);
    i2c_wr(cmd);
    sleep_us(200);
    i2c_wr((uint8_t)(cmd ^ 0xFF));
    i2c_stop();
    bool acc = bl_wait_ack(a, 400);
    if (!addr_ok) {
        uart_cli_printf("I2C PROBE cmd 0x%02X: no addr-ACK (device not in bootloader; re-run)\r\n", cmd);
        return false;
    }
    uart_cli_printf("I2C PROBE cmd 0x%02X: %s\r\n", cmd, acc ? "ACK (accepted)" : "NACK (gated)");
    return acc;
}

// Continue an ACCEPTED 0x11 Read command: send the address frame, length
// frame, and read back len data bytes into out[]. Used by I2CGATE after a
// glitch-corrupted gate passed (the command is already ACKed mid-flight).
// Returns true if all frames ACKed and len bytes were read.
bool i2c_bl_read_frames(uint8_t a, uint32_t address, uint8_t *out, uint32_t len) {
    uint8_t ab[4] = { (uint8_t)(address >> 24), (uint8_t)(address >> 16),
                      (uint8_t)(address >> 8), (uint8_t)address };
    uint8_t xsum = ab[0] ^ ab[1] ^ ab[2] ^ ab[3];
    bool verdict = false;
    i2c_start();
    if (!i2c_wr(ADDR_W(a))) { i2c_stop(); return false; }
    i2c_wr(ab[0]); i2c_wr(ab[1]); i2c_wr(ab[2]); i2c_wr(ab[3]); i2c_wr(xsum);
    i2c_stop();
    if (!bl_wait_ack(a, 100)) return false;

    uint8_t nm1 = (uint8_t)(len - 1);
    i2c_start();
    if (!i2c_wr(ADDR_W(a))) { i2c_stop(); return false; }
    i2c_wr(nm1); i2c_wr((uint8_t)(nm1 ^ 0xFF));
    i2c_stop();
    if (!bl_wait_ack(a, 100)) return false;

    i2c_start();
    if (!i2c_wr(ADDR_R(a))) { i2c_stop(); return false; }
    for (uint32_t j = 0; j < len; j++) out[j] = i2c_rd(j < len - 1);
    i2c_stop();
    return true;
}

// 0x01 Get Version & Read Protection Status
bool i2c_bl_gv(uint8_t a) {
    if (!bl_cmd(a, 0x01)) { uart_cli_send("ERROR: I2C GV: no ACK\r\n"); return false; }
    i2c_start();
    if (!i2c_wr(ADDR_R(a))) { i2c_stop(); uart_cli_send("ERROR: I2C GV: read-addr NACK\r\n"); return false; }
    uint8_t ver = i2c_rd(true), o1 = i2c_rd(true), o2 = i2c_rd(true);
    i2c_rd(false);
    i2c_stop();
    uart_cli_printf("I2C GV: bootloader v%u.%u, RP-status bytes 0x%02X 0x%02X\r\n",
                    (ver >> 4) & 0xF, ver & 0xF, o1, o2);
    return true;
}

// 0x21 Go <addr>
bool i2c_bl_go(uint8_t a, uint32_t addr) {
    if (!bl_cmd(a, 0x21)) { uart_cli_send("ERROR: I2C GO: cmd no ACK (gated?)\r\n"); return false; }
    uint8_t ab[4] = { (uint8_t)(addr>>24),(uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr };
    uint8_t xs = ab[0]^ab[1]^ab[2]^ab[3];
    i2c_start();
    if (!i2c_wr(ADDR_W(a))) { i2c_stop(); uart_cli_send("ERROR: I2C GO: addr-frame NACK\r\n"); return false; }
    i2c_wr(ab[0]); i2c_wr(ab[1]); i2c_wr(ab[2]); i2c_wr(ab[3]); i2c_wr(xs);
    i2c_stop();
    bool ok = bl_wait_ack(a, 400);
    uart_cli_printf("I2C GO 0x%08lX: %s\r\n", (unsigned long)addr, ok ? "accepted (jumped)" : "rejected");
    return ok;
}

// 0x31 Write Memory <addr> <data[len]>
bool i2c_bl_write(uint8_t a, uint32_t addr, const uint8_t *data, uint32_t len) {
    if (len == 0 || len > 256) { uart_cli_send("ERROR: len 1..256\r\n"); return false; }
    if (!bl_cmd(a, 0x31)) { uart_cli_send("ERROR: I2C WRITE: cmd no ACK (gated?)\r\n"); return false; }
    uint8_t ab[4] = { (uint8_t)(addr>>24),(uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr };
    i2c_start();
    if (!i2c_wr(ADDR_W(a))) { i2c_stop(); uart_cli_send("ERROR: I2C WRITE: addr-frame NACK\r\n"); return false; }
    i2c_wr(ab[0]); i2c_wr(ab[1]); i2c_wr(ab[2]); i2c_wr(ab[3]); i2c_wr(ab[0]^ab[1]^ab[2]^ab[3]);
    i2c_stop();
    if (!bl_wait_ack(a, 400)) { uart_cli_send("ERROR: I2C WRITE: address rejected\r\n"); return false; }
    uint8_t nm1 = (uint8_t)(len - 1), xs = nm1;
    i2c_start();
    if (!i2c_wr(ADDR_W(a))) { i2c_stop(); uart_cli_send("ERROR: I2C WRITE: data-frame NACK\r\n"); return false; }
    i2c_wr(nm1);
    for (uint32_t i = 0; i < len; i++) { i2c_wr(data[i]); xs ^= data[i]; }
    i2c_wr(xs);
    i2c_stop();
    bool ok = bl_wait_ack(a, 2000);
    uart_cli_printf("I2C WRITE 0x%08lX %lu bytes: %s\r\n", (unsigned long)addr, (unsigned long)len,
                    ok ? "OK" : "rejected");
    return ok;
}

// 0x45 Extended Erase (No-Stretch), mass erase
bool i2c_bl_erase_mass(uint8_t a) {
    if (!bl_cmd(a, 0x45)) { uart_cli_send("ERROR: I2C ERASE: cmd no ACK (gated?)\r\n"); return false; }
    i2c_start();
    if (!i2c_wr(ADDR_W(a))) { i2c_stop(); uart_cli_send("ERROR: I2C ERASE: frame NACK\r\n"); return false; }
    i2c_wr(0xFF); i2c_wr(0xFF); i2c_wr(0x00);   // mass-erase special code + checksum
    i2c_stop();
    bool ok = bl_wait_ack(a, 30000);
    uart_cli_printf("I2C ERASE (mass): %s\r\n", ok ? "OK" : "rejected/timeout");
    return ok;
}

// 0x83 Readout Protect (No-Stretch) — re-lock to RDP1 (+ reset). Destructive-ish.
bool i2c_bl_rp(uint8_t a) {
    i2c_start();
    bool a1 = i2c_wr(ADDR_W(a));
    sleep_us(1000);
    i2c_wr(0x83);
    sleep_us(200);
    i2c_wr((uint8_t)(0x83 ^ 0xFF));
    i2c_stop();
    if (!a1) { uart_cli_send("ERROR: I2C RP: no addr-ACK (device not in bootloader)\r\n"); return false; }
    bool ok = bl_wait_ack(a, 5000);
    uart_cli_printf("I2C RP (0x83): %s\r\n", ok ? "readout-protect set (RDP1)" : "rejected");
    return ok;
}
