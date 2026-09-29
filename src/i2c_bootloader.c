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

#define I2C_DLY_US       5      // ~100 kHz half-bit (conservative for weak pull-ups)
#define I2C_STRETCH_US   10000  // max clock-stretch wait
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
        if (time_us_64() - t > I2C_STRETCH_US) return false;
    }
    return true;
}

static void i2c_pins_init(void) {
    gpio_deinit(I2C_SCL_PIN);
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

// AN4221: after a command the device is polled by repeating (START + read-addr);
// it NACKs its address while busy, ACKs when ready, then returns the status byte.
static bool bl_wait_ack(uint8_t a, int retries) {
    for (int r = 0; r < retries; r++) {
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

// Send a command frame: START, W-addr, cmd, ~cmd, STOP; then wait for ACK.
static bool bl_cmd(uint8_t a, uint8_t cmd) {
    i2c_start();
    if (!i2c_wr(ADDR_W(a))) { i2c_stop(); return false; }
    i2c_wr(cmd);
    i2c_wr((uint8_t)(cmd ^ 0xFF));
    i2c_stop();
    return bl_wait_ack(a, 200);
}

void i2c_bl_enter(void) {
    target_power_ensure_on();
    gpio_init(PIN_BOOT0); gpio_set_dir(PIN_BOOT0, GPIO_OUT); gpio_put(PIN_BOOT0, 1); // system memory
    gpio_init(PIN_BOOT1); gpio_set_dir(PIN_BOOT1, GPIO_OUT); gpio_put(PIN_BOOT1, 0);
    i2c_pins_init();
    swd_nrst_pulse(20);   // reset into the bootloader
    sleep_ms(40);         // ROM comes up + interface detection arms
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
    uart_cli_printf("I2C bootloader: version %u.%u, %u commands:\r\n",
                    (ver >> 4) & 0xF, ver & 0xF, n);
    uart_cli_send("  ");
    for (int i = 0; i < n; i++) {
        uint8_t c = i2c_rd(true);
        uart_cli_printf("%02X ", c);
    }
    i2c_rd(false);                     // trailing status (NACK last)
    i2c_stop();
    uart_cli_send("\r\n");
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
