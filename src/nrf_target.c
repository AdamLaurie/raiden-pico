/*
 * nRF52840 target support for Raiden-Pico - APPROTECT voltage-glitch bypass.
 *
 * See nrf_target.h for the attack model and references (LimitedResults /
 * atc1441 ESP32_nRF52_SWD). Built on the vendor-neutral swd_* API.
 */

#include "nrf_target.h"
#include "swd.h"
#include "glitch.h"
#include "platform.h"
#include "config.h"
#include "uart_cli.h"
#include "pico/stdlib.h"
#include "pico/time.h"

// Target power control lives in target_uart.c
extern void target_power_on(void);
extern void target_power_off(void);
extern void target_power_on_silent(void);
extern void target_power_off_silent(void);
extern bool target_power_get_state(void);

uint32_t nrf_connect(void)
{
    // swd_connect() does a line reset + reads DPIDR only; it does NOT require
    // the AHB-AP, so it works even while the part is APPROTECT-locked.
    if (!swd_connect())
        return 0;
    uint32_t dpidr = 0;
    if (!swd_read_dp(DP_DPIDR, &dpidr))
        return 0;
    return dpidr;
}

bool nrf_is_unlocked(void)
{
    // GROUND TRUTH = "can the AHB-AP actually read memory?". CTRL-AP
    // APPROTECTSTATUS bit0 is NOT reliable on nRF52 — it can read PROTECTED while
    // the AHB-AP is in fact OPEN (memory readable), which once hid a chip that was
    // already unlocked for an entire campaign. So treat the part as unlocked if
    // EITHER APPROTECTSTATUS says unprotected OR a known FICR word reads back
    // correctly over the AHB-AP. The FICR read is the authoritative test; on a
    // locked part it simply faults and returns false.
    uint32_t status = 0;
    if (swd_read_ap(NRF_CTRL_AP, NRF_CTRLAP_APPROTECTSTATUS, &status) && (status & 1u))
        return true;
    uint32_t part = 0;
    return swd_read_mem(NRF_FICR_INFO_PART, &part, 1) && (part == 0x52840u);
}

bool nrf_read_info(nrf_info_t *i)
{
    if (!i)
        return false;
    *i = (nrf_info_t){0};

    i->dpidr = nrf_connect();
    if (i->dpidr == 0) {
        i->state = 0;
        return false;
    }

    bool unlocked = nrf_is_unlocked();
    i->state = unlocked ? 2 : 1;
    if (!unlocked)
        return true;  // connected but locked - no AHB-AP reads possible

    // Unlocked: read FICR/UICR over AHB-AP.
    swd_read_mem(NRF_FICR_INFO_PART,    &i->info_part,     1);
    swd_read_mem(NRF_FICR_INFO_VARIANT, &i->info_variant,  1);
    swd_read_mem(NRF_FICR_INFO_PACKAGE, &i->info_package,  1);
    swd_read_mem(NRF_FICR_CODEPAGESIZE, &i->codepage_size, 1);
    swd_read_mem(NRF_FICR_CODESIZE,     &i->codesize,      1);
    swd_read_mem(NRF_UICR_APPROTECT,    &i->uicr_approtect,1);
    i->flash_size = i->codepage_size * i->codesize;
    return true;
}

void nrf_cmd_info(void)
{
    nrf_info_t i;
    nrf_read_info(&i);

    uart_cli_send("=== nRF52840 Info ===\r\n");
    if (i.state == 0) {
        uart_cli_send("SWD:          NOT CONNECTED (no DPIDR)\r\n");
        return;
    }
    uart_cli_printf("DPIDR:        0x%08X%s\r\n", (unsigned)i.dpidr,
                    i.dpidr == NRF_DPIDR ? " (nRF52)" : " (unexpected)");
    uart_cli_printf("APPROTECT:    %s\r\n",
                    i.state == 2 ? "UNPROTECTED (debug open)"
                                 : "PROTECTED (locked - glitch required)");
    if (i.state != 2)
        return;

    uart_cli_printf("INFO.PART:    0x%08X\r\n", (unsigned)i.info_part);
    uart_cli_printf("INFO.VARIANT: 0x%08X\r\n", (unsigned)i.info_variant);
    uart_cli_printf("INFO.PACKAGE: 0x%08X\r\n", (unsigned)i.info_package);
    uart_cli_printf("Flash:        %u bytes (%u pages x %u)\r\n",
                    (unsigned)i.flash_size, (unsigned)i.codesize,
                    (unsigned)i.codepage_size);
    uart_cli_printf("UICR.APPROT:  0x%08X\r\n", (unsigned)i.uicr_approtect);
}

void nrf_soft_reset(void)
{
    swd_write_ap(NRF_CTRL_AP, NRF_CTRLAP_RESET, 1);
    sleep_ms(1);
    swd_write_ap(NRF_CTRL_AP, NRF_CTRLAP_RESET, 0);
}

bool nrf_erase_all(void)
{
    if (nrf_connect() == 0) {
        uart_cli_send("ERROR: no SWD link for ERASEALL\r\n");
        return false;
    }
    uart_cli_send("CTRL-AP ERASEALL (mass erase + recover)...\r\n");
    bool w = swd_write_ap(NRF_CTRL_AP, NRF_CTRLAP_ERASEALL, 1);
    uart_cli_printf("  ERASEALL write ack=%d; waiting 600 ms for full-chip erase...\r\n", w);
    sleep_ms(600);                     // nRF52840 full erase ~hundreds of ms; AP faults while busy

    // best-effort status read (don't abort on a transient fault)
    uint32_t status = 0xFFFFFFFF;
    bool r = swd_read_ap(NRF_CTRL_AP, NRF_CTRLAP_ERASEALLSTATUS, &status);
    uart_cli_printf("  ERASEALLSTATUS read=%d val=0x%X (0=done)\r\n", r, (unsigned)status);

    swd_write_ap(NRF_CTRL_AP, NRF_CTRLAP_ERASEALL, 0);
    nrf_soft_reset();
    sleep_ms(150);

    uint32_t dpidr = nrf_connect();
    bool unlocked = (dpidr != 0) && nrf_is_unlocked();
    uart_cli_printf("After erase+reset: DPIDR=0x%08X  APPROTECT=%s\r\n",
                    (unsigned)dpidr, unlocked ? "UNPROTECTED (debug open!)" : "PROTECTED");
    return unlocked;
}

// Forward decl: bring SWD lines hi-Z so they don't back-power the target.
static void nrf_swd_idle(void);

void nrf_glitch_shot(uint32_t delay_us, uint32_t width_cyc)
{
    if (width_cyc > NRF_GLITCH_WIDTH_MAX_CYC) {
        uart_cli_printf("WARN: width %u cyc (>~3us) risks mass-erase; clamping to %u\r\n",
                        (unsigned)width_cyc, (unsigned)NRF_GLITCH_WIDTH_MAX_CYC);
        width_cyc = NRF_GLITCH_WIDTH_MAX_CYC;
    }
    if (width_cyc < NRF_GLITCH_WIDTH_MIN_CYC)
        width_cyc = NRF_GLITCH_WIDTH_MIN_CYC;

    platform_set_type(PLATFORM_CROWBAR);
    glitch_set_width(width_cyc);

    nrf_swd_idle();
    target_power_off_silent();
    glitch_arm();          // arm during the OFF window: glitch_arm()'s PIO program
                           // add/remove + SM init must NOT happen after the delay
                           // wait, or it pushes the real fire late and we overshoot
                           // the early-boot latch window.
    sleep_ms(50);
    target_power_on_silent();   // SILENT: target_power_on()'s uart_cli_send/fflush
                                // blocks ~74 us on USB and adds jitter in-path.
    busy_wait_us(delay_us);  // delay now measured cleanly from power-on
    glitch_execute();   // single pulse on GP2; GP22 marker pulses for the scope

    uart_cli_printf("SHOT: delay=%u us, width=%u cyc (~%u ns). Trigger scope on GP22.\r\n",
                    (unsigned)delay_us, (unsigned)width_cyc,
                    (unsigned)((width_cyc * 20u) / 3u));
}

// Single shot for RESET-boot scope work (fault-mapping the nRST window): reboot via
// nRST (GP15, VDD stays on), wait `delay_us` from the release edge, fire one crowbar
// pulse. No SWD probe -- so the scope is free to capture the marker (boot survival).
// Trigger the scope on the GP15 RELEASE edge (CH1) and watch the blink marker (CH2):
// boot survived = marker resumes; boot crashed = marker stays static. Pairs with
// nrf_timing_marker.py resetcrashmap to localise the sensitive delay window.
void nrf_glitch_shot_reset(uint32_t delay_us, uint32_t width_cyc, uint32_t reset_hold_us)
{
    if (width_cyc > NRF_GLITCH_WIDTH_MAX_CYC) {
        uart_cli_printf("WARN: width %u cyc (>~3us) risks mass-erase; clamping to %u\r\n",
                        (unsigned)width_cyc, (unsigned)NRF_GLITCH_WIDTH_MAX_CYC);
        width_cyc = NRF_GLITCH_WIDTH_MAX_CYC;
    }
    if (width_cyc < NRF_GLITCH_WIDTH_MIN_CYC)
        width_cyc = NRF_GLITCH_WIDTH_MIN_CYC;
    if (reset_hold_us < 120) reset_hold_us = 120;   // must exceed glitch_arm() PIO setup

    platform_set_type(PLATFORM_CROWBAR);
    target_power_on_silent();       // ensure VDD is ON (idempotent; reset-glitch keeps VDD up,
                                    // does NOT power-cycle) -- without this the part is unpowered
                                    // and the boot/marker never runs.
    glitch_set_width(width_cyc);

    nrf_swd_idle();
    swd_nrst_assert();              // nRST low = reset asserted
    glitch_arm();                  // arm during the hold so PIO setup is absorbed
    busy_wait_us(reset_hold_us);
    gpio_set_dir(SWD_NRST_PIN, GPIO_OUT);
    gpio_put(SWD_NRST_PIN, 1);      // crisp active-high release edge (scope trigger on GP15)
    busy_wait_us(delay_us);         // delay measured from the release edge
    glitch_execute();               // crowbar on GP2 -> DEC1; GP22 marker pulses

    uart_cli_printf("SHOTRST: delay=%u us from nRST release, width=%u cyc, hold=%u us. "
                    "Trigger scope on GP15 release.\r\n",
                    (unsigned)delay_us, (unsigned)width_cyc, (unsigned)reset_hold_us);
}

uint32_t nrf_dump_mem(uint32_t addr, uint32_t bytes)
{
    if (!nrf_is_unlocked()) {
        uart_cli_send("ERROR: part is locked - cannot read memory (run TARGET GLITCH APPROTECT first)\r\n");
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

// Drive both SWD lines LOW (output 0) before cutting VDD, so they can't
// back-power the nRF through its clamp diodes while the rail is collapsed.
// Matches atc1441 (SWCLK low before power-cycle). We deliberately skip the
// graceful swd_deinit() here: the target is about to be power-cycled (debug
// state is discarded anyway) and swd_deinit's power-down wait would add ~50 ms
// to every attempt. swd_connect() fully re-initialises these pins on the next probe.
static void nrf_swd_idle(void)
{
    gpio_init(SWD_SWCLK_PIN);
    gpio_set_dir(SWD_SWCLK_PIN, GPIO_OUT);
    gpio_put(SWD_SWCLK_PIN, 0);
    gpio_init(SWD_SWDIO_PIN);
    gpio_set_dir(SWD_SWDIO_PIN, GPIO_OUT);
    gpio_put(SWD_SWDIO_PIN, 0);
}

bool nrf_glitch_approtect(uint32_t delay_start_us, uint32_t delay_end_us,
                          uint32_t delay_step_us, uint32_t width_start_cyc,
                          uint32_t width_end_cyc, uint32_t width_step_cyc,
                          uint32_t power_off_ms, uint32_t settle_ms,
                          uint32_t max_attempts)
{
    if (delay_step_us == 0)
        delay_step_us = 1;
    if (delay_end_us < delay_start_us)
        delay_end_us = delay_start_us;

    // Clamp the width sweep to the nRF52840 safe band (1-3 us). A pulse longer
    // than ~3 us risks latching a mass-erase instead of an APPROTECT-bypass
    // fault, which would wipe the flash we want to read.
    if (width_start_cyc < NRF_GLITCH_WIDTH_MIN_CYC) {
        uart_cli_printf("WARN: width_start %u<%u; raising to %u (~1us floor)\r\n",
                        (unsigned)width_start_cyc, (unsigned)NRF_GLITCH_WIDTH_MIN_CYC,
                        (unsigned)NRF_GLITCH_WIDTH_MIN_CYC);
        width_start_cyc = NRF_GLITCH_WIDTH_MIN_CYC;
    }
    if (width_end_cyc > NRF_GLITCH_WIDTH_MAX_CYC) {
        uart_cli_printf("WARN: width_end %u>%u risks mass-erase; clamping to %u (~3us)\r\n",
                        (unsigned)width_end_cyc, (unsigned)NRF_GLITCH_WIDTH_MAX_CYC,
                        (unsigned)NRF_GLITCH_WIDTH_MAX_CYC);
        width_end_cyc = NRF_GLITCH_WIDTH_MAX_CYC;
    }
    if (width_end_cyc < width_start_cyc)
        width_end_cyc = width_start_cyc;
    if (width_step_cyc == 0)
        width_step_cyc = 1;

    platform_set_type(PLATFORM_CROWBAR);

    uart_cli_send("=== nRF52840 APPROTECT glitch (delay x width sweep) ===\r\n");
    uart_cli_printf("delay %u..%u us step %u | width %u..%u cyc step %u (~%u..%u ns) | "
                    "off %u ms | settle %u ms | max %u\r\n",
                    (unsigned)delay_start_us, (unsigned)delay_end_us, (unsigned)delay_step_us,
                    (unsigned)width_start_cyc, (unsigned)width_end_cyc, (unsigned)width_step_cyc,
                    (unsigned)((width_start_cyc * 20u) / 3u), (unsigned)((width_end_cyc * 20u) / 3u),
                    (unsigned)power_off_ms, (unsigned)settle_ms, (unsigned)max_attempts);
    uart_cli_send("Crowbar on GP2 -> DEC1. Width inner / delay outer. Power-cycle Pico to abort.\r\n");

    // PRE-FLIGHT: confirm the part is actually LOCKED before wasting a sweep on it.
    // A chip left open by a prior ERASEALL/ERASEUICR once read "PROTECTED" via the
    // unreliable APPROTECTSTATUS bit, and a whole campaign ran against an open part.
    // nrf_is_unlocked() now does a real AHB FICR read, so this catches that for real.
    nrf_swd_idle();
    target_power_off_silent(); sleep_ms(power_off_ms);
    target_power_on_silent();  sleep_ms(settle_ms);
    if (nrf_connect() == NRF_DPIDR && nrf_is_unlocked()) {
        uart_cli_send("\r\n*** ABORT: part is ALREADY OPEN (debug unprotected) before any "
                      "glitch — nothing to do. Re-lock UICR.APPROTECT, then retry. ***\r\n");
        nrf_cmd_info();
        return false;
    }

    uint32_t delay_us = delay_start_us;
    uint32_t width = width_start_cyc;
    uint32_t disrupt_count = 0;   // boot crashes (DPIDR lost) - localises the effective window

    for (uint32_t attempt = 1; attempt <= max_attempts; attempt++) {
        glitch_set_width(width);

        // --- power cycle the target; arm during the OFF window so glitch_arm()'s
        //     ~90 us of PIO setup is absorbed before power-on instead of being
        //     added AFTER the delay wait (which overshot the latch window). ---
        nrf_swd_idle();
        target_power_off_silent();
        glitch_arm();
        sleep_ms(power_off_ms);
        target_power_on_silent();   // SILENT: avoid the ~74 us blocking USB print
                                    // in target_power_on() that offsets+jitters the fire.

        // --- wait into the early-boot APPROTECT-latch window, then fire ---
        busy_wait_us(delay_us);
        glitch_execute();   // pulses PIN_GLITCH_OUT (GP2) for `width` cycles

        // --- let the core settle, then probe SWD ---
        sleep_ms(settle_ms);

        uint32_t dpidr = nrf_connect();
        if (dpidr == NRF_DPIDR && nrf_is_unlocked()) {
            uart_cli_printf("\r\n*** UNLOCKED on attempt %u (delay=%u us, "
                            "width=%u cyc) ***\r\n", (unsigned)attempt,
                            (unsigned)delay_us, (unsigned)width);
            nrf_cmd_info();
            return true;
        }

        // Log boot disruptions (DPIDR lost = glitch crashed the core) to localise
        // the effective window. Capped so it can't flood the link.
        if (dpidr != NRF_DPIDR && disrupt_count < 80) {
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

        // advance: width inner, delay outer
        width += width_step_cyc;
        if (width > width_end_cyc) {
            width = width_start_cyc;
            delay_us += delay_step_us;
            if (delay_us > delay_end_us)
                delay_us = delay_start_us;
        }
    }

    uart_cli_send("No unlock. Narrow delay around the DEC1 boot window and retry.\r\n");
    return false;
}

// RESET-based APPROTECT glitch: VDD stays ON; reboot via nRST (GP15) each attempt.
// The boot then starts from a clean digital reset-release edge with DEC1/VDD already
// up -- NO analog rail-ramp jitter -- so the APPROTECT read lands at a repeatable
// delay after release (tens of us, not ~1.3ms). De-jitter without a stiff PSU.
// `delay_us` is measured from the nRST RELEASE edge. `reset_hold_us` is how long nRST
// is held low (>= the ~90us glitch_arm() PIO setup, which we absorb during the hold).
bool nrf_glitch_approtect_reset(uint32_t delay_start_us, uint32_t delay_end_us,
                                uint32_t delay_step_us, uint32_t width_start_cyc,
                                uint32_t width_end_cyc, uint32_t width_step_cyc,
                                uint32_t reset_hold_us, uint32_t settle_ms,
                                uint32_t max_attempts, bool via_ctrlap)
{
    if (delay_step_us == 0) delay_step_us = 1;
    if (delay_end_us < delay_start_us) delay_end_us = delay_start_us;
    if (width_start_cyc < NRF_GLITCH_WIDTH_MIN_CYC) width_start_cyc = NRF_GLITCH_WIDTH_MIN_CYC;
    if (width_end_cyc > NRF_GLITCH_WIDTH_MAX_CYC) width_end_cyc = NRF_GLITCH_WIDTH_MAX_CYC;
    if (width_end_cyc < width_start_cyc) width_end_cyc = width_start_cyc;
    if (width_step_cyc == 0) width_step_cyc = 1;
    if (reset_hold_us < 120) reset_hold_us = 120;   // must exceed glitch_arm() PIO setup (~90us)

    platform_set_type(PLATFORM_CROWBAR);
    target_power_on_silent();      // VDD ON and STAYS on (no power-cycle)
    sleep_ms(50);

    const char *reboot = via_ctrlap ? "CTRL-AP (over SWD, no pad)" : "nRST (GP15)";
    uart_cli_printf("=== nRF52840 APPROTECT glitch via %s RESET (VDD stays on) ===\r\n", reboot);
    uart_cli_printf("delay %u..%u us step %u (from release) | width %u..%u cyc step %u | "
                    "reset_hold %u us | settle %u ms | max %u\r\n",
                    (unsigned)delay_start_us, (unsigned)delay_end_us, (unsigned)delay_step_us,
                    (unsigned)width_start_cyc, (unsigned)width_end_cyc, (unsigned)width_step_cyc,
                    (unsigned)reset_hold_us, (unsigned)settle_ms, (unsigned)max_attempts);
    uart_cli_printf("Crowbar GP2->DEC1; reboot via %s. Width inner / delay outer. Power-cycle Pico to abort.\r\n",
                    reboot);

    // PRE-FLIGHT: reset once and confirm the part is actually LOCKED before sweeping.
    // The reset-glitch never power-cycles, so a part already open from a prior ERASE
    // stays open across a reset and would falsely read as an instant "unlock on attempt 1".
    // nrf_is_unlocked() now does a real AHB FICR read, so this is an honest check.
    if (via_ctrlap) {
        nrf_connect();              // CTRL-AP needs the SWD link up to issue the write
        nrf_soft_reset();           // CTRL-AP RESET assert+release
    } else {
        nrf_swd_idle();
        swd_nrst_assert(); busy_wait_us(reset_hold_us);
        gpio_set_dir(SWD_NRST_PIN, GPIO_OUT); gpio_put(SWD_NRST_PIN, 1);
    }
    sleep_ms(settle_ms);
    if (nrf_connect() == NRF_DPIDR && nrf_is_unlocked()) {
        uart_cli_send("\r\n*** ABORT: part is ALREADY OPEN (debug unprotected) before any "
                      "glitch — nothing to do. Re-lock UICR.APPROTECT, then retry. ***\r\n");
        nrf_cmd_info();
        return false;
    }

    uint32_t delay_us = delay_start_us;
    uint32_t width = width_start_cyc;
    uint32_t disrupt_count = 0;

    for (uint32_t attempt = 1; attempt <= max_attempts; attempt++) {
        glitch_set_width(width);

        // hold target in reset; arm during the hold so the ~90us PIO setup is absorbed
        if (via_ctrlap) {
            // CTRL-AP path: SWD must stay up to issue the AP writes (no nrf_swd_idle).
            nrf_connect();                                    // (re)establish the DP/AP link
            swd_write_ap(NRF_CTRL_AP, NRF_CTRLAP_RESET, 1);   // assert soft reset
            glitch_arm();                                     // absorb ~90us PIO setup during hold
            busy_wait_us(reset_hold_us);
            swd_write_ap(NRF_CTRL_AP, NRF_CTRLAP_RESET, 0);   // RELEASE (timing ref; SWD-jittered)
        } else {
            nrf_swd_idle();             // SWD low so the debugger doesn't hold the bus
            swd_nrst_assert();          // nRST low = reset asserted
            glitch_arm();
            busy_wait_us(reset_hold_us);
            // crisp active-high release edge (vs the high-Z+pull-up default) for repeatable timing
            gpio_set_dir(SWD_NRST_PIN, GPIO_OUT);
            gpio_put(SWD_NRST_PIN, 1);  // nRST high = release -> boot ROM runs (DEC1 already up)
        }

        busy_wait_us(delay_us);         // delay measured from the release edge
        glitch_execute();               // crowbar on GP2 -> DEC1

        sleep_ms(settle_ms);

        uint32_t dpidr = nrf_connect();
        if (dpidr == NRF_DPIDR && nrf_is_unlocked()) {
            uart_cli_printf("\r\n*** UNLOCKED on attempt %u (delay=%u us from nRST, "
                            "width=%u cyc) ***\r\n", (unsigned)attempt,
                            (unsigned)delay_us, (unsigned)width);
            nrf_cmd_info();
            return true;
        }

        if (dpidr != NRF_DPIDR && disrupt_count < 80) {
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

    uart_cli_send("No unlock (reset-glitch). Re-measure nRST->read timing and narrow the delay.\r\n");
    return false;
}
