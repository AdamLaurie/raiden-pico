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

static const lpc_target_info_t lpc_arm7_info = {
    .name          = "LPC2xxx (ARM7TDMI-S)",
    .crp_word_addr = 0x000001FCu,
    .crp_word_end  = 0x000001FFu,
    .has_crp       = true,
};

static const lpc_target_info_t lpc_cm_info = {
    .name          = "LPC Cortex-M (17xx/11xx/13xx/18xx/43xx/54xxx)",
    .crp_word_addr = 0x000002FCu,
    .crp_word_end  = 0x000002FFu,
    .has_crp       = true,
};

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

// ----- Phase 2 stubs (destructive operations) -----
// Implementing W+P+C, P+E, etc. needs a RAM staging area, sector tables,
// and --destructive gating. Deferred until we exercise the read-only path
// on hardware.

void lpc_bl_write(uint32_t addr, const uint8_t *data, uint32_t len) {
    (void)addr; (void)data; (void)len;
    uart_cli_send("ERROR: LPC BL WRITE not yet implemented (phase 2)\r\n");
}

void lpc_bl_erase(int sector_start, int sector_end) {
    (void)sector_start; (void)sector_end;
    uart_cli_send("ERROR: LPC BL ERASE not yet implemented (phase 2)\r\n");
}
