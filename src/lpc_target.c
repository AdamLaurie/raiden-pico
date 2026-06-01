#include "lpc_target.h"
#include "config.h"
#include "uart_cli.h"
#include "target_uart.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TARGET_UART_ID uart1

// ----- Per-family target info -----

// LPC2468 sector map (UM10237 §31.3):
//   sectors 0..7   -> 8 × 4 KB    0x00000-0x07FFF
//   sectors 8..21  -> 14 × 32 KB  0x08000-0x77FFF
//   sectors 22..27 -> 6 × 4 KB    0x78000-0x7DFFF (user accessible)
//   sectors 28..29 -> 2 × 4 KB    0x7E000-0x7FFFF (BOOT BLOCK — reserved,
//                                                  ISP rc=14 on read)
// We list only user-accessible sectors here so that mass-erase / sector
// validation can't accidentally target the boot block.
static const lpc_sector_t lpc2468_sectors[] = {
    { 0, 0x00000000, 0x1000 }, { 1, 0x00001000, 0x1000 },
    { 2, 0x00002000, 0x1000 }, { 3, 0x00003000, 0x1000 },
    { 4, 0x00004000, 0x1000 }, { 5, 0x00005000, 0x1000 },
    { 6, 0x00006000, 0x1000 }, { 7, 0x00007000, 0x1000 },
    { 8, 0x00008000, 0x8000 }, { 9, 0x00010000, 0x8000 },
    {10, 0x00018000, 0x8000 }, {11, 0x00020000, 0x8000 },
    {12, 0x00028000, 0x8000 }, {13, 0x00030000, 0x8000 },
    {14, 0x00038000, 0x8000 }, {15, 0x00040000, 0x8000 },
    {16, 0x00048000, 0x8000 }, {17, 0x00050000, 0x8000 },
    {18, 0x00058000, 0x8000 }, {19, 0x00060000, 0x8000 },
    {20, 0x00068000, 0x8000 }, {21, 0x00070000, 0x8000 },
    {22, 0x00078000, 0x1000 }, {23, 0x00079000, 0x1000 },
    {24, 0x0007A000, 0x1000 }, {25, 0x0007B000, 0x1000 },
    {26, 0x0007C000, 0x1000 }, {27, 0x0007D000, 0x1000 },
};

static const lpc_target_info_t lpc_arm7_info = {
    .name             = "LPC2xxx (ARM7TDMI-S)",
    .crp_word_addr    = 0x000001FCu,
    .crp_word_end     = 0x000001FFu,
    .has_crp          = true,
    .sectors          = lpc2468_sectors,
    .num_sectors      = sizeof(lpc2468_sectors) / sizeof(lpc2468_sectors[0]),
    // ISP scratch / vector area lives below 0x40000200; pick a 256-byte aligned
    // address well above the bootloader workspace per UM10237 §31.6.
    .ram_staging_addr = 0x40001000u,
};

static const lpc_target_info_t lpc_cm_info = {
    .name             = "LPC Cortex-M (17xx/11xx/13xx/18xx/43xx/54xxx)",
    .crp_word_addr    = 0x000002FCu,
    .crp_word_end     = 0x000002FFu,
    .has_crp          = true,
    .sectors          = NULL,   // TODO: per-family Cortex-M sector tables
    .num_sectors      = 0,
    .ram_staging_addr = 0x10001000u,  // typical LPC17xx local SRAM address
};

int lpc_sector_for_addr(const lpc_target_info_t *info, uint32_t addr) {
    if (!info || !info->sectors) return -1;
    for (uint32_t i = 0; i < info->num_sectors; i++) {
        const lpc_sector_t *s = &info->sectors[i];
        if (addr >= s->start_addr && addr < s->start_addr + s->size_bytes)
            return (int)s->number;
    }
    return -1;
}

const lpc_target_info_t *lpc_get_target_info_for(target_type_t t) {
    switch (t) {
        case TARGET_LPC:    return &lpc_arm7_info;
        case TARGET_LPC_CM: return &lpc_cm_info;
        default:            return &lpc_arm7_info;  // safe default
    }
}

const lpc_target_info_t *lpc_get_target_info(void) {
    return lpc_get_target_info_for(target_get_type());
}

// ----- ISP return codes (UM10237 Table 626) -----
#define LPC_RC_TIMEOUT_ECHO  -100
#define LPC_RC_TIMEOUT_RC    -101
#define LPC_RC_BAD_RC        -102
#define LPC_RC_TIMEOUT_DATA  -103
#define LPC_RC_BAD_UU        -104
#define LPC_RC_BAD_CHECKSUM  -105

static const char *lpc_rc_str(int rc) {
    switch (rc) {
        case 0:  return "CMD_SUCCESS";
        case 1:  return "INVALID_COMMAND";
        case 2:  return "SRC_ADDR_ERROR";
        case 3:  return "DST_ADDR_ERROR";
        case 4:  return "SRC_ADDR_NOT_MAPPED";
        case 5:  return "DST_ADDR_NOT_MAPPED";
        case 6:  return "COUNT_ERROR";
        case 7:  return "INVALID_SECTOR";
        case 8:  return "SECTOR_NOT_BLANK";
        case 9:  return "SECTOR_NOT_PREPARED_FOR_WRITE";
        case 10: return "COMPARE_ERROR";
        case 11: return "BUSY";
        case 12: return "PARAM_ERROR";
        case 13: return "ADDR_ERROR";
        case 14: return "ADDR_NOT_MAPPED";
        case 15: return "CMD_LOCKED";
        case 16: return "INVALID_CODE";
        case 17: return "INVALID_BAUD_RATE";
        case 18: return "INVALID_STOP_BIT";
        case 19: return "CODE_READ_PROTECTION_ENABLED";
        case LPC_RC_TIMEOUT_ECHO:  return "timeout (no echo — run TARGET SYNC?)";
        case LPC_RC_TIMEOUT_RC:    return "timeout waiting for return code";
        case LPC_RC_BAD_RC:        return "malformed return code";
        case LPC_RC_TIMEOUT_DATA:  return "timeout waiting for data";
        case LPC_RC_BAD_UU:        return "malformed UU-encoded line";
        case LPC_RC_BAD_CHECKSUM:  return "checksum mismatch";
        default:                   return "unknown";
    }
}

// ----- Low-level UART helpers -----

static void lpc_begin(void) {
    // Disable UART RX IRQ so we own the FIFO during the transaction
    uart_set_irq_enables(TARGET_UART_ID, false, false);
    while (uart_is_readable(TARGET_UART_ID))
        (void)uart_getc(TARGET_UART_ID);
}

static void lpc_end(void) {
    uart_set_irq_enables(TARGET_UART_ID, true, false);
}

// Read one CR/LF-terminated line into buf (NUL-terminated, \r and \n stripped).
// Returns line length on success, negative on timeout/overflow.
static int lpc_recv_line(char *buf, size_t maxlen, uint32_t timeout_ms) {
    size_t pos = 0;
    uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000;
    bool got_any = false;
    while (pos + 1 < maxlen) {
        if (time_us_64() > deadline) {
            buf[pos] = '\0';
            return got_any ? -2 : -1;
        }
        if (uart_is_readable(TARGET_UART_ID)) {
            char c = (char)uart_getc(TARGET_UART_ID);
            got_any = true;
            if (target_get_debug())
                uart_cli_printf("[LPC RX] %02X%s\r\n", (uint8_t)c,
                                (c >= 32 && c < 127) ? "" : "");
            if (c == '\n') {
                if (pos > 0 && buf[pos - 1] == '\r') pos--;
                buf[pos] = '\0';
                return (int)pos;
            }
            buf[pos++] = c;
        }
    }
    buf[maxlen - 1] = '\0';
    return -3;  // overflow
}

static void lpc_send_str(const char *s) {
    if (target_get_debug())
        uart_cli_printf("[LPC TX] %s\r\n", s);
    for (const char *p = s; *p; p++)
        uart_putc_raw(TARGET_UART_ID, *p);
    uart_tx_wait_blocking(TARGET_UART_ID);
}

// Send a command line and read the echo + return code.
// Returns the parsed ISP return code (0 = success) or negative LPC_RC_* on error.
static int lpc_send_cmd(const char *cmd) {
    char line[80];

    // Send command followed by \r\n
    lpc_send_str(cmd);
    uart_putc_raw(TARGET_UART_ID, '\r');
    uart_putc_raw(TARGET_UART_ID, '\n');
    uart_tx_wait_blocking(TARGET_UART_ID);

    // Consume the echoed command line (target re-emits what we sent)
    int n = lpc_recv_line(line, sizeof(line), 2000);
    if (n < 0) return LPC_RC_TIMEOUT_ECHO;

    // Read return code line
    n = lpc_recv_line(line, sizeof(line), 2000);
    if (n < 0) return LPC_RC_TIMEOUT_RC;

    // Some commands echo a blank line first when there's nothing else queued;
    // skip blank lines until we see a digit.
    int skipped = 0;
    while (n == 0 && skipped++ < 2) {
        n = lpc_recv_line(line, sizeof(line), 2000);
        if (n < 0) return LPC_RC_TIMEOUT_RC;
    }

    if (line[0] < '0' || line[0] > '9') return LPC_RC_BAD_RC;
    return atoi(line);
}

// ----- UU-decoded read -----
// Decode one UU-encoded line into out_buf, return number of bytes decoded.
// Returns -1 on malformed input.
static int lpc_uu_decode(const char *line, uint8_t *out_buf, size_t out_max) {
    if (!line[0]) return -1;
    int n = (line[0] == '`') ? 0 : ((line[0] - 0x20) & 0x3F);
    if (n < 0 || (size_t)n > out_max) return -1;
    const char *p = line + 1;
    int decoded = 0;
    while (decoded < n) {
        if (!p[0] || !p[1] || !p[2] || !p[3]) return -1;
        uint8_t d0 = (p[0] == '`') ? 0 : ((p[0] - 0x20) & 0x3F);
        uint8_t d1 = (p[1] == '`') ? 0 : ((p[1] - 0x20) & 0x3F);
        uint8_t d2 = (p[2] == '`') ? 0 : ((p[2] - 0x20) & 0x3F);
        uint8_t d3 = (p[3] == '`') ? 0 : ((p[3] - 0x20) & 0x3F);
        if (decoded < n) out_buf[decoded++] = (d0 << 2) | (d1 >> 4);
        if (decoded < n) out_buf[decoded++] = ((d1 & 0xF) << 4) | (d2 >> 2);
        if (decoded < n) out_buf[decoded++] = ((d2 & 0x3) << 6) | d3;
        p += 4;
    }
    return decoded;
}

// Pretty hex dump (matches stm32_bl_read style)
static void lpc_dump_hex_line(uint32_t base_addr, const uint8_t *buf, uint32_t line_len) {
    uart_cli_printf("0x%08lX:", (unsigned long)base_addr);
    for (uint32_t j = 0; j < line_len; j++)
        uart_cli_printf(" %02X", buf[j]);
    for (uint32_t j = line_len; j < 16; j++)
        uart_cli_send("   ");
    uart_cli_send("  ");
    for (uint32_t j = 0; j < line_len; j++) {
        char c = buf[j];
        uart_cli_printf("%c", (c >= 32 && c <= 126) ? c : '.');
    }
    uart_cli_send("\r\n");
}

// ----- Public commands -----

void lpc_bl_get(void) {
    // K — read boot version. Response after rc=0: two decimal lines (minor, major).
    lpc_begin();
    int rc = lpc_send_cmd("K");
    if (rc != 0) {
        uart_cli_printf("ERROR: K failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }

    char line[32];
    int n1 = lpc_recv_line(line, sizeof(line), 1000);
    if (n1 < 0) { uart_cli_send("ERROR: timeout reading minor\r\n"); lpc_end(); return; }
    int minor = atoi(line);

    int n2 = lpc_recv_line(line, sizeof(line), 1000);
    if (n2 < 0) { uart_cli_send("ERROR: timeout reading major\r\n"); lpc_end(); return; }
    int major = atoi(line);

    uart_cli_printf("Bootloader version: %d.%d\r\n", major, minor);
    lpc_end();
}

void lpc_bl_gid(void) {
    // J — read part ID. Response after rc=0: one decimal line.
    lpc_begin();
    int rc = lpc_send_cmd("J");
    if (rc != 0) {
        uart_cli_printf("ERROR: J failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }

    char line[32];
    int n = lpc_recv_line(line, sizeof(line), 1000);
    if (n < 0) { uart_cli_send("ERROR: timeout reading part ID\r\n"); lpc_end(); return; }

    uint32_t pid = (uint32_t)strtoul(line, NULL, 10);
    // LPC23xx/24xx Part IDs from NXP UM10237 Table 624 plus newer-silicon variants.
    // Multiple parts in the same family often share an ID — only the family is
    // certain from this value; pin count / flash size require the datasheet.
    const char *name = "unknown (check UM10237 Table 624)";
    switch (pid) {
        // LPC236x/237x family
        case 0x1600F923u: name = "LPC2364/2365/2366/2367/2368/2377/2378 (rev A)"; break;
        case 0x1600E823u: name = "LPC2364/2365/2366 (HBD package)"; break;
        // ES_LPC2468 errata: real LPC2468 silicon returns 0x1600FF35 instead of
        // the documented 0x1900FF35 from UM10237 Table 624. Also matches some
        // later-revision LPC2368/2378 silicon, so the call is ambiguous from
        // the ISP J response alone — trust the chip marking.
        case 0x1600FF35u: name = "LPC2468 (ES_LPC2468 errata) — or LPC2368/2378 late-rev silicon"; break;
        // LPC238x family
        case 0x1700FF35u: name = "LPC2387"; break;
        case 0x1800F935u: name = "LPC2388"; break;
        // LPC24xx family
        case 0x1A07FF35u: name = "LPC2458"; break;
        case 0x1A02FF35u: name = "LPC2460 / LPC2478"; break;
        case 0x1900FF35u: name = "LPC2468"; break;
        case 0x1F00FF35u: name = "LPC2470"; break;
        default:          break;
    }
    uart_cli_printf("Part ID: %lu (0x%08lX) — %s\r\n", pid, pid, name);
    lpc_end();
}

// Read N bytes via the R command. Buffers one ISP group (≤900 bytes) at a
// time, then emits the hex dump for that group AFTER the checksum validates
// but BEFORE acking with OK — the LPC is idle in that window, so the slow
// USB-CDC hex output can't drop bytes from the LPC's 11.5 KB/s UART stream.
void lpc_bl_read(uint32_t addr, uint32_t count) {
    if (count == 0) count = 256;

    uart_cli_printf("Reading %lu bytes from 0x%08lX:\r\n", count, addr);

    lpc_begin();
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "R %lu %lu", (unsigned long)addr, (unsigned long)count);
    int rc = lpc_send_cmd(cmd);
    if (rc != 0) {
        uart_cli_printf("ERROR: R failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }

    uint32_t total_decoded = 0;
    uint32_t group_sum = 0;
    int group_lines = 0;     // UU lines in current 20-line group
    char line[80];
    uint8_t dec_line[64];
    // Per-group buffer: up to 20 UU lines × 45 bytes = 900 bytes. Plus a
    // 15-byte carry from the previous group so 16-byte hex lines stay aligned.
    uint8_t group_buf[900 + 16];
    uint32_t group_len = 0;
    // Carry across groups for 16-byte alignment of the hex dump.
    uint8_t carry[16];
    uint32_t carry_len = 0;
    uint32_t print_addr = addr;

    while (total_decoded < count) {
        int n = lpc_recv_line(line, sizeof(line), 3000);
        if (n < 0) {
            uart_cli_send("ERROR: timeout reading data line\r\n");
            lpc_end();
            return;
        }
        if (n == 0) continue;  // skip blanks

        int dec = lpc_uu_decode(line, dec_line, sizeof(dec_line));
        if (dec < 0) {
            uart_cli_printf("ERROR: malformed UU line: '%s'\r\n", line);
            lpc_end();
            return;
        }
        if (total_decoded + (uint32_t)dec > count) dec = (int)(count - total_decoded);
        for (int i = 0; i < dec; i++) group_sum += dec_line[i];
        total_decoded += (uint32_t)dec;
        group_lines++;

        // Accumulate raw bytes into the group buffer — no slow output yet.
        memcpy(group_buf + group_len, dec_line, (size_t)dec);
        group_len += (uint32_t)dec;

        // After 20 lines, or after the final line, target sends a checksum line
        // and waits for OK\r\n / RESEND\r\n.
        bool end_of_data = (total_decoded >= count);
        if (group_lines == 20 || end_of_data) {
            int csn = lpc_recv_line(line, sizeof(line), 2000);
            if (csn < 0) {
                uart_cli_send("ERROR: timeout reading checksum\r\n");
                lpc_end();
                return;
            }
            uint32_t target_sum = (uint32_t)strtoul(line, NULL, 10);
            if (target_sum != group_sum) {
                uart_cli_printf("ERROR: checksum mismatch (target=%lu, ours=%lu)\r\n",
                                target_sum, group_sum);
                lpc_send_str("RESEND\r\n");
                lpc_end();
                return;
            }

            // EMIT the group's hex dump now — LPC is idle waiting for our ack.
            // Combine leftover carry + group_buf for 16-byte aligned output.
            uint32_t emit_pos = 0;
            uint8_t emit_buf[32];  // up to 15 (carry) + 16 (next line) = enough scratch
            // Drain carry + group_buf into 16-byte hex lines
            if (carry_len > 0) {
                memcpy(emit_buf, carry, carry_len);
                uint32_t need = 16 - carry_len;
                uint32_t take = (group_len < need) ? group_len : need;
                memcpy(emit_buf + carry_len, group_buf, take);
                if (carry_len + take == 16) {
                    lpc_dump_hex_line(print_addr, emit_buf, 16);
                    print_addr += 16;
                    emit_pos = take;
                    carry_len = 0;
                } else {
                    // group too small to complete the carry; tack onto carry and stop
                    carry_len += take;
                    memcpy(carry, emit_buf, carry_len);
                    emit_pos = take;
                }
            }
            // Now emit 16-byte chunks directly from group_buf[emit_pos..]
            while (emit_pos + 16 <= group_len) {
                lpc_dump_hex_line(print_addr, group_buf + emit_pos, 16);
                print_addr += 16;
                emit_pos += 16;
            }
            // Save remainder into carry for next group (or final partial line)
            uint32_t remainder = group_len - emit_pos;
            if (remainder > 0) {
                memcpy(carry, group_buf + emit_pos, remainder);
                carry_len = remainder;
            }

            // Ack the group only after we've drained the slow output.
            lpc_send_str("OK\r\n");
            if (!end_of_data) {
                // Consume the echo of our OK before reading the next data line.
                (void)lpc_recv_line(line, sizeof(line), 2000);
            }
            group_sum = 0;
            group_lines = 0;
            group_len = 0;
        }
    }

    // Final partial line (≤15 bytes carry left after last group)
    if (carry_len > 0) {
        lpc_dump_hex_line(print_addr, carry, carry_len);
    }

    uart_cli_printf("OK: Read %lu bytes\r\n", total_decoded);
    lpc_end();
}

void lpc_bl_go(uint32_t addr, bool thumb) {
    // G <addr> <T|A>. Note: after rc=0 the bootloader exits and runs user code,
    // so the response stream may end abruptly.
    lpc_begin();
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "G %lu %c", (unsigned long)addr, thumb ? 'T' : 'A');
    int rc = lpc_send_cmd(cmd);
    if (rc != 0) {
        uart_cli_printf("ERROR: G failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }
    uart_cli_printf("OK: Jumping to 0x%08lX (%s mode) — bootloader exited\r\n",
                    (unsigned long)addr, thumb ? "Thumb" : "ARM");
    lpc_end();
}

void lpc_bl_unlock(void) {
    // U 23130 — unlock destructive commands (E, W, C, G).
    lpc_begin();
    int rc = lpc_send_cmd("U 23130");
    if (rc != 0) {
        uart_cli_printf("ERROR: U failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }
    uart_cli_send("OK: Bootloader unlocked\r\n");
    lpc_end();
}

void lpc_bl_crp(void) {
    // Read the CRP word and interpret it. If the read itself is refused with
    // rc=19 then CRP is already enforcing protection.
    const lpc_target_info_t *info = lpc_get_target_info();
    if (!info->has_crp) {
        uart_cli_send("CRP: this family has no fixed CRP word (CMPA / PUF based)\r\n");
        return;
    }
    uart_cli_printf("Reading CRP word at 0x%08lX (%s)...\r\n",
                    (unsigned long)info->crp_word_addr, info->name);

    lpc_begin();
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "R %lu 4", (unsigned long)info->crp_word_addr);
    int rc = lpc_send_cmd(cmd);
    if (rc == 19) {
        uart_cli_send("CRP: ENABLED (read blocked, rc=19 CODE_READ_PROTECTION_ENABLED)\r\n");
        lpc_end();
        return;
    }
    if (rc != 0) {
        uart_cli_printf("ERROR: R failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }

    // One UU line + checksum
    char line[80];
    int n = lpc_recv_line(line, sizeof(line), 2000);
    if (n < 0) { uart_cli_send("ERROR: timeout reading CRP\r\n"); lpc_end(); return; }
    uint8_t dec[8];
    int dec_n = lpc_uu_decode(line, dec, sizeof(dec));
    if (dec_n < 4) { uart_cli_send("ERROR: bad UU on CRP read\r\n"); lpc_end(); return; }

    // Consume checksum + ack it
    int csn = lpc_recv_line(line, sizeof(line), 2000);
    if (csn >= 0) lpc_send_str("OK\r\n");

    // Little-endian 32-bit word
    uint32_t crp = (uint32_t)dec[0] | ((uint32_t)dec[1] << 8)
                 | ((uint32_t)dec[2] << 16) | ((uint32_t)dec[3] << 24);

    uart_cli_printf("CRP word (0x%08lX) = 0x%08lX\r\n",
                    (unsigned long)info->crp_word_addr, (unsigned long)crp);
    // LPC2468 CRP levels (UM10237 §31.5):
    //   blank/other → NO_ISP_CRP   (full debug+ISP access)
    //   0x12345678  → CRP1         (JTAG/SWD reads of sectors 0..7 blocked,
    //                               ISP allowed except mass erase / write to
    //                               sector 0)
    //   0x87654321  → CRP2         (most ISP commands disabled, JTAG disabled,
    //                               only mass erase via ISP still allowed)
    //   0x43218765  → CRP3         (ISP entry on reset disabled — must rely
    //                               on user-code triggered reinvoke, otherwise
    //                               the part is permanently locked)
    switch (crp) {
        case 0xFFFFFFFFu:
            uart_cli_send("CRP: NONE (blank flash word — full debug + ISP access)\r\n");
            break;
        case 0x12345678u:
            uart_cli_send("CRP: CRP1 (JTAG/SWD read of sectors 0..7 blocked; ISP read+limited write allowed)\r\n");
            break;
        case 0x87654321u:
            uart_cli_send("CRP: CRP2 (JTAG disabled; only mass-erase ISP command allowed)\r\n");
            break;
        case 0x43218765u:
            uart_cli_send("CRP: CRP3 (ISP-on-reset disabled — part is permanently locked unless user code re-invokes ISP)\r\n");
            break;
        default:
            uart_cli_send("CRP: NONE (non-magic value — treated as no CRP)\r\n");
            break;
    }
    lpc_end();
}

void lpc_bl_crp_info(void) {
    const lpc_target_info_t *info = lpc_get_target_info();
    uart_cli_printf("CRP levels (%s):\r\n", info->name);
    if (!info->has_crp) {
        uart_cli_send("  This family has no fixed CRP word — it uses CMPA / PUF\r\n");
        uart_cli_send("  based secure boot. See the family-specific manual.\r\n");
        return;
    }
    uart_cli_printf("  Word at 0x%08lX selects the protection level.\r\n",
                    (unsigned long)info->crp_word_addr);
    uart_cli_send("\r\n");
    uart_cli_send("  NONE  - blank (0xFFFFFFFF) or any non-magic value\r\n");
    uart_cli_send("          Full debug + ISP access.\r\n");
    uart_cli_send("  CRP1  - magic 0x12345678\r\n");
    uart_cli_send("          JTAG/SWD read of sectors 0..7 blocked.\r\n");
    uart_cli_send("          ISP read + limited write still allowed.\r\n");
    uart_cli_send("  CRP2  - magic 0x87654321\r\n");
    uart_cli_send("          JTAG/SWD disabled. Only the mass-erase ISP\r\n");
    uart_cli_send("          command is accepted; ISP reads blocked.\r\n");
    uart_cli_send("  CRP3  - magic 0x43218765\r\n");
    uart_cli_send("          ISP-on-reset entry disabled. Chip is\r\n");
    uart_cli_send("          permanently locked unless user code itself\r\n");
    uart_cli_send("          re-invokes the bootloader on demand.\r\n");
    uart_cli_send("\r\n");
    uart_cli_send("Setting CRP requires re-programming sector 0:\r\n");
    uart_cli_send("  1. TARGET BL UNLOCK\r\n");
    uart_cli_send("  2. TARGET BL ERASE 0           (when phase 2 ships)\r\n");
    uart_cli_printf("  3. TARGET BL WRITE 0x%08lX <hex> LOCK-CRP\r\n",
                    (unsigned long)info->crp_word_addr);
    uart_cli_printf("Note: WRITE refuses to touch 0x%08lX without LOCK-CRP.\r\n",
                    (unsigned long)info->crp_word_addr);
}

void lpc_bl_crp_check(uint32_t value) {
    uart_cli_printf("If CRP word (0x1FC) were set to 0x%08lX:\r\n", (unsigned long)value);
    switch (value) {
        case 0xFFFFFFFFu:
            uart_cli_send("  CRP: NONE (blank flash — full debug + ISP access)\r\n");
            break;
        case 0x12345678u:
            uart_cli_send("  CRP: CRP1 (JTAG/SWD read of sectors 0..7 blocked; ISP read+limited write allowed)\r\n");
            break;
        case 0x87654321u:
            uart_cli_send("  CRP: CRP2 (JTAG disabled; only mass-erase ISP command allowed)\r\n");
            break;
        case 0x43218765u:
            uart_cli_send("  CRP: CRP3 (ISP-on-reset disabled — chip permanently locked)\r\n");
            break;
        default:
            uart_cli_send("  CRP: NONE (non-magic value — treated as no CRP)\r\n");
            break;
    }
}

// ----- Phase 2: destructive operations -----
//
// All of E / W / C require the bootloader to be UNLOCKED first
// (U 23130). The user-facing dispatcher does NOT auto-unlock — the
// rc=15 (CMD_LOCKED) hint is enough.

// --- internal helpers --------------------------------------------------

// Send "P start end" and validate rc=0.
static int lpc_isp_prepare(int start, int end) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "P %d %d", start, end);
    return lpc_send_cmd(cmd);
}

// LPC-variant UU encoder. Encodes up to 45 input bytes into a UU line
// (length byte + 4 chars per 3-byte group). Returns number of chars
// written (not counting the trailing NUL).
static int lpc_uu_encode_line(const uint8_t *in, int n, char *out) {
    if (n < 0 || n > 45) return -1;
    out[0] = (n == 0) ? '`' : (char)(n + 0x20);
    int pos = 1;
    for (int i = 0; i < n; i += 3) {
        uint8_t b0 = in[i];
        uint8_t b1 = (i + 1 < n) ? in[i + 1] : 0;
        uint8_t b2 = (i + 2 < n) ? in[i + 2] : 0;
        int v0 = (b0 >> 2) & 0x3F;
        int v1 = ((b0 & 0x3) << 4) | ((b1 >> 4) & 0xF);
        int v2 = ((b1 & 0xF) << 2) | ((b2 >> 6) & 0x3);
        int v3 = b2 & 0x3F;
        out[pos++] = (v0 == 0) ? '`' : (char)(v0 + 0x20);
        out[pos++] = (v1 == 0) ? '`' : (char)(v1 + 0x20);
        out[pos++] = (v2 == 0) ? '`' : (char)(v2 + 0x20);
        out[pos++] = (v3 == 0) ? '`' : (char)(v3 + 0x20);
    }
    out[pos] = '\0';
    return pos;
}

// Send a UU-encoded buffer to RAM via the W command, with the LPC's
// 20-line group + checksum + OK/RESEND handshake.
//
// Caller must have already validated that target is unlocked and the
// RAM region is writable. Echo of each line is consumed.
//
// Returns 0 on success, negative LPC_RC_* / ISP rc on failure.
static int lpc_isp_write_to_ram(uint32_t ram_addr, const uint8_t *data, uint32_t len) {
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "W %lu %lu", (unsigned long)ram_addr, (unsigned long)len);
    int rc = lpc_send_cmd(cmd);
    if (rc != 0) return rc;

    char line_buf[80];
    char ack_buf[96];   // big enough to swallow the echo of a 61-char UU line
    uint32_t sent = 0;
    uint32_t group_sum = 0;
    int group_lines = 0;

    while (sent < len) {
        uint32_t remaining = len - sent;
        int line_bytes = (remaining >= 45) ? 45 : (int)remaining;

        lpc_uu_encode_line(data + sent, line_bytes, line_buf);

        for (int i = 0; i < line_bytes; i++)
            group_sum += data[sent + i];

        // Send the encoded line + \r\n
        lpc_send_str(line_buf);
        uart_putc_raw(TARGET_UART_ID, '\r');
        uart_putc_raw(TARGET_UART_ID, '\n');
        uart_tx_wait_blocking(TARGET_UART_ID);

        // Drain the echo of our line. If the LPC has echo disabled in
        // W-mode, this just times out quickly; either way we continue.
        (void)lpc_recv_line(ack_buf, sizeof(ack_buf), 500);

        sent += (uint32_t)line_bytes;
        group_lines++;

        bool end_of_data = (sent >= len);
        if (group_lines == 20 || end_of_data) {
            // Send checksum line
            char chk_line[16];
            snprintf(chk_line, sizeof(chk_line), "%lu", (unsigned long)group_sum);
            lpc_send_str(chk_line);
            uart_putc_raw(TARGET_UART_ID, '\r');
            uart_putc_raw(TARGET_UART_ID, '\n');
            uart_tx_wait_blocking(TARGET_UART_ID);

            // Drain the echo of the checksum line (if any).
            (void)lpc_recv_line(ack_buf, sizeof(ack_buf), 500);

            // Read OK or RESEND. LPC may emit a leading blank line first.
            int n = lpc_recv_line(ack_buf, sizeof(ack_buf), 5000);
            int skip = 0;
            while (n == 0 && skip++ < 3)
                n = lpc_recv_line(ack_buf, sizeof(ack_buf), 5000);
            if (n < 0) return LPC_RC_TIMEOUT_DATA;
            if (strncmp(ack_buf, "OK", 2) != 0) {
                if (strncmp(ack_buf, "RESEND", 6) == 0)
                    return LPC_RC_BAD_CHECKSUM;
                return LPC_RC_BAD_CHECKSUM;
            }

            group_sum = 0;
            group_lines = 0;
        }
    }
    return 0;
}

// --- public phase 2 commands ------------------------------------------

void lpc_bl_erase(int sector_start, int sector_end) {
    const lpc_target_info_t *info = lpc_get_target_info();
    if (!info->sectors || info->num_sectors == 0) {
        uart_cli_send("ERROR: no sector map for current LPC family\r\n");
        return;
    }

    // Mass erase: (-1, -1) collapses to "all user-accessible sectors"
    bool mass = (sector_start < 0 || sector_end < 0);
    if (mass) {
        sector_start = (int)info->sectors[0].number;
        sector_end   = (int)info->sectors[info->num_sectors - 1].number;
    }
    if (sector_start > sector_end) {
        uart_cli_send("ERROR: sector_start > sector_end\r\n");
        return;
    }

    uart_cli_printf("Erasing sectors %d..%d%s ...\r\n",
                    sector_start, sector_end, mass ? " (mass)" : "");

    lpc_begin();
    int rc = lpc_isp_prepare(sector_start, sector_end);
    if (rc != 0) {
        uart_cli_printf("ERROR: P failed: %s (rc=%d) — UNLOCK first?\r\n",
                        lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "E %d %d", sector_start, sector_end);
    rc = lpc_send_cmd(cmd);
    if (rc != 0) {
        uart_cli_printf("ERROR: E failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }
    uart_cli_printf("OK: erased sectors %d..%d\r\n", sector_start, sector_end);
    lpc_end();
}

void lpc_bl_write(uint32_t flash_addr, const uint8_t *data, uint32_t len) {
    // LPC ISP Copy-RAM-to-Flash count must be 256 / 512 / 1024 / 4096.
    if (len != 256 && len != 512 && len != 1024 && len != 4096) {
        uart_cli_printf("ERROR: WRITE length must be 256/512/1024/4096 (got %lu)\r\n",
                        (unsigned long)len);
        return;
    }
    if (flash_addr & (len - 1)) {
        uart_cli_printf("ERROR: flash_addr 0x%08lX not aligned to %lu\r\n",
                        (unsigned long)flash_addr, (unsigned long)len);
        return;
    }
    const lpc_target_info_t *info = lpc_get_target_info();
    int s_start = lpc_sector_for_addr(info, flash_addr);
    int s_end   = lpc_sector_for_addr(info, flash_addr + len - 1);
    if (s_start < 0 || s_end < 0) {
        uart_cli_printf("ERROR: flash 0x%08lX..0x%08lX not in any sector\r\n",
                        (unsigned long)flash_addr,
                        (unsigned long)(flash_addr + len - 1));
        return;
    }

    uart_cli_printf("Writing %lu bytes to flash 0x%08lX via RAM 0x%08lX (sectors %d..%d)\r\n",
                    (unsigned long)len, (unsigned long)flash_addr,
                    (unsigned long)info->ram_staging_addr, s_start, s_end);

    lpc_begin();

    int rc = lpc_isp_write_to_ram(info->ram_staging_addr, data, len);
    if (rc != 0) {
        uart_cli_printf("ERROR: W (to RAM) failed: %s (rc=%d)\r\n",
                        lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }

    rc = lpc_isp_prepare(s_start, s_end);
    if (rc != 0) {
        uart_cli_printf("ERROR: P failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "C %lu %lu %lu",
             (unsigned long)flash_addr,
             (unsigned long)info->ram_staging_addr,
             (unsigned long)len);
    rc = lpc_send_cmd(cmd);
    if (rc != 0) {
        uart_cli_printf("ERROR: C failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        lpc_end();
        return;
    }

    uart_cli_printf("OK: wrote %lu bytes to 0x%08lX\r\n",
                    (unsigned long)len, (unsigned long)flash_addr);
    lpc_end();
}

// --- CRP SET orchestrator ----------------------------------------------
//
// Reads sector 0 into a firmware buffer, patches the family CRP word,
// erases sector 0, writes the modified buffer back. Avoids needing the
// host to send 4 KB of hex over the CLI.

// Read N bytes from `addr` into `out_buf` via the R command, with the
// same UU group + checksum + OK handshake as lpc_bl_read but without
// printing the hex dump. Returns 0 on success.
static int lpc_isp_read_into_buf(uint32_t addr, uint32_t count, uint8_t *out_buf) {
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "R %lu %lu", (unsigned long)addr, (unsigned long)count);
    int rc = lpc_send_cmd(cmd);
    if (rc != 0) return rc;

    uint32_t got = 0;
    uint32_t group_sum = 0;
    int group_lines = 0;
    char line[80];
    uint8_t dec_line[64];

    while (got < count) {
        int n = lpc_recv_line(line, sizeof(line), 3000);
        if (n < 0) return LPC_RC_TIMEOUT_DATA;
        if (n == 0) continue;

        int dec = lpc_uu_decode(line, dec_line, sizeof(dec_line));
        if (dec < 0) return LPC_RC_BAD_UU;
        if (got + (uint32_t)dec > count) dec = (int)(count - got);
        memcpy(out_buf + got, dec_line, (size_t)dec);
        for (int i = 0; i < dec; i++) group_sum += dec_line[i];
        got += (uint32_t)dec;
        group_lines++;

        bool eod = (got >= count);
        if (group_lines == 20 || eod) {
            int csn = lpc_recv_line(line, sizeof(line), 2000);
            if (csn < 0) return LPC_RC_TIMEOUT_DATA;
            uint32_t target_sum = (uint32_t)strtoul(line, NULL, 10);
            if (target_sum != group_sum) {
                lpc_send_str("RESEND\r\n");
                return LPC_RC_BAD_CHECKSUM;
            }
            lpc_send_str("OK\r\n");
            if (!eod)
                (void)lpc_recv_line(line, sizeof(line), 2000);
            group_sum = 0;
            group_lines = 0;
        }
    }
    return 0;
}

void lpc_bl_crp_set(uint8_t level) {
    const lpc_target_info_t *info = lpc_get_target_info();
    if (!info->has_crp) {
        uart_cli_send("ERROR: this LPC family has no fixed CRP word\r\n");
        return;
    }
    if (!info->sectors || info->num_sectors == 0) {
        uart_cli_send("ERROR: no sector map for current LPC family\r\n");
        return;
    }

    uint32_t magic;
    const char *level_name;
    switch (level) {
        case 0: magic = 0xFFFFFFFFu; level_name = "NONE"; break;
        case 1: magic = 0x12345678u; level_name = "CRP1"; break;
        case 2: magic = 0x87654321u; level_name = "CRP2"; break;
        case 3: magic = 0x43218765u; level_name = "CRP3"; break;
        default:
            uart_cli_printf("ERROR: invalid CRP level %u (use 0..3)\r\n", level);
            return;
    }

    // Find sector 0 (the one containing the CRP word).
    int sec = lpc_sector_for_addr(info, info->crp_word_addr);
    if (sec < 0) {
        uart_cli_send("ERROR: CRP word address not in any mapped sector\r\n");
        return;
    }
    const lpc_sector_t *s0 = NULL;
    for (uint32_t i = 0; i < info->num_sectors; i++) {
        if (info->sectors[i].number == (uint32_t)sec) { s0 = &info->sectors[i]; break; }
    }
    if (!s0) { uart_cli_send("ERROR: sector lookup failed\r\n"); return; }

    if (s0->size_bytes != 4096) {
        uart_cli_printf("ERROR: CRP-set assumes a 4 KB CRP sector (this one is %lu bytes)\r\n",
                        (unsigned long)s0->size_bytes);
        return;
    }

    static uint8_t sector_buf[4096];

    // 1. Read existing sector contents
    uart_cli_printf("Reading sector %d (0x%08lX..0x%08lX)...\r\n",
                    sec, (unsigned long)s0->start_addr,
                    (unsigned long)(s0->start_addr + s0->size_bytes - 1));
    lpc_begin();
    int rc = lpc_isp_read_into_buf(s0->start_addr, s0->size_bytes, sector_buf);
    lpc_end();
    if (rc != 0) {
        uart_cli_printf("ERROR: read failed: %s (rc=%d)\r\n", lpc_rc_str(rc), rc);
        return;
    }

    // 2. Patch the CRP word (little-endian)
    uint32_t off = info->crp_word_addr - s0->start_addr;
    sector_buf[off + 0] = (uint8_t)(magic & 0xFF);
    sector_buf[off + 1] = (uint8_t)((magic >> 8) & 0xFF);
    sector_buf[off + 2] = (uint8_t)((magic >> 16) & 0xFF);
    sector_buf[off + 3] = (uint8_t)((magic >> 24) & 0xFF);
    uart_cli_printf("Patched CRP word at 0x%08lX -> 0x%08lX (%s)\r\n",
                    (unsigned long)info->crp_word_addr,
                    (unsigned long)magic, level_name);

    // 3. Auto-unlock (the LOCK-CRP token at the CLI was the user's consent)
    uart_cli_send("Auto-unlocking bootloader...\r\n");
    lpc_bl_unlock();

    // 4. Erase the sector
    lpc_bl_erase(sec, sec);

    // 5. Write the patched buffer back
    lpc_bl_write(s0->start_addr, sector_buf, s0->size_bytes);

    uart_cli_printf("OK: CRP set to %s. Power-cycle the target for it to take effect.\r\n",
                    level_name);
}
