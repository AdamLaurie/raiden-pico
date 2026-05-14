#include "command_parser.h"
#include "uart_cli.h"
#include "glitch.h"
#include "target_uart.h"
#include "grbl.h"
#include "swd.h"
#include "jtag.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "pico/bootrom.h"
#include "hardware/regs/sysinfo.h"
#include "hardware/structs/sysinfo.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

#define MAX_CMD_LENGTH 600
#define MAX_ERROR_LENGTH 256

// Command parsing buffer
static char parse_buffer[MAX_CMD_LENGTH];
static char *part_buffers[MAX_CMD_PARTS];

// API mode state
static bool api_mode = false;

// Safe hex/int parsing with overflow detection
// Returns true on success, false on overflow or empty input
static bool parse_u32(const char *str, int base, uint32_t *out) {
    if (!str || !*str) return false;
    char *end;
    unsigned long val = strtoul(str, &end, base);
    if (end == str || *end != '\0') return false;  // no digits or trailing junk
    if (val > 0xFFFFFFFFUL) return false;           // overflow on 64-bit
    // On 32-bit, strtoul wraps — detect by checking digit count for base 16
    if (base == 16 || base == 0) {
        const char *hex = str;
        if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
        size_t ndigits = strlen(hex);
        if (base == 0 && hex != str) ndigits = strlen(hex); // 0x prefix already stripped
        if (ndigits > 8) return false;  // more than 8 hex digits = overflow
    }
    *out = (uint32_t)val;
    return true;
}
static bool command_failed = false;
static char last_error[MAX_ERROR_LENGTH] = {0};

// Helper function to send multi-line response with proper line endings
static void send_multiline_response(const char* response) {
    const char *p = response;
    while (*p) {
        if (*p == '\n') {
            uart_cli_send("\r\n");
        } else if (*p != '\r') {  // Skip \r if present
            char c[2] = {*p, '\0'};
            uart_cli_send(c);
        }
        p++;
    }
}

void command_parser_init(void) {
    // Allocate buffers for command parts (600 bytes to allow 256-byte hex payloads)
    for (int i = 0; i < MAX_CMD_PARTS; i++) {
        part_buffers[i] = malloc(600);
    }
}

bool command_parser_parse(const char *cmd, cmd_parts_t *parts) {
    if (!cmd || !parts) {
        return false;
    }

    // Copy command to parse buffer
    strncpy(parse_buffer, cmd, MAX_CMD_LENGTH - 1);
    parse_buffer[MAX_CMD_LENGTH - 1] = '\0';

    // Initialize parts
    parts->count = 0;

    // Tokenize by spaces
    char *token = strtok(parse_buffer, " \t");
    while (token != NULL && parts->count < MAX_CMD_PARTS) {
        // Convert to uppercase
        for (char *p = token; *p; p++) {
            *p = toupper(*p);
        }

        // Store in part buffer (599 chars max to leave room for null)
        strncpy(part_buffers[parts->count], token, 599);
        part_buffers[parts->count][599] = '\0';
        parts->parts[parts->count] = part_buffers[parts->count];
        parts->count++;

        token = strtok(NULL, " \t");
    }

    return parts->count > 0;
}

const char* command_parser_match(const char *abbrev, const char **candidates, uint8_t count) {
    if (!abbrev || !candidates) {
        return NULL;
    }

    const char *match = NULL;
    uint8_t match_count = 0;
    size_t abbrev_len = strlen(abbrev);

    // Check for exact match first
    for (uint8_t i = 0; i < count; i++) {
        if (strcmp(abbrev, candidates[i]) == 0) {
            return candidates[i];
        }
    }

    // Find all candidates that start with abbrev
    for (uint8_t i = 0; i < count; i++) {
        if (strncmp(abbrev, candidates[i], abbrev_len) == 0) {
            match = candidates[i];
            match_count++;
        }
    }

    // Return match only if unambiguous
    if (match_count == 1) {
        return match;
    } else if (match_count > 1) {
        return NULL;  // Ambiguous
    } else {
        return abbrev;  // No match, return original
    }
}

// API-aware error reporting
static void api_error(const char *msg) {
    command_failed = true;
    strncpy(last_error, msg, MAX_ERROR_LENGTH - 1);
    last_error[MAX_ERROR_LENGTH - 1] = '\0';
    if (!api_mode) {
        uart_cli_send(msg);
    }
}

static void api_error_printf(const char *format, ...) {
    command_failed = true;
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, MAX_ERROR_LENGTH, format, args);
    va_end(args);
    if (!api_mode) {
        uart_cli_send(last_error);
    }
}

// Helper function to match and replace a part
static bool match_and_replace(char **part, const char **candidates, uint8_t count, const char *context) {
    const char *matched = command_parser_match(*part, candidates, count);
    if (matched == NULL) {
        api_error_printf("ERROR: Ambiguous %s '%s' - be more specific\r\n", context, *part);
        return false;
    }
    *part = (char*)matched;
    return true;
}

// Auto-detect STM32 family from DEV_ID via SWD
// Requires SWD already connected. Sets TARGET if successful.
static bool swd_auto_detect_target(void) {
    extern void target_set_type(target_type_t type);
    uint32_t cpuid, dbg_id;
    if (!swd_detect(&cpuid, &dbg_id)) return false;
    uint16_t dev_id = dbg_id & 0xFFF;
    target_type_t tt = TARGET_NONE;
    switch (dev_id) {
        case 0x410: case 0x412: case 0x414: case 0x430:
            tt = TARGET_STM32F1; break;
        case 0x438:
            tt = TARGET_STM32F3; break;
        case 0x413: case 0x419: case 0x421: case 0x423: case 0x433:
            tt = TARGET_STM32F4; break;
        case 0x415: case 0x435: case 0x462: case 0x464: case 0x461:
            tt = TARGET_STM32L4; break;
    }
    if (tt != TARGET_NONE) {
        target_set_type(tt);
        return true;
    }
    return false;
}

// Check if name is a memory region alias (FLASH/SRAM/BOOTROM).
// Does NOT resolve — just checks the string.
static bool is_mem_alias(const char *name) {
    return strcmp(name, "FLASH") == 0 ||
           strcmp(name, "SRAM") == 0 ||
           strcmp(name, "BOOTROM") == 0;
}

// Resolve FLASH/SRAM/BOOTROM alias to base address and size.
// Auto-detects target if needed. Caller must ensure SWD is connected.
// Returns true on success. Caller should have checked is_mem_alias() first.
// size_out may be NULL if not needed.
static bool resolve_mem_alias(const char *name, uint32_t *addr, uint32_t *size_out) {
    extern target_type_t target_get_type(void);
    target_type_t tt = target_get_type();
    if (!target_is_stm32(tt)) {
        if (!swd_auto_detect_target()) {
            uart_cli_send("ERROR: Could not auto-detect STM32 family. Set TARGET manually.\r\n");
            return false;
        }
        tt = target_get_type();
    }
    const stm32_target_info_t *info = stm32_get_target_info(tt);
    if (strcmp(name, "FLASH") == 0) {
        *addr = 0x08000000;
        if (size_out) *size_out = info->flash_size;
    } else if (strcmp(name, "SRAM") == 0) {
        *addr = info->sram_base;
        if (size_out) *size_out = info->sram_size;
    } else {
        *addr = info->bootrom_base;
        if (size_out) *size_out = info->bootrom_size;
    }
    return true;
}

void command_parser_execute(cmd_parts_t *parts) {
    if (!parts || parts->count == 0) {
        return;
    }

    // Initialize API state for this command
    command_failed = false;

    // Send '.' in API mode to acknowledge command received
    if (api_mode) {
        uart_cli_send(".");
    }

    // Match primary command
    const char *primary_commands[] = {
        "SET", "GET", "TRIGGER", "PINS",
        "STATUS", "RESET", "PLATFORM", "CS", "TARGET", "ARM", "GLITCH",
        "HELP", "REBOOT", "DEBUG", "API", "ERROR", "SWD", "JTAG",
        "TRACE", "VERSION", "CLOCK", "GRBL"
    };
    if (!match_and_replace(&parts->parts[0], primary_commands, 22, "command")) {
        goto api_response;
    }

    // Match sub-commands if present
    if (parts->count >= 2) {
        if (strcmp(parts->parts[0], "CS") == 0) {
            const char *chipshot_subcmds[] = {"ARM", "DISARM", "FIRE", "STATUS", "VOLTAGE", "PULSE", "RESET", "TRIGGER", "HVOUT", "FAULTS"};
            if (!match_and_replace(&parts->parts[1], chipshot_subcmds, 10, "CS sub-command")) {
                goto api_response;
            }
        } else if (strcmp(parts->parts[0], "TARGET") == 0) {
            const char *target_subcmds[] = {"LPC", "STM32F1", "STM32F3", "STM32F4", "STM32L4",
                                              "BOOT0", "BOOT1", "BOOTLOADER", "SYNC", "SEND", "RESPONSE", "RESET", "TIMEOUT", "POWER", "GLITCH", "BL"};
            if (!match_and_replace(&parts->parts[1], target_subcmds, 16, "TARGET sub-command")) {
                goto api_response;
            }
        } else if (strcmp(parts->parts[0], "SWD") == 0) {
            const char *swd_subcmds[] = {"CONNECT", "CONNECTRST", "READ", "WRITE", "FILL", "IDCODE",
                                          "HALT", "RESUME", "REGS", "SETREG", "RDP", "OPT", "FLASH", "RESET", "BPTEST", "SPEED"};
            if (!match_and_replace(&parts->parts[1], swd_subcmds, 16, "SWD sub-command")) {
                goto api_response;
            }
        } else if (strcmp(parts->parts[0], "JTAG") == 0) {
            const char *jtag_subcmds[] = {"RESET", "TEST", "IDCODE", "SCAN", "IR", "DR"};
            if (!match_and_replace(&parts->parts[1], jtag_subcmds, 6, "JTAG sub-command")) {
                goto api_response;
            }
        } else if (strcmp(parts->parts[0], "TRACE") == 0 && parts->count >= 2) {
            const char *trace_subcmds[] = {"START", "RESET", "STATUS", "DUMP", "ARM", "RATE"};
            match_and_replace(&parts->parts[1], trace_subcmds, 6, NULL);
        }
    }

    // Match arguments
    if (strcmp(parts->parts[0], "TRIGGER") == 0 && parts->count == 2) {
        const char *trigger_types[] = {"NONE", "UART", "GPIO"};
        if (!match_and_replace(&parts->parts[1], trigger_types, 3, "trigger type")) {
            goto api_response;
        }
    } else if (strcmp(parts->parts[0], "PLATFORM") == 0 && strcmp(parts->parts[1], "SET") == 0 && parts->count == 3) {
        const char *platform_types[] = {"MANUAL", "CHIPSHOUTER", "EMFI", "CROWBAR"};
        if (!match_and_replace(&parts->parts[2], platform_types, 4, "platform type")) {
            goto api_response;
        }
    } else if (strcmp(parts->parts[0], "ARM") == 0 && parts->count == 2) {
        const char *arm_states[] = {"ON", "OFF", "TRACE"};
        if (!match_and_replace(&parts->parts[1], arm_states, 3, "ARM state")) {
            goto api_response;
        }
    } else if (strcmp(parts->parts[0], "SET") == 0 && parts->count >= 2) {
        const char *variables[] = {"PAUSE", "COUNT", "WIDTH", "GAP", "BREAKPOINT", "BP"};
        if (!match_and_replace(&parts->parts[1], variables, 6, "variable name")) {
            goto api_response;
        }
    } else if (strcmp(parts->parts[0], "GET") == 0 && parts->count == 2) {
        const char *variables[] = {"PAUSE", "COUNT", "WIDTH", "GAP"};
        if (!match_and_replace(&parts->parts[1], variables, 4, "variable name")) {
            goto api_response;
        }
    }

    // Execute commands
    if (strcmp(parts->parts[0], "HELP") == 0) {
        uart_cli_send("=== Raiden Pico Command Reference ===\r\n\r\n");
        uart_cli_send("== API Mode (for scripting) ==\r\n");
        uart_cli_send("API [ON|OFF]           - Enable/disable/show API mode (minimal output)\r\n");
        uart_cli_send("ERROR                  - Get last error message\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("API Mode Response Format:\r\n");
        uart_cli_send("  .  = Command received\r\n");
        uart_cli_send("  +  = Command succeeded\r\n");
        uart_cli_send("  !  = Command failed (use ERROR to get details)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== ChipSHOUTER Control ==\r\n");
        uart_cli_send("CS ARM                    - Arm ChipSHOUTER\r\n");
        uart_cli_send("CS DISARM                 - Disarm ChipSHOUTER\r\n");
        uart_cli_send("CS FIRE                   - Trigger ChipSHOUTER\r\n");
        uart_cli_send("CS PULSE [<ns>]           - Set/get ChipSHOUTER pulse width\r\n");
        uart_cli_send("CS RESET                  - Reset ChipSHOUTER and verify errors cleared\r\n");
        uart_cli_send("CS STATUS                 - Get ChipSHOUTER status\r\n");
        uart_cli_send("CS TRIGGER HW <HIGH|LOW>  - Set HW trigger (active high/low w/ pull)\r\n");
        uart_cli_send("CS TRIGGER SW             - Set SW trigger (fires via interrupt)\r\n");
        uart_cli_send("CS VOLTAGE [<V>]          - Set/get ChipSHOUTER voltage\r\n");
        uart_cli_send("CS HVOUT                  - Query HV output status\r\n");
        uart_cli_send("CS FAULTS                 - Get ChipSHOUTER fault status\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Clock Generator ==\r\n");
        uart_cli_send("CLOCK [<freq>] [ON|OFF]   - Set/get clock frequency and enable/disable\r\n");
        uart_cli_send("                            Examples: CLOCK 12000000 ON, CLOCK ON, CLOCK OFF\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Command Shortcuts ==\r\n");
        uart_cli_send("Non-ambiguous shortcuts supported for commands, sub-commands, and arguments.\r\n");
        uart_cli_send("Examples:\r\n");
        uart_cli_send("  STAT → STATUS       GL → GLITCH         P → PINS\r\n");
        uart_cli_send("  TARG B → TARGET BOOTLOADER\r\n");
        uart_cli_send("  SET P 1000 → SET PAUSE 1000             TRIG G R → TRIGGER GPIO RISING\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Glitch Configuration ==\r\n");
        uart_cli_send("GET [PAUSE|WIDTH|GAP|COUNT] - Get current glitch parameter(s)\r\n");
        uart_cli_send("SET [PAUSE|WIDTH|GAP|COUNT] [<cycles>] - Set/get glitch parameter(s)\r\n");
        uart_cli_send("  Example: SET WIDTH 150 = 1us (150 cycles × 6.67ns @ 150MHz)\r\n");
        uart_cli_send("SET BP <slot> <name|0xADDR> [HARD|SOFT] - Set FPB breakpoint\r\n");
        uart_cli_send("SET BP LIST / CLEAR [slot|ALL]          - List/clear breakpoints\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Glitch Control ==\r\n");
        uart_cli_send("ARM [ON|OFF|TRACE]     - Arm/disarm/show (TRACE = trigger only, no glitch)\r\n");
        uart_cli_send("DEBUG [ON|OFF]         - Toggle/show target UART debug display\r\n");
        uart_cli_send("GLITCH                 - Execute single glitch\r\n");
        uart_cli_send("PINS                   - Show pin configuration\r\n");
        uart_cli_send("REBOOT [BL]            - Reboot Pico (BL = bootloader mode)\r\n");
        uart_cli_send("RESET                  - Reset system\r\n");
        uart_cli_send("STATUS                 - Show current status\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Target Control ==\r\n");
        uart_cli_send("TARGET <LPC|STM32F1|STM32F3|STM32F4|STM32L4> - Set target type\r\n");
        uart_cli_send("TARGET BOOT0 [HIGH|LOW]  - Set BOOT0 pin (GP13)\r\n");
        uart_cli_send("TARGET BOOT1 [HIGH|LOW]  - Set BOOT1 pin (GP14)\r\n");
        uart_cli_send("TARGET BOOTLOADER [baud] [crystal_khz] - Enter bootloader\r\n");
        uart_cli_send("                   (defaults: 115200 baud, 12000 kHz crystal)\r\n");
        uart_cli_send("TARGET POWER [ON|OFF|CYCLE [ms]]  - Control target power (GP10/11/12)\r\n");
        uart_cli_send("TARGET GLITCH TEST <V> [count]  - Basic power glitch test\r\n");
        uart_cli_send("TARGET GLITCH SWEEP              - Voltage sweep (SRAM retention, ADC on GP26)\r\n");
        uart_cli_send("TARGET GLITCH PAYLOAD [V] [n]    - Glitch with SRAM payload\r\n");
        uart_cli_send("TARGET GLITCH BYPASS [attempts] [bytes] - RDP1 bypass + flash dump [STM32F1]\r\n");
        uart_cli_send("TARGET GLITCH HALT [bytes]       - RDP1 flash dump via SWD+FPB (no glitch)\r\n");
        uart_cli_send("TARGET GLITCH LITERAL             - Literal payload test\r\n");
        uart_cli_send("TARGET GLITCH REGDUMP             - Register dump payload\r\n");
        uart_cli_send("TARGET GLITCH GLITCH_REGDUMP [n]  - Glitch + register dump\r\n");
        uart_cli_send("TARGET GLITCH RESETTEST           - Reset/low-power disruption test\r\n");
        uart_cli_send("TARGET GLITCH TIMING [name|0xADDR] [samples] [FLASH|BOOTLOADER]\r\n");
        uart_cli_send("                                   - DWT cycle count + ADC shunt timing\r\n");
        uart_cli_send("TARGET RESET [PERIOD <ms>] [PIN <n>] [HIGH] - Reset target\r\n");
        uart_cli_send("                   (defaults: 300ms, GP15, active low)\r\n");
        uart_cli_send("TARGET RESPONSE        - Show response from target\r\n");
        uart_cli_send("TARGET SEND <hex|\"text\"> - Send hex bytes or quoted text (appends \\r)\r\n");
        uart_cli_send("                   Examples: 3F, 68656C6C6F, \"hello\"\r\n");
        uart_cli_send("TARGET SYNC [baud] [crystal_khz] [reset_delay_ms] [retries] - Reset + bootloader\r\n");
        uart_cli_send("                   (defaults: 115200, 12000, 500ms, 5 retries)\r\n");
        uart_cli_send("TARGET TIMEOUT [<ms>]  - Get/set transparent bridge timeout (default: 50ms)\r\n");
        uart_cli_send("TARGET BL GET          - Bootloader version + supported commands\r\n");
        uart_cli_send("TARGET BL GV           - Get version + option bytes\r\n");
        uart_cli_send("TARGET BL GID          - Chip product ID\r\n");
        uart_cli_send("TARGET BL READ <addr> [count] - Read memory (default 256 bytes)\r\n");
        uart_cli_send("TARGET BL WRITE <addr> <hex>  - Write memory (hex byte string)\r\n");
        uart_cli_send("TARGET BL GO <addr>    - Jump to address\r\n");
        uart_cli_send("TARGET BL ERASE <page|ALL WIPE> - Erase flash page or mass erase\r\n");
        uart_cli_send("TARGET BL RU WIPE      - Readout unprotect (mass erase + remove RDP)\r\n");
        uart_cli_send("TARGET BL RP CONFIRM   - Readout protect (set RDP1)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Trigger Configuration ==\r\n");
        uart_cli_send("TRIGGER [NONE|GPIO|UART] - Configure/show trigger\r\n");
        uart_cli_send("TRIGGER GPIO <RISING|FALLING> - GPIO trigger on GP3\r\n");
        uart_cli_send("TRIGGER NONE           - Disable trigger\r\n");
        uart_cli_send("TRIGGER UART <byte> [TX|RX] - UART byte trigger (TX=Raiden TX, RX=Raiden RX)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== ADC Trace (GP27/ADC1 shunt current) ==\r\n");
        uart_cli_send("TRACE                  - Show trace status\r\n");
        uart_cli_send("TRACE [samples] [pre%] - Start ADC trace (~2us/sample, default 4096/50%)\r\n");
        uart_cli_send("TRACE ARM              - Arm trace (trigger only, no glitch)\r\n");
        uart_cli_send("TRACE STATUS           - Check trace state (IDLE/RUNNING/COMPLETE)\r\n");
        uart_cli_send("TRACE DUMP             - Dump raw ADC samples (hex)\r\n");
        uart_cli_send("TRACE RATE <clkdiv>    - Set ADC rate (0=2us, 50=100us, 500=1ms/sample)\r\n");
        uart_cli_send("TRACE RESET            - Discard trace and reset to IDLE\r\n");
        uart_cli_send("  Usage: TRIGGER UART 79 → TRACE 4096 50 → ARM TRACE → (trigger event)\r\n");
        uart_cli_send("         TRACE STATUS → TRACE DUMP → ARM OFF\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== XY Platform (Grbl) ==\r\n");
        uart_cli_send("GRBL SEND <gcode>      - Send raw G-code command\r\n");
        uart_cli_send("GRBL UNLOCK            - Unlock alarm (enable movement without homing)\r\n");
        uart_cli_send("GRBL SET HOME          - Set current position as home (0,0,0)\r\n");
        uart_cli_send("GRBL HOME              - Move to home position (0,0)\r\n");
        uart_cli_send("GRBL AUTOHOME          - Auto-home the XY platform (limit switches)\r\n");
        uart_cli_send("GRBL MOVE <X> <Y> [F]  - Move to absolute position (mm, feedrate mm/min)\r\n");
        uart_cli_send("GRBL STEP <DX> <DY> [F]- Move relative distance (mm, feedrate mm/min)\r\n");
        uart_cli_send("GRBL POS               - Get current XYZ position\r\n");
        uart_cli_send("GRBL RESET             - Soft reset Grbl controller\r\n");
        uart_cli_send("GRBL TEST              - Test UART loopback\r\n");
        uart_cli_send("GRBL DEBUG             - Debug RX FIFO status\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== SWD Debug Interface (GP17=SWCLK, GP18=SWDIO) ==\r\n");
        uart_cli_send("SWD                    - Show full SWD command list\r\n");
        uart_cli_send("SWD CONNECT[RST]       - Connect to target [under reset]\r\n");
        uart_cli_send("SWD FILL <addr> <val> [n] - Fill memory with pattern\r\n");
        uart_cli_send("SWD HALT / RESUME      - Halt / resume target core\r\n");
        uart_cli_send("SWD IDCODE             - Identify chip\r\n");
        uart_cli_send("SWD OPT / RDP          - Read option bytes / RDP level\r\n");
        uart_cli_send("SWD READ <addr> [n]    - Read memory (hex dump)\r\n");
        uart_cli_send("SWD REGS               - Read core registers\r\n");
        uart_cli_send("SWD RESET [ms|HOLD|RELEASE] - Target reset via nRST (default 100ms)\r\n");
        uart_cli_send("SWD SETREG <reg> <val> - Write core register\r\n");
        uart_cli_send("SWD SPEED [us]         - Get/set clock delay (0=max, def 1)\r\n");
        uart_cli_send("SWD WRITE <addr|region> <val> - Write memory (auto-verify)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== JTAG Debug Interface (TCK=17, TMS=18, TDI=19, TDO=20, RTCK=21, TRST=15) ==\r\n");
        uart_cli_send("JTAG RESET             - Reset TAP state machine\r\n");
        uart_cli_send("JTAG IDCODE            - Read device IDCODE\r\n");
        uart_cli_send("JTAG SCAN              - Scan chain for devices\r\n");
        uart_cli_send("JTAG IR <value> [bits] - Shift instruction register (default 4 bits)\r\n");
        uart_cli_send("JTAG DR <value> [bits] - Shift data register (default 32 bits)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== General ==\r\n");
        uart_cli_send("VERSION                - Show firmware version and features\r\n");
        uart_cli_send("STATUS                 - Show current configuration and state\r\n");
        uart_cli_send("REBOOT                 - Reboot Raiden Pico\r\n");
        uart_cli_send("PINS                   - Show GPIO pin assignments\r\n");
        uart_cli_send("\r\n");

    } else if (strcmp(parts->parts[0], "VERSION") == 0) {
        uart_cli_send("Raiden Pico Glitcher v0.4\r\n");
    } else if (strcmp(parts->parts[0], "STATUS") == 0) {
        glitch_config_t *cfg = glitch_get_config();
        system_flags_t *flags = glitch_get_flags();

        // Check chip variant and package
        uint32_t chip_id = sysinfo_hw->chip_id;
        uint32_t revision = sysinfo_hw->gitref_rp2350;
        uint32_t package_sel = sysinfo_hw->package_sel;  // 0=QFN60, 1=QFN80

        // Determine variant and package from PACKAGE_SEL register
        // PACKAGE_SEL: 0 = QFN-60 (RP2350A, 30 GPIOs), 1 = QFN-80 (RP2350B, 48 GPIOs, PSRAM)
        const char *variant;
        const char *package;
        bool has_psram;
        uint32_t num_gpios;

        if (package_sel == 1) {
            variant = "B";
            package = "QFN-80";
            has_psram = true;
            num_gpios = 48;
        } else {
            variant = "A";
            package = "QFN-60";
            has_psram = false;
            num_gpios = 30;
        }

        // Check for WiFi model (compile-time detection)
        #ifdef RASPBERRYPI_PICO2_W
        const char *wifi_str = " W";
        #else
        const char *wifi_str = "";
        #endif

        uart_cli_send("=== System Status ===\r\n\r\n");

        // System info
        uart_cli_send("== System ==\r\n");
        uart_cli_printf("Model:        Raspberry Pi Pico 2%s\r\n", wifi_str);
        uart_cli_printf("Chip:         RP2350%s (%s, %u GPIOs)\r\n", variant, package, num_gpios);
        if (has_psram) {
            uart_cli_printf("Memory:       PSRAM\r\n");
        }
        uart_cli_printf("Chip ID:      0x%08x\r\n", chip_id);
        uart_cli_printf("Revision:     0x%08x\r\n", revision);
        uart_cli_printf("API Mode:     %s\r\n", api_mode ? "ON" : "OFF");
        uart_cli_send("\r\n");

        // Glitch parameters
        uart_cli_send("== Glitch Parameters ==\r\n");
        uint32_t count = glitch_get_count();  // Check count first (updates armed flag if PIO fired)
        uart_cli_printf("Armed:        %s\r\n", flags->armed ? "YES" : "NO");
        uart_cli_printf("Glitch Count: %u\r\n", count);
        uart_cli_printf("Pause:        %u cycles (%.2f us)\r\n", cfg->pause_cycles, cfg->pause_cycles / 150.0f);
        uart_cli_printf("Width:        %u cycles (%.2f us)\r\n", cfg->width_cycles, cfg->width_cycles / 150.0f);
        uart_cli_printf("Gap:          %u cycles (%.2f us)\r\n", cfg->gap_cycles, cfg->gap_cycles / 150.0f);
        uart_cli_printf("Count:        %u\r\n", cfg->count);
        uart_cli_printf("Output Pins:  GP%u (normal), GP%u (inverted)\r\n", PIN_GLITCH_OUT, PIN_GLITCH_OUT_INV);
        uart_cli_send("\r\n");

        // Trigger configuration
        uart_cli_send("== Trigger ==\r\n");
        const char* trigger_str = "NONE";
        if (cfg->trigger == TRIGGER_GPIO) {
            trigger_str = "GPIO";
        } else if (cfg->trigger == TRIGGER_UART) {
            trigger_str = "UART";
        }
        uart_cli_printf("Type:         %s\r\n", trigger_str);
        if (cfg->trigger == TRIGGER_GPIO) {
            uart_cli_printf("Pin:          GP%u\r\n", cfg->trigger_pin);
            uart_cli_printf("Edge:         %s\r\n", cfg->trigger_edge == EDGE_RISING ? "RISING" : "FALLING");
        } else if (cfg->trigger == TRIGGER_UART) {
            uart_cli_printf("Byte:         0x%02X (%u)\r\n", cfg->trigger_byte, cfg->trigger_byte);
            uart_cli_printf("Pin:          GP%u (%s)\r\n", cfg->trigger_uart_pin,
                           cfg->trigger_uart_pin == 4 ? "TX" : "RX");
        }
        uart_cli_send("\r\n");

        // Target configuration
        extern target_type_t target_get_type(void);
        extern bool target_get_debug(void);
        extern uint32_t target_get_timeout(void);
        extern bool target_power_get_state(void);

        uart_cli_send("== Target ==\r\n");
        target_type_t target_type = target_get_type();
        const char* target_type_str = "NONE";
        if (target_type == TARGET_LPC) {
            target_type_str = "LPC";
        } else if (target_is_stm32(target_type)) {
            const stm32_target_info_t *info = stm32_get_target_info(target_type);
            target_type_str = info ? info->name : "STM32";
        }
        uart_cli_printf("Type:         %s\r\n", target_type_str);
        uart_cli_printf("Power (GP10): %s\r\n", target_power_get_state() ? "ON" : "OFF");
        uart_cli_printf("Reset (GP15): HIGH, LOW 300ms pulse\r\n");
        uart_cli_printf("Debug Mode:   %s\r\n", target_get_debug() ? "ON" : "OFF");
        uart_cli_printf("UART Timeout: %u ms\r\n", target_get_timeout());

        uart_cli_send("\r\n");
        uart_cli_send("== Clock Generator ==\r\n");
        uint32_t clock_freq = clock_get_frequency();
        bool clock_enabled = clock_is_enabled();
        if (clock_freq == 0) {
            uart_cli_send("Status:       Not configured\r\n");
        } else {
            uart_cli_printf("Frequency:    %u Hz\r\n", clock_freq);
            uart_cli_printf("Status:       %s\r\n", clock_enabled ? "ON" : "OFF");
            uart_cli_send("Pin:          GP6\r\n");
        }

    } else if (strcmp(parts->parts[0], "SET") == 0) {
        glitch_config_t *cfg = glitch_get_config();

        if (parts->count == 1) {
            // Show all current glitch parameters
            uart_cli_send("Current glitch parameters:\r\n");
            uart_cli_printf("  PAUSE: %u cycles (%.2f us)\r\n", cfg->pause_cycles, cfg->pause_cycles / 150.0f);
            uart_cli_printf("  WIDTH: %u cycles (%.2f us)\r\n", cfg->width_cycles, cfg->width_cycles / 150.0f);
            uart_cli_printf("  GAP:   %u cycles (%.2f us)\r\n", cfg->gap_cycles, cfg->gap_cycles / 150.0f);
            uart_cli_printf("  COUNT: %u\r\n", cfg->count);
        } else if (strcmp(parts->parts[1], "BREAKPOINT") == 0 || strcmp(parts->parts[1], "BP") == 0) {
            // SET BREAKPOINT <slot> <name|0xADDR> [HARD|SOFT]
            // SET BREAKPOINT LIST
            // SET BREAKPOINT CLEAR [slot|ALL]
            #include "stm32_breakpoints.h"
            if (parts->count == 2 || (parts->count == 3 && strcasecmp(parts->parts[2], "LIST") == 0)) {
                swd_bp_list();
            } else if (parts->count >= 3 && strcasecmp(parts->parts[2], "CLEAR") == 0) {
                if (parts->count >= 4 && strcasecmp(parts->parts[3], "ALL") == 0) {
                    swd_bp_clear_all();
                } else if (parts->count >= 4) {
                    uint32_t slot;
                    if (!parse_u32(parts->parts[3], 0, &slot)) {
                        api_error("ERROR: Invalid slot number\r\n");
                        goto api_response;
                    }
                    swd_bp_clear_hard(slot);
                } else {
                    swd_bp_clear_all();
                }
            } else if (parts->count >= 4) {
                uint32_t slot;
                if (!parse_u32(parts->parts[2], 0, &slot)) {
                    api_error("ERROR: Usage: SET BREAKPOINT <slot 0-5> <name|0xADDR> [HARD|SOFT]\r\n");
                    goto api_response;
                }

                const char *bp_arg = parts->parts[3];
                uint32_t addr = 0;
                const char *bp_name = NULL;

                // Resolve name or address
                extern target_type_t target_get_type(void);
                const stm32_bp_table_t *table = stm32_get_breakpoints(target_get_type());
                if (bp_arg[0] == '0' && (bp_arg[1] == 'x' || bp_arg[1] == 'X')) {
                    addr = strtoul(bp_arg, NULL, 16);
                    if (table) {
                        const stm32_bp_entry_t *e = stm32_find_breakpoint(table, bp_arg);
                        if (e) bp_name = e->name;
                    }
                } else {
                    if (!table) {
                        api_error("ERROR: Set TARGET type first for named breakpoints\r\n");
                        goto api_response;
                    }
                    const stm32_bp_entry_t *e = stm32_find_breakpoint(table, bp_arg);
                    if (!e) {
                        api_error_printf("ERROR: Unknown breakpoint '%s'\r\n", bp_arg);
                        goto api_response;
                    }
                    addr = e->addr;
                    bp_name = e->name;
                }

                // Hard or soft?
                bool soft = false;
                if (parts->count >= 5) {
                    if (strcasecmp(parts->parts[4], "SOFT") == 0) soft = true;
                }

                if (soft) {
                    swd_bp_set_soft(addr, bp_name);
                } else {
                    swd_bp_set_hard(slot, addr, bp_name);
                }
            } else {
                uart_cli_send("Usage: SET BREAKPOINT <slot 0-5> <name|0xADDR> [HARD|SOFT]\r\n");
                uart_cli_send("       SET BREAKPOINT LIST\r\n");
                uart_cli_send("       SET BREAKPOINT CLEAR [slot|ALL]\r\n");
            }
        } else if (parts->count != 3) {
            uart_cli_send("ERROR: Usage: SET <PAUSE|WIDTH|GAP|COUNT> <value>\r\n");
            uart_cli_send("       Value is in system clock cycles (150MHz = 6.67ns per cycle)\r\n");
            goto api_response;
        } else {
            uint32_t value = atoi(parts->parts[2]);

            if (strcmp(parts->parts[1], "PAUSE") == 0) {
                glitch_set_pause(value);
                uart_cli_printf("OK: PAUSE set to %u cycles (%.2f us)\r\n", value, value / 150.0f);
            } else if (strcmp(parts->parts[1], "WIDTH") == 0) {
                glitch_set_width(value);
                uart_cli_printf("OK: WIDTH set to %u cycles (%.2f us)\r\n", value, value / 150.0f);
            } else if (strcmp(parts->parts[1], "GAP") == 0) {
                glitch_set_gap(value);
                uart_cli_printf("OK: GAP set to %u cycles (%.2f us)\r\n", value, value / 150.0f);
            } else if (strcmp(parts->parts[1], "COUNT") == 0) {
                glitch_set_count(value);
                uart_cli_printf("OK: COUNT set to %u\r\n", value);
            } else {
                api_error_printf("ERROR: Unknown variable '%s' (use PAUSE/WIDTH/GAP/COUNT)\r\n", parts->parts[1]);
            }
        }

    } else if (strcmp(parts->parts[0], "GET") == 0) {
        glitch_config_t *cfg = glitch_get_config();

        if (parts->count == 1) {
            // Show all glitch parameters
            uart_cli_printf("PAUSE: %u cycles (%.2f us)\r\n", cfg->pause_cycles, cfg->pause_cycles / 150.0f);
            uart_cli_printf("WIDTH: %u cycles (%.2f us)\r\n", cfg->width_cycles, cfg->width_cycles / 150.0f);
            uart_cli_printf("GAP:   %u cycles (%.2f us)\r\n", cfg->gap_cycles, cfg->gap_cycles / 150.0f);
            uart_cli_printf("COUNT: %u\r\n", cfg->count);
        } else if (parts->count != 2) {
            uart_cli_send("ERROR: Usage: GET <PAUSE|WIDTH|GAP|COUNT>\r\n");
            goto api_response;
        } else {
            if (strcmp(parts->parts[1], "PAUSE") == 0) {
                uart_cli_printf("%u cycles (%.2f us)\r\n", cfg->pause_cycles, cfg->pause_cycles / 150.0f);
            } else if (strcmp(parts->parts[1], "WIDTH") == 0) {
                uart_cli_printf("%u cycles (%.2f us)\r\n", cfg->width_cycles, cfg->width_cycles / 150.0f);
            } else if (strcmp(parts->parts[1], "GAP") == 0) {
                uart_cli_printf("%u cycles (%.2f us)\r\n", cfg->gap_cycles, cfg->gap_cycles / 150.0f);
            } else if (strcmp(parts->parts[1], "COUNT") == 0) {
                uart_cli_printf("%u\r\n", cfg->count);
            } else {
                api_error_printf("ERROR: Unknown variable '%s' (use PAUSE/WIDTH/GAP/COUNT)\r\n", parts->parts[1]);
            }
        }

    } else if (strcmp(parts->parts[0], "ARM") == 0) {
        if (parts->count == 1) {
            // Query ARM status
            system_flags_t* flags = glitch_get_flags();
            if (flags->armed) {
                uart_cli_send("ARMED\r\n");
            } else {
                uart_cli_send("DISARMED\r\n");
            }
        } else if (parts->count == 2) {
            if (strcmp(parts->parts[1], "ON") == 0) {
                if (glitch_arm()) {
                    uart_cli_send("OK: System armed\r\n");
                } else {
                    uart_cli_send("ERROR: Failed to arm system\r\n");
                }
            } else if (strcmp(parts->parts[1], "TRACE") == 0) {
                if (glitch_arm_trace()) {
                    uart_cli_send("OK: Trace armed (trigger only, no glitch pulse)\r\n");
                } else {
                    uart_cli_send("ERROR: Failed to arm trace\r\n");
                }
            } else if (strcmp(parts->parts[1], "OFF") == 0) {
                glitch_disarm();
                uart_cli_send("OK: System disarmed\r\n");
            } else {
                api_error("ERROR: Usage: ARM <ON|OFF|TRACE>\r\n");
                goto api_response;
            }
        } else {
            api_error("ERROR: Usage: ARM [ON|OFF]\r\n");
            goto api_response;
        }

    } else if (strcmp(parts->parts[0], "CLOCK") == 0) {
        if (parts->count == 1) {
            // Show current clock status
            uint32_t freq = clock_get_frequency();
            bool enabled = clock_is_enabled();
            if (freq == 0) {
                uart_cli_send("Clock: not configured\r\n");
            } else {
                uart_cli_printf("Clock: %u Hz (%s)\r\n", freq, enabled ? "ON" : "OFF");
            }
        } else {
            // Parse arguments: CLOCK [freq] [ON|OFF]
            uint32_t new_freq = 0;
            bool set_freq = false;
            bool set_enable = false;
            bool enable = false;

            for (int i = 1; i < parts->count; i++) {
                if (strcmp(parts->parts[i], "ON") == 0) {
                    set_enable = true;
                    enable = true;
                } else if (strcmp(parts->parts[i], "OFF") == 0) {
                    set_enable = true;
                    enable = false;
                } else {
                    // Try to parse as frequency
                    new_freq = atoi(parts->parts[i]);
                    if (new_freq > 0) {
                        set_freq = true;
                    }
                }
            }

            // Apply changes
            if (set_freq) {
                clock_set_frequency(new_freq);
                uart_cli_printf("OK: Clock frequency set to %u Hz\r\n", new_freq);
            }

            if (set_enable) {
                if (enable) {
                    if (clock_get_frequency() == 0) {
                        uart_cli_send("ERROR: Set frequency first\r\n");
                        goto api_response;
                    }
                    clock_enable();
                    uart_cli_send("OK: Clock enabled\r\n");
                } else {
                    clock_disable();
                    uart_cli_send("OK: Clock disabled\r\n");
                }
            }

            // Show final status if both were set
            if (set_freq && set_enable && enable) {
                uart_cli_printf("Clock running at %u Hz\r\n", clock_get_frequency());
            }

            if (!set_freq && !set_enable) {
                api_error("ERROR: Usage: CLOCK <freq> [ON|OFF] or CLOCK ON|OFF\r\n");
            }
        }

    } else if (strcmp(parts->parts[0], "GLITCH") == 0) {
        if (glitch_execute()) {
            uart_cli_send("OK: Glitch executed\r\n");
        } else {
            uart_cli_send("ERROR: Failed to execute glitch\r\n");
        }

    } else if (strcmp(parts->parts[0], "RESET") == 0) {
        glitch_reset();
        uart_cli_send("OK: System reset\r\n");

    } else if (strcmp(parts->parts[0], "REBOOT") == 0) {
        // Check for bootloader argument
        if (parts->count >= 2 && strcmp(parts->parts[1], "BL") == 0) {
            uart_cli_send("Rebooting into bootloader mode...\r\n");
            sleep_ms(100);  // Give time for message to be sent
            reset_usb_boot(0, 0);  // Reboot into USB bootloader
        } else {
            uart_cli_send("Rebooting Pico...\r\n");
            sleep_ms(100);  // Give time for message to be sent
            watchdog_reboot(0, 0, 0);
        }

    } else if (strcmp(parts->parts[0], "DEBUG") == 0) {
        extern void target_set_debug(bool enable);
        extern bool target_get_debug(void);

        if (parts->count < 2) {
            // No argument - show current state
            uart_cli_printf("Debug mode: %s\r\n", target_get_debug() ? "ON" : "OFF");
        } else if (strcmp(parts->parts[1], "ON") == 0) {
            target_set_debug(true);
            uart_cli_send("OK: Debug mode enabled - all target UART traffic will be displayed\r\n");
        } else if (strcmp(parts->parts[1], "OFF") == 0) {
            target_set_debug(false);
            uart_cli_send("OK: Debug mode disabled\r\n");
        } else {
            uart_cli_send("ERROR: Usage: DEBUG [ON|OFF]\r\n");
        }

    } else if (strcmp(parts->parts[0], "PINS") == 0) {
        glitch_config_t *cfg = glitch_get_config();

        uart_cli_send("=== Pin Configuration ===\r\n\r\n");
        uart_cli_send("== Communication ==\r\n");
        uart_cli_send("USB  - CLI (ttyACM0, USB CDC)\r\n");
        uart_cli_send("GP0  - ChipSHOUTER UART TX (UART0)\r\n");
        uart_cli_send("GP1  - ChipSHOUTER UART RX (UART0)\r\n");
        uart_cli_send("GP4  - Target UART TX (UART1, bootloader/bypass TX)\r\n");
        uart_cli_send("GP5  - Target UART RX (UART1, bootloader/bypass RX, also PIO monitored)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Glitch Control ==\r\n");
        uart_cli_printf("GP%u  - Glitch Output (normal)\r\n", PIN_GLITCH_OUT);
        uart_cli_printf("GP%u  - Glitch Output (inverted)\r\n", PIN_GLITCH_OUT_INV);
        uart_cli_printf("GP%u  - Trigger Input\r\n", cfg->trigger_pin);
        uart_cli_printf("GP%u  - Clock Output\r\n", PIN_CLOCK);
        uart_cli_printf("GP%u  - Armed Status\r\n", PIN_ARMED);
        uart_cli_printf("GP%u  - Glitch Fired\r\n", PIN_GLITCH_FIRED);
        uart_cli_send("\r\n");
        uart_cli_send("== XY Platform (Grbl) ==\r\n");
        uart_cli_send("GP8  - Grbl UART TX (UART1 alternate)\r\n");
        uart_cli_send("GP9  - Grbl UART RX (UART1 alternate)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Target Control ==\r\n");
        uart_cli_send("GP10 - Target Power (ganged, default ON, 12mA drive)\r\n");
        uart_cli_send("GP11 - Target Power (ganged, default ON, 12mA drive)\r\n");
        uart_cli_send("GP12 - Target Power (ganged, default ON, 12mA drive)\r\n");
        uart_cli_send("GP15 - Target Reset (default HIGH, LOW 300ms pulse)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("GP13 - BOOT0 Control\r\n");
        uart_cli_send("GP14 - BOOT1 Control\r\n");
        uart_cli_send("GP26 - ADC Power Monitor (voltage sense for glitch detection)\r\n");
        uart_cli_send("GP27 - ADC Shunt Current Monitor (timing measurement)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Debug Interface (SWD/JTAG) ==\r\n");
        uart_cli_send("GP15 - nRST / TRST (shared with Target Reset)\r\n");
        uart_cli_send("GP17 - SWCLK / TCK\r\n");
        uart_cli_send("GP18 - SWDIO / TMS\r\n");
        uart_cli_send("GP19 - TDI (JTAG only)\r\n");
        uart_cli_send("GP20 - TDO (JTAG only)\r\n");
        uart_cli_send("GP21 - RTCK (JTAG adaptive clocking, optional)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Status ==\r\n");
        uart_cli_send("GP25 - Status LED\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== External Reset (suggested) ==\r\n");
        uart_cli_send("EN   - Tie to FTDI DTR (on /dev/ttyUSB0) for host-driven reset.\r\n");
        uart_cli_send("       Use scripts/reset_pico.py to pulse DTR low ~100ms.\r\n");
        uart_cli_send("       Useful when the CLI is hung and REBOOT BL won't respond.\r\n");

    } else if (strcmp(parts->parts[0], "TRIGGER") == 0) {
        glitch_config_t *cfg = glitch_get_config();

        if (parts->count == 1) {
            // Show current trigger configuration
            uart_cli_printf("Trigger type: ");
            if (cfg->trigger == TRIGGER_NONE) {
                uart_cli_send("NONE\r\n");
            } else if (cfg->trigger == TRIGGER_GPIO) {
                uart_cli_printf("GPIO (Pin: GP%u, Edge: %s)\r\n",
                    cfg->trigger_pin,
                    cfg->trigger_edge == EDGE_RISING ? "RISING" : "FALLING");
            } else if (cfg->trigger == TRIGGER_UART) {
                uart_cli_printf("UART (Byte: 0x%02X, %s)\r\n", cfg->trigger_byte,
                               cfg->trigger_uart_pin == 4 ? "TX" : "RX");
            }
        } else if (strcmp(parts->parts[1], "NONE") == 0) {
            glitch_set_trigger_type(TRIGGER_NONE);
            uart_cli_send("OK: Trigger disabled\r\n");
        } else if (strcmp(parts->parts[1], "GPIO") == 0) {
            if (parts->count < 3) {
                uart_cli_send("ERROR: Usage: TRIGGER GPIO <RISING|FALLING>\r\n");
                goto api_response;
            }
            edge_type_t edge = (strcmp(parts->parts[2], "RISING") == 0) ? EDGE_RISING : EDGE_FALLING;
            glitch_set_trigger_pin(3, edge);  // GPIO trigger is hardcoded to GP3
            glitch_set_trigger_type(TRIGGER_GPIO);
            uart_cli_printf("OK: GPIO trigger on GP3, %s edge\r\n",
                          edge == EDGE_RISING ? "RISING" : "FALLING");
        } else if (strcmp(parts->parts[1], "UART") == 0) {
            if (parts->count < 3) {
                uart_cli_send("ERROR: Usage: TRIGGER UART <byte> [TX|RX]\r\n");
                goto api_response;
            }
            // Parse hex value (support both 0x0D and 0D formats)
            uint8_t byte;
            const char *val = parts->parts[2];
            if (val[0] == '0' && (val[1] == 'X' || val[1] == 'x')) {
                // Has 0x prefix, skip it
                byte = (uint8_t)strtol(val + 2, NULL, 16);
            } else {
                // No prefix, try hex first, fall back to decimal
                char *endptr;
                long parsed = strtol(val, &endptr, 16);
                if (*endptr == '\0') {
                    // Successfully parsed as hex
                    byte = (uint8_t)parsed;
                } else {
                    // Fall back to decimal
                    byte = (uint8_t)atoi(val);
                }
            }
            // Optional TX/RX pin selection (default RX = GP5)
            uint8_t uart_pin = 5;  // GP5 = RX (STM32 TX → Pico)
            const char *pin_name = "RX";
            if (parts->count >= 4) {
                if (strcasecmp(parts->parts[3], "TX") == 0) {
                    uart_pin = 4;  // GP4 = TX (Pico → STM32)
                    pin_name = "TX";
                } else if (strcasecmp(parts->parts[3], "RX") == 0) {
                    uart_pin = 5;
                    pin_name = "RX";
                }
            }
            glitch_set_trigger_byte(byte);
            glitch_set_trigger_type(TRIGGER_UART);
            glitch_get_config()->trigger_uart_pin = uart_pin;
            uart_cli_printf("OK: UART trigger on %s byte 0x%02X (%u)\r\n", pin_name, byte, byte);
        } else {
            api_error_printf("ERROR: Unknown trigger type '%s' (use NONE/GPIO/UART)\r\n", parts->parts[1]);
        }

    } else if (strcmp(parts->parts[0], "TARGET") == 0) {
        extern void target_set_type(target_type_t type);
        extern target_type_t target_get_type(void);
        extern bool target_enter_bootloader(uint32_t baud, uint32_t crystal_khz);
        extern void target_uart_send_hex(const char *hex_str);
        extern void target_uart_print_response_hex(void);
        extern void target_uart_clear_response(void);
        extern void target_reset_execute(void);
        extern void target_reset_config(uint8_t pin, uint32_t period_ms, bool active_high);

        if (parts->count < 2) {
            uart_cli_send("ERROR: Usage: TARGET <LPC|STM32F1|STM32F3|STM32F4|STM32L4|BOOTLOADER|SYNC|SEND|RESPONSE|RESET|TIMEOUT|POWER>\r\n");
            goto api_response;
        }

        if (strcmp(parts->parts[1], "LPC") == 0) {
            target_set_type(TARGET_LPC);
            uart_cli_send("OK: Target type set to LPC (NXP ISP protocol)\r\n");
        } else if (strcmp(parts->parts[1], "STM32F1") == 0) {
            target_set_type(TARGET_STM32F1);
            uart_cli_send("OK: Target type set to STM32F1 (Cortex-M3, 128KB flash)\r\n");
        } else if (strcmp(parts->parts[1], "STM32F3") == 0) {
            target_set_type(TARGET_STM32F3);
            uart_cli_send("OK: Target type set to STM32F3 (Cortex-M4, 64KB flash)\r\n");
        } else if (strcmp(parts->parts[1], "STM32F4") == 0) {
            target_set_type(TARGET_STM32F4);
            uart_cli_send("OK: Target type set to STM32F4 (Cortex-M4, 512KB flash)\r\n");
        } else if (strcmp(parts->parts[1], "STM32L4") == 0) {
            target_set_type(TARGET_STM32L4);
            uart_cli_send("OK: Target type set to STM32L4 (Cortex-M4, 256KB flash)\r\n");
        } else if (strcmp(parts->parts[1], "BOOT0") == 0) {
            gpio_init(PIN_BOOT0);
            gpio_set_dir(PIN_BOOT0, GPIO_OUT);
            if (parts->count >= 3 && strcmp(parts->parts[2], "LOW") == 0) {
                gpio_put(PIN_BOOT0, 0);
                uart_cli_send("OK: BOOT0 (GP13) = LOW\r\n");
            } else {
                gpio_put(PIN_BOOT0, 1);
                uart_cli_send("OK: BOOT0 (GP13) = HIGH\r\n");
            }
        } else if (strcmp(parts->parts[1], "BOOT1") == 0) {
            gpio_init(PIN_BOOT1);
            gpio_set_dir(PIN_BOOT1, GPIO_OUT);
            if (parts->count >= 3 && strcmp(parts->parts[2], "LOW") == 0) {
                gpio_put(PIN_BOOT1, 0);
                uart_cli_send("OK: BOOT1 (GP14) = LOW\r\n");
            } else {
                gpio_put(PIN_BOOT1, 1);
                uart_cli_send("OK: BOOT1 (GP14) = HIGH\r\n");
            }
        } else if (strcmp(parts->parts[1], "BOOTLOADER") == 0 ||
                   strcmp(parts->parts[1], "SYNC") == 0) {
            // Release SWD debug port so target can reset into bootloader
            extern void swd_deinit(void);
            extern bool swd_is_connected(void);
            bool was_swd = swd_is_connected();
            swd_deinit();

            // If SWD was active, power-cycle to fully clear Cortex-M debug state
            // (nRST alone doesn't clear debug halt on Cortex-M3)
            if (was_swd) {
                extern void target_power_cycle(uint32_t time_ms);
                target_power_cycle(100);
                sleep_ms(100);
            }

            uint32_t baud = 115200;       // Default baud
            uint32_t crystal_khz = 12000; // Default 12MHz crystal
            uint32_t reset_delay_ms = 500; // Default 500ms delay for bootloader to initialize
            uint32_t retries = 5;         // Default 5 retries
            if (parts->count >= 3) {
                baud = atoi(parts->parts[2]);
            }
            if (parts->count >= 4) {
                crystal_khz = atoi(parts->parts[3]);
            }
            if (parts->count >= 5) {
                reset_delay_ms = atoi(parts->parts[4]);
            }
            if (parts->count >= 6) {
                retries = atoi(parts->parts[5]);
                // Ensure at least 1 retry
                if (retries < 1) {
                    retries = 1;
                }
            }

            // Check target type before doing anything
            if (target_get_type() == TARGET_NONE) {
                api_error("ERROR: No target type set. Use TARGET <LPC|STM32F1|STM32F3|...> first\r\n");
                goto api_response;
            }

            // For STM32, set BOOT0 HIGH before reset (bootloader mode)
            bool is_stm32 = target_is_stm32(target_get_type());
            if (is_stm32) {
                gpio_init(PIN_BOOT0);
                gpio_set_dir(PIN_BOOT0, GPIO_OUT);
                gpio_put(PIN_BOOT0, 1);
                uart_cli_send("OK: BOOT0 = HIGH\r\n");
            }

            // Try up to specified times to sync with target
            bool sync_success = false;
            for (uint32_t retry = 0; retry < retries; retry++) {
                if (retry > 0) {
                    uart_cli_printf("Retry %u/%u...\r\n", retry, retries);
                }
                uart_cli_send("Resetting target...\r\n");
                target_reset_execute();
                uart_cli_printf("Waiting %u ms for target to boot...\r\n", reset_delay_ms);
                sleep_ms(reset_delay_ms);
                uart_cli_send("Entering bootloader mode...\r\n");
                if (target_enter_bootloader(baud, crystal_khz)) {
                    sync_success = true;
                    break;
                }
                if (retry < retries - 1) {  // Don't print on last retry
                    uart_cli_send("Sync failed, retrying...\r\n");
                    sleep_ms(100);  // Small delay between retries
                }
            }
            if (!sync_success) {
                uart_cli_printf("ERROR: Failed to sync with target after %u attempts\r\n", retries);
            }
        } else if (strcmp(parts->parts[1], "SEND") == 0) {
            extern void target_uart_send_string(const char *str);
            extern const char* uart_cli_get_command(void);

            if (parts->count < 3) {
                uart_cli_send("ERROR: Usage: TARGET SEND <hex_data or \"text\">\r\n");
                goto api_response;
            }

            // Get original command to check for quoted string
            const char *orig_cmd = uart_cli_get_command();
            const char *send_pos = strstr(orig_cmd, "send ");
            if (!send_pos) {
                send_pos = strstr(orig_cmd, "SEND ");
            }

            if (send_pos) {
                // Skip past "SEND " to get to the argument
                send_pos += 5;
                // Skip whitespace
                while (*send_pos == ' ' || *send_pos == '\t') {
                    send_pos++;
                }

                // Check if it starts with a quote
                if (*send_pos == '"' || *send_pos == '\'') {
                    char quote_char = *send_pos;
                    send_pos++; // Skip opening quote

                    // Find closing quote
                    const char *end_quote = strchr(send_pos, quote_char);
                    if (end_quote) {
                        // Extract the string between quotes
                        static char quoted_str[128];
                        size_t len = end_quote - send_pos;
                        if (len >= sizeof(quoted_str)) {
                            len = sizeof(quoted_str) - 1;
                        }
                        strncpy(quoted_str, send_pos, len);
                        quoted_str[len] = '\0';

                        // Send as string (no uppercase conversion)
                        target_uart_send_string(quoted_str);
                    } else {
                        uart_cli_send("ERROR: Unterminated quote\r\n");
                    }
                } else {
                    // No quotes, treat as hex
                    target_uart_send_hex(parts->parts[2]);
                }
            } else {
                // Fallback to hex
                target_uart_send_hex(parts->parts[2]);
            }
        } else if (strcmp(parts->parts[1], "RESPONSE") == 0) {
            target_uart_print_response_hex();
        } else if (strcmp(parts->parts[1], "RESET") == 0) {
            // Parse configuration parameters with defaults
            uint8_t pin = 15;           // Default pin
            uint32_t period = 300;      // Default 300ms
            bool active_high = false;   // Default active low
            bool has_params = (parts->count > 2);

            for (uint8_t i = 2; i < parts->count; i++) {
                if (strcmp(parts->parts[i], "PIN") == 0 && i + 1 < parts->count) {
                    pin = atoi(parts->parts[i + 1]);
                    i++;  // Skip next part
                } else if (strcmp(parts->parts[i], "PERIOD") == 0 && i + 1 < parts->count) {
                    period = atoi(parts->parts[i + 1]);
                    i++;  // Skip next part
                } else if (strcmp(parts->parts[i], "HIGH") == 0) {
                    active_high = true;
                }
            }

            // Always configure the pin (with defaults or specified values)
            target_reset_config(pin, period, active_high);

            if (has_params) {
                uart_cli_printf("OK: Reset configured: GP%u, %u ms, active %s\r\n",
                                pin, period, active_high ? "HIGH" : "LOW");
            } else {
                // No parameters given — also execute the reset
                target_reset_execute();
            }
        } else if (strcmp(parts->parts[1], "TIMEOUT") == 0) {
            if (parts->count >= 3) {
                // Set timeout
                uint32_t timeout_ms = atoi(parts->parts[2]);
                target_set_timeout(timeout_ms);
                char msg[64];
                snprintf(msg, sizeof(msg), "Target bridge timeout set to %lu ms\r\n", timeout_ms);
                uart_cli_send(msg);
            } else {
                // Show current timeout
                uint32_t timeout_ms = target_get_timeout();
                char msg[64];
                snprintf(msg, sizeof(msg), "Target bridge timeout: %lu ms\r\n", timeout_ms);
                uart_cli_send(msg);
            }
        } else if (strcmp(parts->parts[1], "POWER") == 0) {
            if (parts->count < 3) {
                bool power_state = target_power_get_state();
                uart_cli_printf("Target power (GP10): %s\r\n", power_state ? "ON" : "OFF");
            } else {
                const char *power_cmds[] = {"ON", "OFF", "CYCLE"};
                if (!match_and_replace(&parts->parts[2], power_cmds, 3, "POWER command")) {
                    goto api_response;
                }

                if (strcmp(parts->parts[2], "ON") == 0) {
                    target_power_on();
                } else if (strcmp(parts->parts[2], "OFF") == 0) {
                    target_power_off();
                } else if (strcmp(parts->parts[2], "CYCLE") == 0) {
                    uint32_t cycle_time_ms = 300;
                    if (parts->count >= 4) {
                        if (!parse_u32(parts->parts[3], 0, &cycle_time_ms)) {
                            api_error("ERROR: Invalid cycle time. Usage: TARGET POWER CYCLE [ms]\r\n");
                            goto api_response;
                        }
                    }
                    target_power_cycle(cycle_time_ms);
                }
            }
        } else if (strcmp(parts->parts[1], "BL") == 0) {
            if (parts->count < 3) {
                uart_cli_send("Usage: TARGET BL <command>\r\n");
                uart_cli_send("  GET                  - Bootloader version + supported commands\r\n");
                uart_cli_send("  GV                   - Get version + option bytes\r\n");
                uart_cli_send("  GID                  - Chip product ID\r\n");
                uart_cli_send("  READ <addr> [count]  - Read memory (default 256 bytes)\r\n");
                uart_cli_send("  WRITE <addr> <hex>   - Write hex bytes to memory\r\n");
                uart_cli_send("  GO <addr>            - Jump to address\r\n");
                uart_cli_send("  ERASE <page|ALL WIPE> - Erase flash page or mass erase\r\n");
                uart_cli_send("  RU WIPE              - Readout unprotect (mass erase!)\r\n");
                uart_cli_send("  RP CONFIRM           - Readout protect (set RDP1)\r\n");
                uart_cli_send("Requires: TARGET SYNC first\r\n");
            } else {
                const char *bl_cmds[] = {"GET", "GV", "GID", "READ", "WRITE", "GO", "ERASE", "RU", "RP"};
                if (!match_and_replace(&parts->parts[2], bl_cmds, 9, "BL command")) {
                    goto api_response;
                }

                // Auto-sync: if bootloader not synced, run TARGET SYNC automatically
                if (!target_is_bl_synced()) {
                    if (target_get_type() == TARGET_NONE) {
                        api_error("ERROR: No target type set. Use TARGET <STM32F1|STM32F3|...> first\r\n");
                        goto api_response;
                    }
                    uart_cli_send("Bootloader not synced — running TARGET SYNC...\r\n");

                    extern void swd_deinit(void);
                    extern bool swd_is_connected(void);
                    if (swd_is_connected()) {
                        swd_deinit();
                        target_power_cycle(100);
                        sleep_ms(100);
                    }

                    bool is_stm32 = target_is_stm32(target_get_type());
                    if (is_stm32) {
                        gpio_init(PIN_BOOT0);
                        gpio_set_dir(PIN_BOOT0, GPIO_OUT);
                        gpio_put(PIN_BOOT0, 1);
                    }

                    bool synced = false;
                    for (uint32_t retry = 0; retry < 5; retry++) {
                        if (retry > 0)
                            uart_cli_printf("Retry %u/5...\r\n", retry);
                        target_reset_execute();
                        sleep_ms(500);
                        if (target_enter_bootloader(115200, 12000)) {
                            synced = true;
                            break;
                        }
                        sleep_ms(100);
                    }
                    if (!synced) {
                        api_error("ERROR: Auto-sync failed — check wiring and target power\r\n");
                        goto api_response;
                    }
                }

                if (strcmp(parts->parts[2], "GET") == 0) {
                    stm32_bl_get();
                } else if (strcmp(parts->parts[2], "GV") == 0) {
                    stm32_bl_get_version();
                } else if (strcmp(parts->parts[2], "GID") == 0) {
                    stm32_bl_gid();
                } else if (strcmp(parts->parts[2], "READ") == 0) {
                    if (parts->count < 4) {
                        api_error("ERROR: Usage: TARGET BL READ <addr> [count]\r\n");
                        goto api_response;
                    }
                    uint32_t addr = strtoul(parts->parts[3], NULL, 16);
                    uint32_t count = 256;
                    if (parts->count >= 5) {
                        if (!parse_u32(parts->parts[4], 0, &count)) {
                            api_error("ERROR: Invalid count\r\n");
                            goto api_response;
                        }
                    }
                    stm32_bl_read(addr, count);
                } else if (strcmp(parts->parts[2], "WRITE") == 0) {
                    if (parts->count < 5) {
                        api_error("ERROR: Usage: TARGET BL WRITE <addr> <hex_bytes>\r\n");
                        goto api_response;
                    }
                    uint32_t addr = strtoul(parts->parts[3], NULL, 16);
                    const char *hex = parts->parts[4];
                    size_t hex_len = strlen(hex);
                    if (hex_len == 0 || hex_len % 2 != 0 || hex_len > 512) {
                        api_error("ERROR: Hex data must be even length, max 256 bytes\r\n");
                        goto api_response;
                    }
                    uint8_t buf[256];
                    uint32_t data_len = hex_len / 2;
                    for (uint32_t i = 0; i < data_len; i++) {
                        char byte_str[3] = {hex[i*2], hex[i*2+1], '\0'};
                        buf[i] = (uint8_t)strtoul(byte_str, NULL, 16);
                    }
                    stm32_bl_write(addr, buf, data_len);
                } else if (strcmp(parts->parts[2], "GO") == 0) {
                    if (parts->count < 4) {
                        api_error("ERROR: Usage: TARGET BL GO <addr>\r\n");
                        goto api_response;
                    }
                    uint32_t addr = strtoul(parts->parts[3], NULL, 16);
                    stm32_bl_go(addr);
                } else if (strcmp(parts->parts[2], "ERASE") == 0) {
                    if (parts->count < 4) {
                        api_error("ERROR: Usage: TARGET BL ERASE <page> or TARGET BL ERASE ALL WIPE\r\n");
                        goto api_response;
                    }
                    if (strcasecmp(parts->parts[3], "ALL") == 0) {
                        if (parts->count < 5 || strcasecmp(parts->parts[4], "WIPE") != 0) {
                            api_error("ERROR: Mass erase requires confirmation: TARGET BL ERASE ALL WIPE\r\n");
                            goto api_response;
                        }
                        stm32_bl_erase(-1, true);
                    } else {
                        int page = (int)strtol(parts->parts[3], NULL, 0);
                        stm32_bl_erase(page, false);
                    }
                } else if (strcmp(parts->parts[2], "RU") == 0) {
                    if (parts->count < 4 || strcasecmp(parts->parts[3], "WIPE") != 0) {
                        api_error("ERROR: Readout unprotect erases all flash! Confirm: TARGET BL RU WIPE\r\n");
                        goto api_response;
                    }
                    stm32_bl_readout_unprotect();
                } else if (strcmp(parts->parts[2], "RP") == 0) {
                    if (parts->count < 4 || strcasecmp(parts->parts[3], "CONFIRM") != 0) {
                        api_error("ERROR: Readout protect locks flash! Confirm: TARGET BL RP CONFIRM\r\n");
                        goto api_response;
                    }
                    stm32_bl_readout_protect();
                }
            }
        } else if (strcmp(parts->parts[1], "GLITCH") == 0) {
            if (parts->count < 3) {
                uart_cli_send("Usage: TARGET GLITCH <command>\r\n");
                uart_cli_send("  TEST <voltage> [count]     - Basic power glitch test\r\n");
                uart_cli_send("  SWEEP                      - Voltage sweep\r\n");
                uart_cli_send("  PAYLOAD [voltage] [attempts] - Glitch with SRAM payload\r\n");
                uart_cli_send("  BYPASS [attempts] [dump_bytes] - RDP bypass + flash dump\r\n");
                uart_cli_send("  HALT [dump_bytes]          - Halt-based flash dump\r\n");
                uart_cli_send("  LITERAL                    - Literal payload test\r\n");
                uart_cli_send("  REGDUMP                    - Register dump payload\r\n");
                uart_cli_send("  GLITCH_REGDUMP [attempts]  - Glitch + register dump\r\n");
                uart_cli_send("  RESETTEST                  - Reset/low-power disruption test\r\n");
                uart_cli_send("  TIMING [name|0xADDR] [samples] [FLASH|BOOTLOADER]\r\n");
                uart_cli_send("                             - Measure cycle count to breakpoint (DWT+ADC)\r\n");
            } else {
                const char *glitch_cmds[] = {"TEST", "SWEEP", "PAYLOAD", "BYPASS", "HALT", "LITERAL", "REGDUMP", "GLITCH_REGDUMP", "RESETTEST", "TIMING"};
                if (!match_and_replace(&parts->parts[2], glitch_cmds, 10, "GLITCH command")) {
                    goto api_response;
                }

                if (strcmp(parts->parts[2], "TEST") == 0) {
                    if (parts->count < 4) {
                        api_error("ERROR: Usage: TARGET GLITCH TEST <voltage> [count]\r\n");
                        goto api_response;
                    }
                    float voltage = strtof(parts->parts[3], NULL);
                    uint32_t count = 10;
                    if (parts->count >= 5) {
                        if (!parse_u32(parts->parts[4], 0, &count)) {
                            api_error("ERROR: Invalid count. Usage: TARGET GLITCH TEST <voltage> [count]\r\n");
                            goto api_response;
                        }
                    }
                    if (count < 1) count = 1;
                    if (count > 1000) count = 1000;
                    target_power_glitch(voltage, count);
                } else if (strcmp(parts->parts[2], "SWEEP") == 0) {
                    target_power_sweep();
                } else if (strcmp(parts->parts[2], "PAYLOAD") == 0) {
                    float voltage = 1.8f;
                    uint32_t attempts = 20;
                    if (parts->count >= 4)
                        voltage = strtof(parts->parts[3], NULL);
                    if (parts->count >= 5) {
                        if (!parse_u32(parts->parts[4], 0, &attempts)) {
                            api_error("ERROR: Invalid attempts. Usage: TARGET GLITCH PAYLOAD [voltage] [attempts]\r\n");
                            goto api_response;
                        }
                    }
                    if (attempts < 1) attempts = 1;
                    if (attempts > 100) attempts = 100;
                    target_power_payload(voltage, attempts);
                } else if (strcmp(parts->parts[2], "BYPASS") == 0) {
                    uint32_t attempts = 20;
                    uint32_t dump_bytes = 0;
                    if (parts->count >= 4) {
                        if (!parse_u32(parts->parts[3], 0, &attempts)) {
                            api_error("ERROR: Invalid attempts. Usage: TARGET GLITCH BYPASS [attempts] [dump_bytes]\r\n");
                            goto api_response;
                        }
                    }
                    if (parts->count >= 5) {
                        if (!parse_u32(parts->parts[4], 0, &dump_bytes)) {
                            api_error("ERROR: Invalid dump_bytes. Usage: TARGET GLITCH BYPASS [attempts] [dump_bytes]\r\n");
                            goto api_response;
                        }
                    }
                    if (attempts < 1) attempts = 1;
                    if (attempts > 100) attempts = 100;
                    target_power_bypass(attempts, dump_bytes);
                } else if (strcmp(parts->parts[2], "HALT") == 0) {
                    uint32_t dump_bytes = 0;
                    if (parts->count >= 4) {
                        if (!parse_u32(parts->parts[3], 0, &dump_bytes)) {
                            api_error("ERROR: Invalid dump_bytes. Usage: TARGET GLITCH HALT [dump_bytes]\r\n");
                            goto api_response;
                        }
                    }
                    target_power_halt(dump_bytes);
                } else if (strcmp(parts->parts[2], "LITERAL") == 0) {
                    target_power_literal();
                } else if (strcmp(parts->parts[2], "REGDUMP") == 0) {
                    target_power_regdump();
                } else if (strcmp(parts->parts[2], "GLITCH_REGDUMP") == 0) {
                    uint32_t attempts = 20;
                    if (parts->count >= 4) {
                        if (!parse_u32(parts->parts[3], 0, &attempts)) {
                            api_error("ERROR: Invalid attempts. Usage: TARGET GLITCH GLITCH_REGDUMP [attempts]\r\n");
                            goto api_response;
                        }
                    }
                    if (attempts < 1) attempts = 1;
                    if (attempts > 100) attempts = 100;
                    target_power_glitch_regdump(attempts);
                } else if (strcmp(parts->parts[2], "RESETTEST") == 0) {
                    target_power_resettest();
                } else if (strcmp(parts->parts[2], "TIMING") == 0) {
                    if (parts->count < 4) {
                        // No args: list breakpoints
                        target_power_timing(NULL, 0, true);
                    } else {
                        const char *bp_name = parts->parts[3];
                        uint32_t samples = 0;  // 0 = use default in target_power_timing()
                        bool bootloader = true;
                        if (parts->count >= 5) {
                            // Check if arg 4 is a boot mode or sample count
                            if (strcasecmp(parts->parts[4], "FLASH") == 0) {
                                bootloader = false;
                            } else if (strcasecmp(parts->parts[4], "BOOTLOADER") == 0) {
                                bootloader = true;
                            } else {
                                if (!parse_u32(parts->parts[4], 0, &samples)) {
                                    api_error("ERROR: Invalid samples. Usage: TARGET GLITCH TIMING <name|0xADDR> [samples] [FLASH|BOOTLOADER]\r\n");
                                    goto api_response;
                                }
                            }
                        }
                        if (parts->count >= 6) {
                            if (strcasecmp(parts->parts[5], "FLASH") == 0) {
                                bootloader = false;
                            } else if (strcasecmp(parts->parts[5], "BOOTLOADER") == 0) {
                                bootloader = true;
                            }
                        }
                        target_power_timing(bp_name, samples, bootloader);
                    }
                }
            }
        } else {
            api_error_printf("ERROR: Unknown TARGET command '%s'\r\n", parts->parts[1]);
        }

    } else if (strcmp(parts->parts[0], "CS") == 0) {
        extern void chipshot_uart_init(void);
        extern void chipshot_uart_send(const char *data);
        extern void chipshot_uart_process(void);
        extern bool chipshot_uart_response_ready(void);
        extern const char* chipshot_uart_get_response(void);
        extern void chipshot_uart_clear_response(void);
        extern const char* chipshot_uart_read_response_blocking(uint32_t timeout_ms);
        extern void chipshot_arm(void);
        extern void chipshot_disarm(void);
        extern void chipshot_fire(void);
        extern void chipshot_set_voltage(uint32_t voltage);
        extern void chipshot_set_pulse(uint32_t pulse_us);
        extern void chipshot_get_status(void);
        extern void chipshot_reset(void);
        extern void chipshot_set_trigger_hw(bool active_high);
        extern void chipshot_set_trigger_sw(void);

        if (parts->count < 2) {
            uart_cli_send("ERROR: Usage: CS <ARM|DISARM|FIRE|STATUS|VOLTAGE|PULSE|RESET|TRIGGER>\r\n");
            goto api_response;
        }

        if (strcmp(parts->parts[1], "ARM") == 0) {
            chipshot_arm();
            uart_cli_send("ChipSHOUTER: Sent arm command\r\n");

            // Blocking read with 2 second timeout
            const char* response = chipshot_uart_read_response_blocking(2000);
            if (response) {
                uart_cli_send("ChipSHOUTER response:\r\n");
                send_multiline_response(response);
                uart_cli_send("\r\n");
            } else {
                uart_cli_send("No response from ChipSHOUTER\r\n");
            }

        } else if (strcmp(parts->parts[1], "DISARM") == 0) {
            chipshot_disarm();
            uart_cli_send("ChipSHOUTER: Sent disarm command\r\n");

            // Blocking read with 2 second timeout
            const char* response = chipshot_uart_read_response_blocking(2000);
            if (response) {
                uart_cli_send("ChipSHOUTER response:\r\n");
                send_multiline_response(response);
                uart_cli_send("\r\n");
            } else {
                uart_cli_send("No response from ChipSHOUTER\r\n");
            }

        } else if (strcmp(parts->parts[1], "FIRE") == 0) {
            chipshot_fire();
            uart_cli_send("ChipSHOUTER: Sent trigger command\r\n");

            // Blocking read with 2 second timeout
            const char* response = chipshot_uart_read_response_blocking(2000);
            if (response) {
                uart_cli_send("ChipSHOUTER response:\r\n");
                send_multiline_response(response);
                uart_cli_send("\r\n");
            } else {
                uart_cli_send("No response from ChipSHOUTER\r\n");
            }

        } else if (strcmp(parts->parts[1], "STATUS") == 0) {
            chipshot_get_status();
            uart_cli_send("ChipSHOUTER: Sent status command\r\n");

            // Blocking read with 2 second timeout
            const char* response = chipshot_uart_read_response_blocking(2000);
            if (response) {
                uart_cli_send("ChipSHOUTER response:\r\n");
                send_multiline_response(response);
                uart_cli_send("\r\n");
            } else {
                uart_cli_send("No response from ChipSHOUTER\r\n");
            }

        } else if (strcmp(parts->parts[1], "VOLTAGE") == 0) {
            if (parts->count < 3) {
                // Show current voltage by querying status
                uart_cli_send("ChipSHOUTER: Querying status for voltage...\r\n");
                chipshot_get_status();
                const char* response = chipshot_uart_read_response_blocking(2000);
                if (response) {
                    send_multiline_response(response);
                    uart_cli_send("\r\n");
                } else {
                    uart_cli_send("No response from ChipSHOUTER\r\n");
                }
            } else {
                uint32_t voltage = atoi(parts->parts[2]);
                chipshot_set_voltage(voltage);
                uart_cli_printf("ChipSHOUTER: Sent voltage %u command\r\n", voltage);

                // Blocking read with 2 second timeout
                const char* response = chipshot_uart_read_response_blocking(2000);
                if (response) {
                    uart_cli_send("ChipSHOUTER response:\r\n");
                    send_multiline_response(response);
                    uart_cli_send("\r\n");
                } else {
                    uart_cli_send("No response from ChipSHOUTER\r\n");
                }
            }

        } else if (strcmp(parts->parts[1], "PULSE") == 0) {
            if (parts->count < 3) {
                // Show current pulse width by querying status
                uart_cli_send("ChipSHOUTER: Querying status for pulse width...\r\n");
                chipshot_get_status();
                const char* response = chipshot_uart_read_response_blocking(2000);
                if (response) {
                    send_multiline_response(response);
                    uart_cli_send("\r\n");
                } else {
                    uart_cli_send("No response from ChipSHOUTER\r\n");
                }
            } else {
                uint32_t pulse_ns = atoi(parts->parts[2]);
                chipshot_set_pulse(pulse_ns);
                uart_cli_printf("ChipSHOUTER: Sent pulse %u ns command\r\n", pulse_ns);

                // Blocking read with 2 second timeout
                const char* response = chipshot_uart_read_response_blocking(2000);
                if (response) {
                    uart_cli_send("ChipSHOUTER response:\r\n");
                    send_multiline_response(response);
                    uart_cli_send("\r\n");
                } else {
                    uart_cli_send("No response from ChipSHOUTER\r\n");
                }
            }

        } else if (strcmp(parts->parts[1], "RESET") == 0) {
            uart_cli_send("ChipSHOUTER: Sending reset command...\r\n");
            chipshot_reset();

            // Wait 5 seconds for reset to complete, actively draining UART
            uart_cli_send("ChipSHOUTER: Waiting 5 seconds for reset to complete...\r\n");
            uint32_t reset_start = to_ms_since_boot(get_absolute_time());
            while (to_ms_since_boot(get_absolute_time()) - reset_start < 5000) {
                // Continuously drain UART
                while (uart_is_readable(CHIPSHOT_UART_ID)) {
                    uart_getc(CHIPSHOT_UART_ID);
                }
                sleep_ms(10);
            }

            // Clear response buffer to start fresh
            chipshot_uart_clear_response();

            // Now send STATUS command to verify reset was successful
            uart_cli_send("ChipSHOUTER: Verifying reset...\r\n");
            chipshot_get_status();

            // Blocking read for status response
            const char* status_response = chipshot_uart_read_response_blocking(2000);
            if (status_response) {
                uart_cli_send("ChipSHOUTER Status:\r\n");
                send_multiline_response(status_response);
                uart_cli_send("\r\n");

                // Check if errors are cleared (look for "error" or "fault" in response)
                bool has_error = (strstr(status_response, "error") != NULL ||
                                 strstr(status_response, "fault") != NULL ||
                                 strstr(status_response, "Error") != NULL ||
                                 strstr(status_response, "Fault") != NULL);

                if (!has_error) {
                    uart_cli_send("SUCCESS: ChipSHOUTER reset complete, no errors\r\n");
                } else {
                    uart_cli_send("WARNING: ChipSHOUTER may still have errors\r\n");
                }
            } else {
                uart_cli_send("WARNING: Could not verify status after reset\r\n");
            }

        } else if (strcmp(parts->parts[1], "TRIGGER") == 0) {
            if (parts->count < 3) {
                uart_cli_send("ERROR: Usage: CS TRIGGER HW <HIGH|LOW> | CS TRIGGER SW\r\n");
                goto api_response;
            }

            // Match HW/SW argument
            const char *trigger_modes[] = {"HW", "SW"};
            if (!match_and_replace(&parts->parts[2], trigger_modes, 2, "trigger mode")) {
                goto api_response;
            }

            if (strcmp(parts->parts[2], "HW") == 0) {
                // Hardware trigger mode requires HIGH or LOW argument
                if (parts->count < 4) {
                    uart_cli_send("ERROR: Usage: CS TRIGGER HW <HIGH|LOW>\r\n");
                    goto api_response;
                }

                // Match HIGH/LOW argument
                const char *polarity_args[] = {"HIGH", "LOW"};
                if (!match_and_replace(&parts->parts[3], polarity_args, 2, "trigger polarity")) {
                    goto api_response;
                }

                bool active_high = (strcmp(parts->parts[3], "HIGH") == 0);
                chipshot_set_trigger_hw(active_high);
                uart_cli_send("ChipSHOUTER: Sent set trigger hw command (");
                uart_cli_send(active_high ? "active high)\r\n" : "active low)\r\n");
                uart_cli_printf("Configured GPIO%u with %s\r\n",
                    PIN_GLITCH_OUT,
                    active_high ? "pull-down" : "pull-up");

            } else if (strcmp(parts->parts[2], "SW") == 0) {
                chipshot_set_trigger_sw();
                uart_cli_send("ChipSHOUTER: Sent set trigger sw commands\r\n");
            }

            // Blocking read with 2 second timeout
            const char* response = chipshot_uart_read_response_blocking(2000);
            if (response) {
                uart_cli_send("ChipSHOUTER response:\r\n");
                send_multiline_response(response);
                uart_cli_send("\r\n");
            } else {
                uart_cli_send("No response from ChipSHOUTER\r\n");
            }

        } else if (strcmp(parts->parts[1], "HVOUT") == 0) {
            extern void chipshot_get_hv_out(void);
            chipshot_get_hv_out();
            uart_cli_send("ChipSHOUTER: Querying HV output status...\r\n");

            const char* response = chipshot_uart_read_response_blocking(2000);
            if (response) {
                uart_cli_send("ChipSHOUTER HV status:\r\n");
                send_multiline_response(response);
                uart_cli_send("\r\n");
            } else {
                uart_cli_send("No response from ChipSHOUTER\r\n");
            }

        } else if (strcmp(parts->parts[1], "FAULTS") == 0) {
            extern void chipshot_get_faults(void);
            chipshot_get_faults();
            uart_cli_send("ChipSHOUTER: Querying faults...\r\n");

            const char* response = chipshot_uart_read_response_blocking(2000);
            if (response) {
                uart_cli_send("ChipSHOUTER faults:\r\n");
                send_multiline_response(response);
                uart_cli_send("\r\n");
            } else {
                uart_cli_send("No response from ChipSHOUTER\r\n");
            }
        } else {
            api_error_printf("ERROR: Unknown CS command '%s'\r\n", parts->parts[1]);
        }

    } else if (strcmp(parts->parts[0], "API") == 0) {
        if (parts->count < 2) {
            // No argument - show current state
            uart_cli_printf("API mode: %s\r\n", api_mode ? "ON" : "OFF");
        } else if (strcmp(parts->parts[1], "ON") == 0) {
            api_mode = true;
            uart_cli_send("OK: API mode enabled\r\n");
        } else if (strcmp(parts->parts[1], "OFF") == 0) {
            api_mode = false;
            uart_cli_send("OK: API mode disabled\r\n");
        } else {
            api_error("ERROR: Usage: API [ON|OFF]\r\n");
        }

    } else if (strcmp(parts->parts[0], "GRBL") == 0) {
        extern void grbl_init(void);
        extern bool grbl_is_active(void);
        extern void grbl_send(const char *gcode);
        extern void grbl_home(void);
        extern void grbl_move_absolute(float x, float y, float feedrate);
        extern void grbl_move_relative(float dx, float dy, float feedrate);
        extern bool grbl_get_position(float *x, float *y, float *z);

        if (parts->count < 2) {
            uart_cli_send("ERROR: Usage: GRBL <SEND|UNLOCK|SET|HOME|MOVE|STEP|POS|TEST|DEBUG>\r\n");
            goto api_response;
        }

        // Auto-initialize Grbl UART if not already active (will auto-deinit target if needed)
        if (!grbl_is_active()) {
            grbl_init();
        }

        if (strcmp(parts->parts[1], "SEND") == 0) {
            if (parts->count < 3) {
                api_error("ERROR: Usage: GRBL SEND <gcode>\r\n");
                goto api_response;
            }
            // Reconstruct G-code from remaining parts, stripping quotes if present
            char gcode[128] = {0};
            for (int i = 2; i < parts->count; i++) {
                const char *part = parts->parts[i];
                // Strip leading quote from first part
                if (i == 2 && part[0] == '"') {
                    part++;
                }
                if (i > 2) strcat(gcode, " ");
                strcat(gcode, part);
            }
            // Strip trailing quote from final string
            size_t len = strlen(gcode);
            if (len > 0 && gcode[len-1] == '"') {
                gcode[len-1] = '\0';
            }
            grbl_send(gcode);
        } else if (strcmp(parts->parts[1], "HOME") == 0) {
            // Move to home position (0,0) - synchronous
            uint32_t timeout_ms = 30000;  // Default 30 second timeout
            if (parts->count >= 3) {
                timeout_ms = atoi(parts->parts[2]);
            }
            if (grbl_move_absolute_sync(0.0f, 0.0f, 300.0f, timeout_ms)) {
                uart_cli_send("OK: Moved to home position (0,0)\r\n");
            } else {
                api_error("ERROR: Move to home failed or timeout\r\n");
            }
        } else if (strcmp(parts->parts[1], "RESET") == 0) {
            grbl_reset();
            uart_cli_send("OK: Grbl soft reset sent\r\n");
        } else if (strcmp(parts->parts[1], "AUTOHOME") == 0) {
            // Synchronous homing - waits for completion
            uint32_t timeout_ms = 60000;  // Default 60 second timeout
            if (parts->count >= 3) {
                timeout_ms = atoi(parts->parts[2]);
            }

            if (grbl_home_sync(timeout_ms)) {
                uart_cli_send("OK: Homing complete\r\n");
            } else {
                api_error("ERROR: Homing failed or timeout\r\n");
            }
        } else if (strcmp(parts->parts[1], "MOVE") == 0) {
            // Synchronous move - waits for ack and idle before returning
            if (parts->count < 4) {
                uart_cli_send("ERROR: Usage: GRBL MOVE <X> <Y> [FEEDRATE] [TIMEOUT_MS]\r\n");
                goto api_response;
            }

            float x = atof(parts->parts[2]);
            float y = atof(parts->parts[3]);
            float feedrate = 300.0f;  // Default 300 mm/min (5mm/sec)
            uint32_t timeout_ms = 30000;  // Default 30 second timeout
            if (parts->count >= 5) {
                feedrate = atof(parts->parts[4]);
            }
            if (parts->count >= 6) {
                timeout_ms = atoi(parts->parts[5]);
            }

            if (grbl_move_absolute_sync(x, y, feedrate, timeout_ms)) {
                uart_cli_printf("OK: Move to X=%.3f Y=%.3f complete\r\n", x, y);
            } else {
                api_error("ERROR: Move failed or timeout\r\n");
            }
        } else if (strcmp(parts->parts[1], "STEP") == 0) {
            // Synchronous step - waits for ack and idle before returning
            if (parts->count < 4) {
                uart_cli_send("ERROR: Usage: GRBL STEP <DX> <DY> [FEEDRATE] [TIMEOUT_MS]\r\n");
                goto api_response;
            }

            float dx = atof(parts->parts[2]);
            float dy = atof(parts->parts[3]);
            float feedrate = 300.0f;  // Default 300 mm/min (5mm/sec)
            uint32_t timeout_ms = 30000;  // Default 30 second timeout
            if (parts->count >= 5) {
                feedrate = atof(parts->parts[4]);
            }
            if (parts->count >= 6) {
                timeout_ms = atoi(parts->parts[5]);
            }

            if (grbl_move_relative_sync(dx, dy, feedrate, timeout_ms)) {
                uart_cli_printf("OK: Step DX=%.3f DY=%.3f complete\r\n", dx, dy);
            } else {
                api_error("ERROR: Step failed or timeout\r\n");
            }
        } else if (strcmp(parts->parts[1], "POS") == 0) {
            float x, y, z;
            if (grbl_get_position(&x, &y, &z)) {
                uart_cli_printf("Position: X=%.3f Y=%.3f Z=%.3f\r\n", x, y, z);
            } else {
                api_error("ERROR: Failed to get position\r\n");
            }
        } else if (strcmp(parts->parts[1], "UNLOCK") == 0) {
            // Unlock alarm state (kill alarm lock) to allow movement without homing
            grbl_send("$X");
            uart_cli_send("OK: Alarm unlocked (movement enabled without homing)\r\n");
        } else if (strcmp(parts->parts[1], "SET") == 0) {
            if (parts->count < 3 || strcmp(parts->parts[2], "HOME") != 0) {
                api_error("ERROR: Usage: GRBL SET HOME\r\n");
                goto api_response;
            }
            // Set current position as home (0,0,0) using G92
            grbl_send("G92 X0 Y0 Z0");
            uart_cli_send("OK: Current position set as home (0,0,0)\r\n");
        } else if (strcmp(parts->parts[1], "TEST") == 0) {
            // Test hardware UART loopback
            extern bool grbl_test_loopback(void);
            grbl_test_loopback();
        } else if (strcmp(parts->parts[1], "DEBUG") == 0) {
            // Debug: Check RX FIFO and report status
            extern int grbl_debug_rx_fifo(char *buffer, int max_len);
            char debug_buf[256];
            int count = grbl_debug_rx_fifo(debug_buf, sizeof(debug_buf));
            if (count > 0) {
                uart_cli_printf("RX FIFO has %d bytes:\r\n", count);
                for (int i = 0; i < count; i++) {
                    uart_cli_printf("  [%d] 0x%02X '%c'\r\n", i, debug_buf[i],
                                    (debug_buf[i] >= 32 && debug_buf[i] < 127) ? debug_buf[i] : '.');
                }
            } else {
                uart_cli_send("RX FIFO is empty\r\n");
            }
        } else {
            api_error("ERROR: Unknown GRBL command\r\n");
        }

    } else if (strcmp(parts->parts[0], "SWD") == 0) {
        // SWD debug interface commands
        if (parts->count < 2) {
            uart_cli_send("SWD commands:\r\n");
            uart_cli_send("  SWD CONNECT              - Connect (line reset + JTAG-to-SWD)\r\n");
            uart_cli_send("  SWD CONNECTRST           - Connect under reset (hold nRST, halt)\r\n");
            uart_cli_send("  SWD DISCONNECT           - Disconnect and release pins\r\n");
            uart_cli_send("  SWD FILL <addr|region> <val> [n] - Fill n words with pattern\r\n");
            uart_cli_send("  SWD FLASH ERASE <page>   - Erase flash page\r\n");
            uart_cli_send("  SWD HALT                 - Halt target core\r\n");
            uart_cli_send("  SWD IDCODE               - Identify chip (DPIDR + CPUID + debug ID)\r\n");
            uart_cli_send("  SWD OPT                  - Read option bytes\r\n");
            uart_cli_send("  SWD RDP                  - Read RDP level\r\n");
            uart_cli_send("  SWD RDP SET <0|1>        - Set RDP level\r\n");
            uart_cli_send("  SWD READ <addr> [n]      - Read n words (hex dump)\r\n");
            uart_cli_send("  SWD READ <region> [n]    - Read FLASH|SRAM|BOOTROM (default: full)\r\n");
            uart_cli_send("  SWD READ DP|AP <addr>    - Read debug/access port register\r\n");
            uart_cli_send("  SWD REGS                 - Read core registers (r0-r15, xPSR)\r\n");
            uart_cli_send("  SWD RESET [ms|HOLD|RELEASE] - Target reset via nRST (default 100ms)\r\n");
            uart_cli_send("  SWD RESUME               - Resume target core\r\n");
            uart_cli_send("  SWD SETREG <reg> <val>   - Write core register (r0-r12,sp,lr,pc,...)\r\n");
            uart_cli_send("  SWD SPEED [us]               - Get/set clock delay (0=max, default 1)\r\n");
            uart_cli_send("  SWD WRITE <addr|region> <val> - Write memory (auto-verify)\r\n");
            uart_cli_send("  SWD WRITE DP|AP <addr> <val> - Write debug/access port register\r\n");
            uart_cli_send("  SWD BPTEST               - Run FPB breakpoint self-test\r\n");
            goto api_response;
        }

        // Auto-connect for all SWD commands except CONNECT, CONNECTRST, DISCONNECT
        if (strcmp(parts->parts[1], "CONNECT") != 0 &&
            strcmp(parts->parts[1], "CONNECTRST") != 0 &&
            strcmp(parts->parts[1], "DISCONNECT") != 0 &&
            strcmp(parts->parts[1], "BPTEST") != 0 &&
            strcmp(parts->parts[1], "SPEED") != 0) {
            if (!swd_ensure_connected()) {
                api_error("ERROR: SWD connection failed (check target power and wiring)\r\n");
                goto api_response;
            }
            // Clear any sticky errors from previous commands
            swd_clear_errors();
        }

        if (strcmp(parts->parts[1], "CONNECT") == 0) {
            if (swd_connect()) {
                uint32_t dpidr;
                swd_read_dp(DP_DPIDR, &dpidr);
                uart_cli_printf("OK: Connected, DPIDR=0x%08X\r\n", dpidr);
            } else {
                api_error("ERROR: SWD connect failed (check target power and wiring)\r\n");
            }

        } else if (strcmp(parts->parts[1], "CONNECTRST") == 0) {
            if (swd_connect_under_reset()) {
                uart_cli_send("OK: Connected under reset, core halted\r\n");
            } else {
                api_error("ERROR: Connect-under-reset failed\r\n");
            }

        } else if (strcmp(parts->parts[1], "DISCONNECT") == 0) {
            swd_deinit();
            uart_cli_send("OK: SWD disconnected (pins high-Z)\r\n");

        } else if (strcmp(parts->parts[1], "SPEED") == 0) {
            if (parts->count >= 3) {
                uint32_t delay = strtoul(parts->parts[2], NULL, 0);
                swd_set_speed(delay);
                if (delay == 0) {
                    uart_cli_send("OK: SWD speed set to 0 (MAX, no delay)\r\n");
                } else {
                    uart_cli_printf("OK: SWD speed set to %lu us (~%lu kHz)\r\n",
                                    delay, 1000 / (2 * delay));
                }
            } else {
                uint32_t delay = swd_get_speed();
                if (delay == 0) {
                    uart_cli_send("SWD clock delay: 0 (MAX, no delay)\r\n");
                } else {
                    uart_cli_printf("SWD clock delay: %lu us (~%lu kHz)\r\n",
                                    delay, 1000 / (2 * delay));
                }
            }

        } else if (strcmp(parts->parts[1], "IDCODE") == 0) {
            // Auto-connect already ran; just read DPIDR
            uint32_t dpidr;
            if (!swd_read_dp(DP_DPIDR, &dpidr) || dpidr == 0) {
                api_error("ERROR: Could not read DPIDR\r\n");
                goto api_response;
            }
            uart_cli_printf("DPIDR:    0x%08X\r\n", dpidr);
            uint16_t designer = (dpidr >> 1) & 0x7FF;
            uart_cli_printf("  Designer: 0x%03X%s, PartNo: 0x%02X, Rev: %u, Ver: %u\r\n",
                           designer, designer == 0x23B ? " (ARM)" : "",
                           (dpidr >> 20) & 0xFF, (dpidr >> 28) & 0xF, (dpidr >> 12) & 0xF);

            uint32_t cpuid = 0, dbg_id = 0;
            if (swd_detect(&cpuid, &dbg_id)) {
                uart_cli_printf("CPUID:    0x%08X\r\n", cpuid);
                uint8_t impl = (cpuid >> 24) & 0xFF;
                uint8_t var = (cpuid >> 20) & 0xF;
                uint16_t part = (cpuid >> 4) & 0xFFF;
                uint8_t rev = cpuid & 0xF;
                uart_cli_printf("  Implementer: 0x%02X%s\r\n", impl,
                               impl == 0x41 ? " (ARM)" : "");
                const char *core = "Unknown";
                if (part == 0xC23) core = "Cortex-M3";
                else if (part == 0xC24) core = "Cortex-M4";
                else if (part == 0xC27) core = "Cortex-M7";
                else if (part == 0xC60) core = "Cortex-M0+";
                uart_cli_printf("  Core: %s r%up%u\r\n", core, var, rev);

                uart_cli_printf("DBG_ID:   0x%08X\r\n", dbg_id);
                uint16_t dev_id = dbg_id & 0xFFF;
                uint16_t rev_id = (dbg_id >> 16) & 0xFFFF;
                uart_cli_printf("  DEV_ID: 0x%03X, REV_ID: 0x%04X\r\n", dev_id, rev_id);

                // Try to identify known STM32 devices
                const char *chip = "Unknown";
                switch (dev_id) {
                    case 0x410: chip = "STM32F1 Medium-density"; break;
                    case 0x412: chip = "STM32F1 Low-density"; break;
                    case 0x414: chip = "STM32F1 High-density"; break;
                    case 0x430: chip = "STM32F1 XL-density"; break;
                    case 0x438: chip = "STM32F334"; break;
                    case 0x413: chip = "STM32F405/F407/F415/F417"; break;
                    case 0x419: chip = "STM32F42x/F43x"; break;
                    case 0x421: chip = "STM32F446"; break;
                    case 0x423: chip = "STM32F401xB/C"; break;
                    case 0x433: chip = "STM32F401xD/E"; break;
                    case 0x415: chip = "STM32L47x/L48x"; break;
                    case 0x435: chip = "STM32L43x/L44x"; break;
                    case 0x462: chip = "STM32L45x/L46x"; break;
                    case 0x464: chip = "STM32L41x/L42x"; break;
                    case 0x461: chip = "STM32L496/L4A6"; break;
                }
                uart_cli_printf("  Chip: %s\r\n", chip);

                // Auto-set TARGET based on DEV_ID
                extern void target_set_type(target_type_t type);
                target_type_t auto_tt = TARGET_NONE;
                switch (dev_id) {
                    case 0x410: case 0x412: case 0x414: case 0x430:
                        auto_tt = TARGET_STM32F1; break;
                    case 0x438:
                        auto_tt = TARGET_STM32F3; break;
                    case 0x413: case 0x419: case 0x421: case 0x423: case 0x433:
                        auto_tt = TARGET_STM32F4; break;
                    case 0x415: case 0x435: case 0x462: case 0x464: case 0x461:
                        auto_tt = TARGET_STM32L4; break;
                }
                if (auto_tt != TARGET_NONE) {
                    target_set_type(auto_tt);
                    const stm32_target_info_t *ai = stm32_get_target_info(auto_tt);
                    uart_cli_printf("  TARGET auto-set to %s\r\n", ai->name);
                }
            } else {
                api_error("ERROR: Could not read CPUID/debug registers\r\n");
            }

        } else if (strcmp(parts->parts[1], "HALT") == 0) {
            if (swd_halt()) {
                uart_cli_send("OK: Target halted\r\n");
            } else {
                api_error("ERROR: Halt failed\r\n");
            }

        } else if (strcmp(parts->parts[1], "RESUME") == 0) {
            if (swd_resume()) {
                uart_cli_send("OK: Target resumed\r\n");
            } else {
                api_error("ERROR: Resume failed\r\n");
            }

        } else if (strcmp(parts->parts[1], "REGS") == 0) {
            // Must halt to read registers
            swd_halt();
            sleep_ms(10);
            const char *reg_names[] = {
                "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
                "xPSR", "MSP", "PSP"
            };
            for (int i = 0; i <= 18; i++) {
                uint32_t val;
                if (swd_read_core_reg(i, &val)) {
                    uart_cli_printf("  %-5s = 0x%08X\r\n", reg_names[i], val);
                } else {
                    uart_cli_printf("  %-5s = <read failed>\r\n", reg_names[i]);
                }
            }

        } else if (strcmp(parts->parts[1], "SETREG") == 0) {
            // SWD SETREG <reg> <value> — write core register via DCRSR/DCRDR
            // reg: 0-12=r0-r12, 13=sp, 14=lr, 15=pc, 16=xPSR, 17=MSP, 18=PSP
            // Also accepts names: r0-r12, sp, lr, pc, xpsr, msp, psp
            if (parts->count < 4) {
                api_error("ERROR: Usage: SWD SETREG <reg|name> <value>\r\n");
                goto api_response;
            }
            int reg_num = -1;
            const char *rarg = parts->parts[2];
            // Try register name first
            const char *rnames[] = {"r0","r1","r2","r3","r4","r5","r6","r7",
                                    "r8","r9","r10","r11","r12","sp","lr","pc",
                                    "xpsr","msp","psp"};
            for (int i = 0; i < 19; i++) {
                if (strcasecmp(rarg, rnames[i]) == 0) { reg_num = i; break; }
            }
            if (reg_num < 0) {
                reg_num = (int)strtoul(rarg, NULL, 0);
                if (reg_num > 18) {
                    api_error("ERROR: Register must be 0-18 or name (r0-r12,sp,lr,pc,xpsr,msp,psp)\r\n");
                    goto api_response;
                }
            }
            uint32_t val = strtoul(parts->parts[3], NULL, 16);
            if (swd_write_core_reg(reg_num, val)) {
                uart_cli_printf("OK: %s = 0x%08X\r\n", rnames[reg_num], val);
            } else {
                api_error("ERROR: Failed to write register\r\n");
            }

        } else if (strcmp(parts->parts[1], "READ") == 0) {
            if (parts->count < 3) {
                api_error("ERROR: Usage: SWD READ <addr|AP|BOOTROM|DP|FLASH|MEM|SRAM> [count]\r\n");
                goto api_response;
            }

            // Resolve operation type: DP=0, AP=1, MEM=2
            enum { SWD_DP, SWD_AP, SWD_MEM } op;
            uint32_t addr;
            int count_arg_idx;  // which parts[] index holds the optional count

            uint32_t alias_addr;
            uint32_t alias_size = 0;  // bytes; 0 = not an alias
            if (is_mem_alias(parts->parts[2])) {
                // SWD READ FLASH [count]
                if (!resolve_mem_alias(parts->parts[2], &alias_addr, &alias_size))
                    goto api_response;
                op = SWD_MEM;
                addr = alias_addr;
                count_arg_idx = 3;
            } else if (strcmp(parts->parts[2], "MEM") == 0 && parts->count >= 4
                       && is_mem_alias(parts->parts[3])) {
                // SWD READ MEM FLASH [count]
                if (!resolve_mem_alias(parts->parts[3], &alias_addr, &alias_size))
                    goto api_response;
                op = SWD_MEM;
                addr = alias_addr;
                count_arg_idx = 4;
            } else if (strcmp(parts->parts[2], "DP") == 0) {
                if (parts->count < 4) { api_error("ERROR: Usage: SWD READ DP <addr>\r\n"); goto api_response; }
                op = SWD_DP;
                if (!parse_u32(parts->parts[3], 16, &addr)) {
                    api_error("ERROR: Invalid address. Usage: SWD READ DP <addr>\r\n");
                    goto api_response;
                }
                count_arg_idx = -1;
            } else if (strcmp(parts->parts[2], "AP") == 0) {
                if (parts->count < 4) { api_error("ERROR: Usage: SWD READ AP <addr>\r\n"); goto api_response; }
                op = SWD_AP;
                if (!parse_u32(parts->parts[3], 16, &addr)) {
                    api_error("ERROR: Invalid address. Usage: SWD READ AP <addr>\r\n");
                    goto api_response;
                }
                count_arg_idx = -1;
            } else if (strcmp(parts->parts[2], "MEM") == 0) {
                if (parts->count < 4) { api_error("ERROR: Usage: SWD READ MEM <addr> [count]\r\n"); goto api_response; }
                op = SWD_MEM;
                if (!parse_u32(parts->parts[3], 16, &addr)) {
                    api_error("ERROR: Invalid address. Usage: SWD READ MEM <addr> [count]\r\n");
                    goto api_response;
                }
                count_arg_idx = 4;
            } else {
                // Treat as raw hex address: SWD READ <addr> [count]
                op = SWD_MEM;
                if (!parse_u32(parts->parts[2], 16, &addr)) {
                    api_error("ERROR: Invalid address. Usage: SWD READ <addr|AP|BOOTROM|DP|FLASH|MEM|SRAM> [count]\r\n");
                    goto api_response;
                }
                count_arg_idx = 3;
            }

            switch (op) {
            case SWD_DP: {
                uint32_t value;
                if (swd_read_dp(addr & 0xC, &value))
                    uart_cli_printf("OK: DP[0x%X] = 0x%08X\r\n", addr & 0xC, value);
                else
                    api_error_printf("ERROR: DP read failed (ACK=0x%X)\r\n", swd_get_last_ack());
                break;
            }
            case SWD_AP: {
                uint32_t value;
                if (swd_read_ap(0, addr, &value))
                    uart_cli_printf("OK: AP[0x%02X] = 0x%08X\r\n", addr, value);
                else
                    api_error_printf("ERROR: AP read failed (ACK=0x%X)\r\n", swd_get_last_ack());
                break;
            }
            case SWD_MEM: {
                // Determine byte count
                uint32_t bytes;
                if (count_arg_idx >= 0 && parts->count > (uint32_t)count_arg_idx) {
                    uint32_t cnt;
                    if (!parse_u32(parts->parts[count_arg_idx], 0, &cnt)) {
                        api_error("ERROR: Invalid count\r\n");
                        goto api_response;
                    }
                    bytes = cnt * 4;
                } else if (alias_size > 0) {
                    bytes = alias_size;
                } else {
                    bytes = 4;  // default: 1 word
                }
                uint32_t words = (bytes + 3) / 4;

                uart_cli_printf("Reading %u bytes from 0x%08X:\r\n", bytes, addr);
                uint32_t buf[64];
                bool had_error = false;
                for (uint32_t offset = 0; offset < words; offset += 64) {
                    uint32_t chunk = words - offset;
                    if (chunk > 64) chunk = 64;
                    uint32_t nread = swd_read_mem(addr + offset * 4, buf, chunk);
                    if (nread == 0) {
                        // Try to recover: clear errors and retry once
                        swd_clear_errors();
                        nread = swd_read_mem(addr + offset * 4, buf, chunk);
                    }
                    if (nread == 0) {
                        api_error_printf("ERROR: Read failed at 0x%08X (ACK=0x%X)\r\n",
                                         addr + offset * 4, swd_get_last_ack());
                        had_error = true;
                        break;
                    }
                    // Hex dump: 16 bytes per line with ASCII
                    uint8_t *p = (uint8_t *)buf;
                    for (uint32_t i = 0; i < nread * 4 && (offset * 4 + i) < bytes; i += 16) {
                        uint32_t line_addr = addr + offset * 4 + i;
                        uart_cli_printf("0x%08X:", line_addr);
                        for (uint32_t j = i; j < i + 16 && (offset * 4 + j) < bytes; j++) {
                            if (j < nread * 4)
                                uart_cli_printf(" %02X", p[j]);
                            else
                                uart_cli_send("   ");
                        }
                        uart_cli_send("  ");
                        for (uint32_t j = i; j < i + 16 && (offset * 4 + j) < bytes; j++) {
                            char c = (j < nread * 4) ? p[j] : '.';
                            if (c < 32 || c > 126) c = '.';
                            uart_cli_printf("%c", c);
                        }
                        uart_cli_send("\r\n");
                    }
                }
                if (!had_error)
                    uart_cli_send("OK: Read complete\r\n");
                break;
            }
            }

        } else if (strcmp(parts->parts[1], "WRITE") == 0) {
            if (parts->count < 4) {
                api_error("ERROR: Usage: SWD WRITE <addr|AP|BOOTROM|DP|FLASH|MEM|SRAM> <value>\r\n");
                goto api_response;
            }

            // Resolve operation type
            enum { SWD_WR_DP, SWD_WR_AP, SWD_WR_MEM } op;
            uint32_t addr;
            int val_arg_idx;  // which parts[] index holds the value/data

            uint32_t alias_addr;
            if (is_mem_alias(parts->parts[2])) {
                if (!resolve_mem_alias(parts->parts[2], &alias_addr, NULL))
                    goto api_response;
                op = SWD_WR_MEM;
                addr = alias_addr;
                val_arg_idx = 3;
            } else if (strcmp(parts->parts[2], "MEM") == 0 && parts->count >= 5
                       && is_mem_alias(parts->parts[3])) {
                if (!resolve_mem_alias(parts->parts[3], &alias_addr, NULL))
                    goto api_response;
                op = SWD_WR_MEM;
                addr = alias_addr;
                val_arg_idx = 4;
            } else if (strcmp(parts->parts[2], "DP") == 0) {
                if (parts->count < 5) { api_error("ERROR: Usage: SWD WRITE DP <addr> <value>\r\n"); goto api_response; }
                op = SWD_WR_DP;
                if (!parse_u32(parts->parts[3], 16, &addr)) {
                    api_error("ERROR: Invalid address. Usage: SWD WRITE DP <addr> <value>\r\n");
                    goto api_response;
                }
                val_arg_idx = 4;
            } else if (strcmp(parts->parts[2], "AP") == 0) {
                if (parts->count < 5) { api_error("ERROR: Usage: SWD WRITE AP <addr> <value>\r\n"); goto api_response; }
                op = SWD_WR_AP;
                if (!parse_u32(parts->parts[3], 16, &addr)) {
                    api_error("ERROR: Invalid address. Usage: SWD WRITE AP <addr> <value>\r\n");
                    goto api_response;
                }
                val_arg_idx = 4;
            } else if (strcmp(parts->parts[2], "MEM") == 0) {
                if (parts->count < 5) { api_error("ERROR: Usage: SWD WRITE MEM <addr> <value>\r\n"); goto api_response; }
                op = SWD_WR_MEM;
                if (!parse_u32(parts->parts[3], 16, &addr)) {
                    api_error("ERROR: Invalid address. Usage: SWD WRITE MEM <addr> <value>\r\n");
                    goto api_response;
                }
                val_arg_idx = 4;
            } else {
                // Treat as raw hex address: SWD WRITE <addr> <value>
                op = SWD_WR_MEM;
                if (!parse_u32(parts->parts[2], 16, &addr)) {
                    api_error("ERROR: Invalid address. Usage: SWD WRITE <addr|AP|BOOTROM|DP|FLASH|MEM|SRAM> <value>\r\n");
                    goto api_response;
                }
                val_arg_idx = 3;
            }

            switch (op) {
            case SWD_WR_DP: {
                uint32_t value;
                if (!parse_u32(parts->parts[val_arg_idx], 16, &value)) {
                    api_error("ERROR: Invalid value. Usage: SWD WRITE DP <addr> <value>\r\n");
                    goto api_response;
                }
                uart_cli_printf("Writing DP[0x%X] = 0x%08X\r\n", addr & 0xC, value);
                if (swd_write_dp(addr & 0xC, value))
                    uart_cli_printf("OK: DP[0x%X] = 0x%08X\r\n", addr & 0xC, value);
                else
                    api_error_printf("ERROR: DP write failed (ACK=0x%X)\r\n", swd_get_last_ack());
                break;
            }
            case SWD_WR_AP: {
                uint32_t value;
                if (!parse_u32(parts->parts[val_arg_idx], 16, &value)) {
                    api_error("ERROR: Invalid value. Usage: SWD WRITE AP <addr> <value>\r\n");
                    goto api_response;
                }
                uart_cli_printf("Writing AP[0x%02X] = 0x%08X\r\n", addr, value);
                if (swd_write_ap(0, addr, value))
                    uart_cli_printf("OK: AP[0x%02X] = 0x%08X\r\n", addr, value);
                else
                    api_error_printf("ERROR: AP write failed (ACK=0x%X)\r\n", swd_get_last_ack());
                break;
            }
            case SWD_WR_MEM: {
                uint32_t value;
                if (!parse_u32(parts->parts[val_arg_idx], 16, &value)) {
                    api_error("ERROR: Invalid value (max 8 hex digits). Usage: SWD WRITE <addr> <value>\r\n");
                    goto api_response;
                }
                uart_cli_printf("Writing [0x%08X] = 0x%08X\r\n", addr, value);

                // Auto-detect flash address and use flash controller
                if (addr >= 0x08000000 && addr < 0x08100000) {
                    extern target_type_t target_get_type(void);
                    target_type_t tt = target_get_type();
                    if (!target_is_stm32(tt)) {
                        if (!swd_auto_detect_target()) {
                            api_error("ERROR: Flash write needs TARGET set\r\n");
                            goto api_response;
                        }
                        tt = target_get_type();
                    }
                    const stm32_target_info_t *info = stm32_get_target_info(tt);

                    // Require ERASE keyword for flash writes
                    bool do_erase = false;
                    if ((uint32_t)(val_arg_idx + 1) < parts->count &&
                        strcmp(parts->parts[val_arg_idx + 1], "ERASE") == 0) {
                        do_erase = true;
                    }

                    if (!do_erase) {
                        uint32_t page = (addr - 0x08000000) / info->page_size;
                        uart_cli_printf("Address 0x%08X is flash (page %u, %u bytes).\r\n",
                                        addr, page, info->page_size);
                        uart_cli_send("Flash writes require page erase first.\r\n");
                        uart_cli_send("Append ERASE to confirm: SWD WRITE <addr> <val> ERASE\r\n");
                        goto api_response;
                    }

                    uint32_t page = (addr - 0x08000000) / info->page_size;
                    uart_cli_printf("Erasing page %u...\r\n", page);
                    if (!swd_stm32_flash_erase_page(info, page)) {
                        api_error("ERROR: Flash erase failed\r\n");
                        goto api_response;
                    }

                    if (!swd_stm32_flash_unlock(info)) {
                        api_error("ERROR: Flash unlock failed\r\n");
                        goto api_response;
                    }
                    uint8_t data[4] = { value & 0xFF, (value >> 8) & 0xFF,
                                        (value >> 16) & 0xFF, (value >> 24) & 0xFF };
                    uint32_t written = swd_stm32_flash_write(info, addr, data, 4);
                    swd_clear_errors();
                    uint32_t readback;
                    if (written == 4 && swd_read_mem(addr, &readback, 1) > 0) {
                        if (readback == value)
                            uart_cli_printf("OK: [0x%08X] = 0x%08X (verified)\r\n", addr, value);
                        else
                            api_error_printf("ERROR: Verify failed at 0x%08X: wrote 0x%08X, read 0x%08X\r\n",
                                             addr, value, readback);
                    } else {
                        api_error_printf("ERROR: Flash write failed at 0x%08X\r\n", addr);
                    }
                    break;
                }

                uint32_t written = swd_write_mem(addr, &value, 1);
                // Clear any sticky errors before verify read
                swd_clear_errors();
                uint32_t readback;
                if (written > 0 && swd_read_mem(addr, &readback, 1) > 0) {
                    if (readback == value)
                        uart_cli_printf("OK: [0x%08X] = 0x%08X (verified)\r\n", addr, value);
                    else
                        api_error_printf("ERROR: Verify failed at 0x%08X: wrote 0x%08X, read 0x%08X\r\n",
                                         addr, value, readback);
                } else {
                    api_error_printf("ERROR: Write failed at 0x%08X (ACK=0x%X)\r\n", addr, swd_get_last_ack());
                }
                break;
            }
            }

        } else if (strcmp(parts->parts[1], "FILL") == 0) {
            // SWD FILL <addr|FLASH|SRAM|BOOTROM> <value> [words]
            if (parts->count < 4) {
                api_error("ERROR: Usage: SWD FILL <addr|FLASH|SRAM|BOOTROM> <value> [words]\r\n");
                goto api_response;
            }
            uint32_t addr;
            uint32_t alias_size = 0;
            int val_idx, count_idx;

            if (is_mem_alias(parts->parts[2])) {
                if (!resolve_mem_alias(parts->parts[2], &addr, &alias_size))
                    goto api_response;
                val_idx = 3;
                count_idx = 4;
            } else {
                if (!parse_u32(parts->parts[2], 16, &addr)) {
                    api_error("ERROR: Invalid address. Usage: SWD FILL <addr> <value> <words>\r\n");
                    goto api_response;
                }
                val_idx = 3;
                count_idx = 4;
                if (parts->count < 5) {
                    api_error("ERROR: Usage: SWD FILL <addr> <value> <words>\r\n");
                    goto api_response;
                }
            }

            uint32_t pattern;
            if (!parse_u32(parts->parts[val_idx], 16, &pattern)) {
                api_error("ERROR: Invalid pattern (max 8 hex digits). Usage: SWD FILL <addr> <value> [words]\r\n");
                goto api_response;
            }
            // Parse optional word count — skip ERASE keyword
            uint32_t words = 0;
            bool have_count = false;
            if ((uint32_t)count_idx < parts->count &&
                strcmp(parts->parts[count_idx], "ERASE") != 0) {
                if (!parse_u32(parts->parts[count_idx], 0, &words)) {
                    api_error("ERROR: Invalid word count\r\n");
                    goto api_response;
                }
                have_count = true;
            }
            if (!have_count) {
                if (alias_size > 0)
                    words = alias_size / 4;
                else
                    words = 1;
            }
            if (words == 0) words = 1;

            uart_cli_printf("Filling %u words at 0x%08X with pattern 0x%08X\r\n", words, addr, pattern);

            // Flash fill: needs erase + flash controller
            if (addr >= 0x08000000 && addr < 0x08100000) {
                // Check for ERASE keyword
                bool has_erase = false;
                for (uint32_t i = val_idx + 1; i < parts->count; i++) {
                    if (strcmp(parts->parts[i], "ERASE") == 0) { has_erase = true; break; }
                }
                if (!has_erase) {
                    uint32_t bytes = words * 4;
                    extern target_type_t target_get_type(void);
                    target_type_t tt = target_get_type();
                    if (!target_is_stm32(tt)) swd_auto_detect_target();
                    tt = target_get_type();
                    const stm32_target_info_t *info = stm32_get_target_info(tt);
                    uint32_t pages = info ? (bytes + info->page_size - 1) / info->page_size : 0;
                    uart_cli_printf("Fill covers %u bytes of flash (%u pages).\r\n", bytes, pages);
                    uart_cli_send("Flash fill requires page erase first.\r\n");
                    uart_cli_send("Append ERASE to confirm: SWD FILL <target> <val> [n] ERASE\r\n");
                    goto api_response;
                }

                extern target_type_t target_get_type(void);
                target_type_t tt = target_get_type();
                if (!target_is_stm32(tt)) {
                    if (!swd_auto_detect_target()) {
                        api_error("ERROR: Flash fill needs TARGET set\r\n");
                        goto api_response;
                    }
                    tt = target_get_type();
                }
                const stm32_target_info_t *info = stm32_get_target_info(tt);

                // Erase all pages covered by the fill
                uint32_t bytes = words * 4;
                uint32_t first_page = (addr - 0x08000000) / info->page_size;
                uint32_t last_page = (addr + bytes - 1 - 0x08000000) / info->page_size;
                for (uint32_t p = first_page; p <= last_page; p++) {
                    uart_cli_printf("Erasing page %u...\r\n", p);
                    if (!swd_stm32_flash_erase_page(info, p)) {
                        api_error_printf("ERROR: Flash erase failed on page %u\r\n", p);
                        goto api_response;
                    }
                }

                // Write pattern via flash controller
                if (!swd_stm32_flash_unlock(info)) {
                    api_error("ERROR: Flash unlock failed\r\n");
                    goto api_response;
                }

                uart_cli_printf("Programming %u words...\r\n", words);
                uint8_t pat_bytes[4] = { pattern & 0xFF, (pattern >> 8) & 0xFF,
                                         (pattern >> 16) & 0xFF, (pattern >> 24) & 0xFF };
                // Build a small page buffer and write page-by-page
                uint8_t page_buf[2048];
                for (uint32_t i = 0; i < sizeof(page_buf); i += 4) {
                    page_buf[i] = pat_bytes[0]; page_buf[i+1] = pat_bytes[1];
                    page_buf[i+2] = pat_bytes[2]; page_buf[i+3] = pat_bytes[3];
                }

                uint32_t written_total = 0;
                bool had_error = false;
                uint32_t remaining = bytes;
                uint32_t cur_addr = addr;
                while (remaining > 0) {
                    uint32_t chunk = remaining;
                    if (chunk > info->page_size) chunk = info->page_size;
                    uint32_t w = swd_stm32_flash_write(info, cur_addr, page_buf, chunk);
                    written_total += w;
                    if (w < chunk) {
                        api_error_printf("ERROR: Flash write failed at 0x%08X, wrote %u/%u bytes\r\n",
                                         cur_addr + w, written_total, bytes);
                        had_error = true;
                        break;
                    }
                    cur_addr += chunk;
                    remaining -= chunk;
                }
                if (!had_error) {
                    // Spot-check verify
                    swd_clear_errors();
                    uint32_t v0, v1;
                    bool v_ok = true;
                    if (swd_read_mem(addr, &v0, 1) > 0 && v0 != pattern) v_ok = false;
                    if (words > 1 && swd_read_mem(addr + (words - 1) * 4, &v1, 1) > 0 && v1 != pattern) v_ok = false;
                    if (v_ok)
                        uart_cli_printf("OK: Filled %u words (%u bytes) verified\r\n", words, bytes);
                    else
                        api_error_printf("ERROR: Fill verify failed\r\n");
                }
                goto api_response;
            }

            // RAM fill: direct write
            uint32_t buf[64];
            for (uint32_t i = 0; i < 64; i++) buf[i] = pattern;

            uart_cli_printf("Filling %u words at 0x%08X with 0x%08X...\r\n", words, addr, pattern);
            uint32_t written_total = 0;
            bool had_error = false;
            for (uint32_t offset = 0; offset < words; offset += 64) {
                uint32_t chunk = words - offset;
                if (chunk > 64) chunk = 64;
                uint32_t nw = swd_write_mem(addr + offset * 4, buf, chunk);
                if (nw < chunk) {
                    swd_clear_errors();
                    uint32_t nw2 = swd_write_mem(addr + (offset + nw) * 4, buf, chunk - nw);
                    nw += nw2;
                }
                written_total += nw;
                if (nw < chunk) {
                    api_error_printf("ERROR: Write failed at 0x%08X (ACK=0x%X), wrote %u/%u words\r\n",
                                     addr + (offset + nw) * 4, swd_get_last_ack(),
                                     written_total, words);
                    had_error = true;
                    break;
                }
            }
            if (!had_error) {
                swd_clear_errors();
                uint32_t v0, v1;
                bool v_ok = true;
                if (swd_read_mem(addr, &v0, 1) > 0 && v0 != pattern) v_ok = false;
                if (words > 1 && swd_read_mem(addr + (words - 1) * 4, &v1, 1) > 0 && v1 != pattern) v_ok = false;
                if (v_ok)
                    uart_cli_printf("OK: Filled %u words (%u bytes) verified\r\n", written_total, written_total * 4);
                else
                    api_error_printf("ERROR: Fill verify failed (wrote %u words but readback mismatch)\r\n", written_total);
            }

        } else if (strcmp(parts->parts[1], "RDP") == 0) {
            // SWD RDP [SET <0|1>]
            extern target_type_t target_get_type(void);
            target_type_t tt = target_get_type();
            if (!target_is_stm32(tt)) {
                if (!swd_auto_detect_target()) {
                    api_error("ERROR: Could not auto-detect STM32 family. Set TARGET manually.\r\n");
                    goto api_response;
                }
                tt = target_get_type();
            }
            const stm32_target_info_t *info = stm32_get_target_info(tt);

            if (parts->count >= 4 && strcmp(parts->parts[2], "SET") == 0) {
                uint8_t level = atoi(parts->parts[3]);
                if (level > 2) {
                    api_error("ERROR: RDP level must be 0, 1, or 2\r\n");
                    goto api_response;
                }
                if (level == 2) {
                    api_error("ERROR: RDP Level 2 is PERMANENT and irreversible. Refusing.\r\n");
                    goto api_response;
                }
                if (level == 0) {
                    // Require safe word "WIPE" to confirm mass erase
                    if (parts->count < 5 || strcmp(parts->parts[4], "WIPE") != 0) {
                        uart_cli_send("WARNING: Setting RDP to 0 triggers MASS ERASE of all flash!\r\n");
                        uart_cli_send("This will PERMANENTLY DESTROY all data on the target.\r\n");
                        uart_cli_send("To confirm, append WIPE: SWD RDP SET 0 WIPE\r\n");
                        goto api_response;
                    }
                }
                uart_cli_printf("Setting RDP to Level %u on %s...\r\n", level, info->name);
                if (swd_stm32_set_rdp(info, level)) {
                    uart_cli_printf("OK: RDP set to Level %u (power cycle target to apply)\r\n", level);
                } else {
                    api_error("ERROR: Failed to set RDP\r\n");
                }
            } else if (parts->count >= 3) {
                // User typed something like "SWD RDP 1" — suggest correct syntax
                uart_cli_send("ERROR: Unknown RDP subcommand. Did you mean:\r\n");
                uart_cli_send("  SWD RDP          - Read current RDP level\r\n");
                uart_cli_send("  SWD RDP SET <0|1> - Set RDP level\r\n");
            } else {
                int rdp = swd_stm32_read_rdp(info);
                if (rdp >= 0) {
                    uart_cli_printf("OK: RDP Level %d on %s\r\n", rdp, info->name);
                } else {
                    api_error("ERROR: Could not read RDP status\r\n");
                }
            }

        } else if (strcmp(parts->parts[1], "OPT") == 0) {
            // SWD OPT - read option bytes
            extern target_type_t target_get_type(void);
            target_type_t tt = target_get_type();
            if (!target_is_stm32(tt)) {
                if (!swd_auto_detect_target()) {
                    api_error("ERROR: Could not auto-detect STM32 family. Set TARGET manually.\r\n");
                    goto api_response;
                }
                tt = target_get_type();
            }
            const stm32_target_info_t *info = stm32_get_target_info(tt);
            if (!swd_stm32_read_options(info)) {
                api_error("ERROR: Could not read option bytes\r\n");
            }

        } else if (strcmp(parts->parts[1], "FLASH") == 0) {
            // SWD FLASH <ERASE|TEST>
            extern target_type_t target_get_type(void);
            target_type_t tt = target_get_type();
            if (!target_is_stm32(tt)) {
                if (!swd_auto_detect_target()) {
                    api_error("ERROR: Could not auto-detect STM32 family. Set TARGET manually.\r\n");
                    goto api_response;
                }
                tt = target_get_type();
            }
            const stm32_target_info_t *info = stm32_get_target_info(tt);

            if (parts->count < 3) {
                api_error("ERROR: Usage: SWD FLASH ERASE <page>\r\n");
                goto api_response;
            }

            if (strcmp(parts->parts[2], "ERASE") == 0) {
                if (parts->count < 4) {
                    api_error("ERROR: Usage: SWD FLASH ERASE <page>\r\n");
                    goto api_response;
                }
                uint32_t page;
                if (!parse_u32(parts->parts[3], 0, &page)) {
                    api_error("ERROR: Invalid page number. Usage: SWD FLASH ERASE <page>\r\n");
                    goto api_response;
                }
                uart_cli_printf("Erasing page %u on %s...\r\n", page, info->name);
                if (swd_stm32_flash_erase_page(info, page)) {
                    uart_cli_printf("OK: Page %u erased\r\n", page);
                } else {
                    api_error("ERROR: Page erase failed\r\n");
                }
            } else {
                api_error("ERROR: Usage: SWD FLASH ERASE <page>\r\n");
            }

        } else if (strcmp(parts->parts[1], "RESET") == 0) {
            if (parts->count >= 3 && strcmp(parts->parts[2], "HOLD") == 0) {
                swd_nrst_assert();
                uart_cli_send("OK: nRST asserted (target held in reset)\r\n");
            } else if (parts->count >= 3 && strcmp(parts->parts[2], "RELEASE") == 0) {
                swd_nrst_release();
                uart_cli_send("OK: nRST released\r\n");
            } else {
                uint32_t ms = 100;
                if (parts->count >= 3) {
                    if (!parse_u32(parts->parts[2], 0, &ms)) {
                        api_error("ERROR: Invalid duration. Usage: SWD RESET [ms|HOLD|RELEASE]\r\n");
                        goto api_response;
                    }
                }
                if (ms < 1) ms = 1;
                if (ms > 10000) ms = 10000;
                swd_nrst_pulse(ms);
                uart_cli_printf("OK: nRST pulsed for %u ms\r\n", (unsigned)ms);
            }

        } else if (strcmp(parts->parts[1], "BPTEST") == 0) {
            // Test breakpoints on SRAM code: BKPT instruction, DWT PC-match,
            // and boot ROM equivalent timing with ADC shunt.
            // Strategy: normal connect + halt (not reset) so IWDG doesn't interfere.
            uart_cli_send("=== SWD Breakpoint Test ===\r\n");

            // Helper: connect, halt, freeze watchdogs
            uart_cli_send("[1] Connecting and halting...\r\n");
            if (!swd_connect()) {
                api_error("ERROR: SWD connect failed\r\n");
                goto api_response;
            }
            swd_clear_errors();
            if (!swd_halt()) {
                api_error("ERROR: Halt failed\r\n");
                goto api_response;
            }
            // Freeze watchdogs
            uint32_t dbg_cr;
            swd_read_mem(0xE0042004, &dbg_cr, 1);
            dbg_cr |= (1u << 8) | (1u << 9);
            swd_write_mem(0xE0042004, &dbg_cr, 1);

            uint32_t dhcsr;
            swd_read_mem(0xE000EDF0, &dhcsr, 1);
            uart_cli_printf("    DHCSR = 0x%08X\r\n", dhcsr);

            uint32_t pc_init;
            if (swd_read_core_reg(15, &pc_init))
                uart_cli_printf("    PC = 0x%08X\r\n", pc_init);

            uint32_t pc_check, func0;

            // ============================================================
            // Part 1: Software BKPT instruction test
            // ============================================================
            uart_cli_send("\r\n--- Part 1: Software BKPT instruction ---\r\n");
            {
                // Code at 0x20004000:
                //   mov r0, #0       ; 0x2000
                //   bkpt #0          ; 0xBE00  <- should halt here
                //   add r0, #1       ; 0x3001
                //   b .-4            ; 0xE7FC
                uint32_t bkpt_code[2] = { 0xBE002000, 0xE7FC3001 };
                swd_write_mem(0x20004000, bkpt_code, 2);

                // Verify
                uint32_t readback[2];
                swd_read_mem(0x20004000, readback, 2);
                uart_cli_printf("    Code: %08X %08X %s\r\n",
                               readback[0], readback[1],
                               (readback[0] == 0xBE002000) ? "OK" : "FAIL");

                // Set registers
                swd_write_core_reg(13, 0x20005000);  // SP
                swd_write_core_reg(15, 0x20004001);  // PC (Thumb)
                uint32_t xpsr;
                swd_read_core_reg(16, &xpsr);
                xpsr |= (1u << 24);  // T bit
                swd_write_core_reg(16, xpsr);

                // Enable debug halt on BKPT: C_DEBUGEN must be set (it already is)
                // DEMCR: TRCENA + no vector catch
                uint32_t demcr = (1u << 24);
                swd_write_mem(0xE000EDFC, &demcr, 1);

                // Resume
                swd_resume();
                sleep_ms(50);

                // Check halt
                swd_read_mem(0xE000EDF0, &dhcsr, 1);
                bool halted = (dhcsr & (1u << 17)) != 0;
                swd_read_core_reg(15, &pc_check);
                uart_cli_printf("    DHCSR=0x%08X PC=0x%08X\r\n", dhcsr, pc_check);

                if (halted && pc_check == 0x20004002) {
                    uart_cli_send("*** PASS: Software BKPT works ***\r\n");
                } else if (halted) {
                    uart_cli_printf("    Halted but PC=0x%08X (expected 0x20004002)\r\n", pc_check);
                } else {
                    uart_cli_send("    FAIL: Not halted\r\n");
                    swd_halt();
                }
            }

            // Re-halt for next test
            swd_halt();

            // ============================================================
            // Part 2: DWT PC-match test
            // ============================================================
            uart_cli_send("\r\n--- Part 2: DWT PC-match breakpoint ---\r\n");
            {
                // Code at 0x20004000:
                //   mov r0, #0       ; 0x2000
                //   add r0, #1       ; 0x3001  <- DWT breakpoint here (0x20004002)
                //   b .-2            ; 0xE7FD
                //   nop              ; 0xBF00
                uint32_t dwt_code[2] = { 0x30012000, 0xBF00E7FD };
                swd_write_mem(0x20004000, dwt_code, 2);

                swd_write_core_reg(13, 0x20005000);
                swd_write_core_reg(15, 0x20004001);
                uint32_t xpsr;
                swd_read_core_reg(16, &xpsr);
                xpsr |= (1u << 24);
                swd_write_core_reg(16, xpsr);

                // DWT PC-match
                uint32_t demcr = (1u << 24);
                swd_write_mem(0xE000EDFC, &demcr, 1);
                uint32_t bp_addr = 0x20004002;
                swd_write_mem(0xE0001020, &bp_addr, 1);  // COMP0
                uint32_t zero = 0;
                swd_write_mem(0xE0001024, &zero, 1);      // MASK0
                uint32_t dwt_func = 4;                     // PC match
                swd_write_mem(0xE0001028, &dwt_func, 1);  // FUNCTION0

                swd_resume();
                sleep_ms(50);

                swd_read_mem(0xE000EDF0, &dhcsr, 1);
                bool halted = (dhcsr & (1u << 17)) != 0;
                swd_read_core_reg(15, &pc_check);
                swd_read_mem(0xE0001028, &func0, 1);
                uart_cli_printf("    DHCSR=0x%08X PC=0x%08X FUNC0=0x%08X\r\n",
                               dhcsr, pc_check, func0);

                if (halted && pc_check == 0x20004002 && (func0 & (1u << 24))) {
                    uart_cli_send("*** PASS: DWT PC-match works (MATCHED=1) ***\r\n");
                } else {
                    uart_cli_printf("    FAIL: halted=%d MATCHED=%u\r\n",
                                   halted, (func0 >> 24) & 1);
                    swd_halt();
                }
            }

            // Re-halt for next test
            swd_halt();

            // ============================================================
            // Part 3: Boot ROM equivalent — RDP check code in SRAM
            // with DWT_CYCCNT timing + ADC shunt power trace
            //
            // Replicates boot ROM 0x1FFFF132..0x1FFFF13E:
            //   ldr r1, [pc, #N]   ; r1 = FLASH base (0x40022000)
            //   movs r0, #0        ; r0 = 0 (not protected)
            //   ldr r1, [r1, #28]  ; r1 = FLASH_OBR (0x4002201C)
            //   lsls r1, r1, #30   ; shift bit 1 into N flag
            //   bpl .+2            ; if N=0, skip
            //   movs r0, #1        ; r0 = 1 (protected)
            //   bkpt #0            ; halt here ← software breakpoint
            //   b .-14             ; loop back to start
            // ============================================================
            uart_cli_send("\r\n--- Part 3: Boot ROM RDP equivalent + timing ---\r\n");
            {
                // Enable DWT cycle counter
                uint32_t demcr = (1u << 24);
                swd_write_mem(0xE000EDFC, &demcr, 1);

                // Disable any DWT watchpoints from part 2
                uint32_t zero = 0;
                swd_write_mem(0xE0001028, &zero, 1);  // FUNCTION0=0 (disabled)

                // Zero DWT_CYCCNT and enable counter
                swd_write_mem(0xE0001004, &zero, 1);   // CYCCNT = 0
                uint32_t cyccnt_en = 1;
                swd_write_mem(0xE0001000, &cyccnt_en, 1);  // DWT_CTRL.CYCCNTENA

                // Write boot ROM RDP-check equivalent to SRAM at 0x20004000
                // Thumb-2 encoding, little-endian words:
                //
                // 0x20004000: ldr r1, [pc, #16]   ; 0x4908  → loads from 0x20004024
                // 0x20004002: movs r0, #0          ; 0x2000
                // 0x20004004: ldr r1, [r1, #28]    ; 0x69C9  (FLASH_OBR = base+0x1C)
                // 0x20004006: lsls r1, r1, #30     ; 0x0789
                // 0x20004008: bpl .+2               ; 0xD500
                // 0x2000400A: movs r0, #1           ; 0x2001
                // 0x2000400C: bkpt #0               ; 0xBE00  ← halt here
                // 0x2000400E: b 0x20004000          ; 0xE7F7
                //
                // Literal pool at 0x20004024:
                // 0x20004024: 0x40022000            ; FLASH register base
                //
                // Note: ldr r1, [pc, #16] at addr 0x20004000:
                //   PC-relative load: (PC & ~3) + 4 + 16 = 0x20004000 + 4 + 16 = 0x20004014
                //   Wait, that's wrong. Let me recalculate.
                //   For Thumb: effective PC = (addr + 4) & ~3 = 0x20004004 & ~3 = 0x20004004
                //   Offset = 16 → target = 0x20004004 + 16 = 0x20004014
                //   Hmm, let me just put literal right after code at aligned offset.
                //
                // Simpler: put literal at 0x20004010 (word-aligned, right after code)
                //   ldr r1, [pc, #8] at 0x20004000:
                //     effective PC = 0x20004004 (aligned)
                //     target = 0x20004004 + 8 = 0x2000400C — no, that's the bkpt
                //
                // Even simpler: use mov/movt to load 0x40022000 directly.
                //   movw r1, #0x2000     ; 0xF240 0x1000 → F240 1000
                //   movt r1, #0x4002     ; 0xF2C4 0x0102 → F2C4 0102  (imm16=0x4002)
                //   movs r0, #0          ; 0x2000
                //   ldr r1, [r1, #28]    ; 0x69C9
                //   lsls r1, r1, #30     ; 0x0789
                //   bpl .+2              ; 0xD500
                //   movs r0, #1          ; 0x2001
                //   bkpt #0              ; 0xBE00
                //   b start              ; 0xE7F5 (back to movw, 12 halfwords back)
                //   nop                  ; 0xBF00
                //
                // movw encoding: 0xF240 | (imm4<<16) | (i<<26), 0x(imm3<<12)|(rd<<8)|(imm8)
                // movw r1, #0x2000: imm16=0x2000 → imm4=0x2,i=0,imm3=0,imm8=0x00
                //   = F242 1000
                // movt r1, #0x4002: imm16=0x4002 → imm4=0x4,i=0,imm3=0,imm8=0x02
                //   = F2C4 0102
                //
                // Actually Thumb-2 movw/movt encoding is complex. Let me use a
                // simpler approach: put the constant in SRAM and use a PC-relative load.
                //
                // Better approach: just put code + literal pool contiguously.
                // At 0x20004000:
                //   0x00: movs r0, #0          ; 0x2000
                //   0x02: ldr  r1, [pc, #12]   ; 0x4903  → loads from (0x20004006 & ~3) + 12 = 0x20004010
                //   0x04: ldr  r1, [r1, #28]   ; 0x69C9
                //   0x06: lsls r1, r1, #30     ; 0x0789
                //   0x08: bpl  .+2              ; 0xD500
                //   0x0A: movs r0, #1           ; 0x2001
                //   0x0C: bkpt #0               ; 0xBE00
                //   0x0E: b    .-14             ; 0xE7F7 (back to 0x20004000)
                //   0x10: .word 0x40022000      ; literal pool
                //
                // Check: ldr r1, [pc, #12] at 0x20004002:
                //   effective PC = (0x20004002 + 4) & ~3 = 0x20004004
                //   target = 0x20004004 + 12 = 0x20004010  ✓

                uint32_t rdp_code[] = {
                    0x49032000,  // movs r0, #0; ldr r1, [pc, #12]
                    0x078969C9,  // ldr r1, [r1, #28]; lsls r1, r1, #30
                    0x2001D500,  // bpl .+2; movs r0, #1
                    0xE7F7BE00,  // bkpt #0; b .-14
                    0x40022000,  // literal: FLASH register base
                };
                swd_write_mem(0x20004000, rdp_code, 5);

                // Verify
                uint32_t readback[5];
                swd_read_mem(0x20004000, readback, 5);
                bool code_ok = true;
                for (int i = 0; i < 5; i++) {
                    if (readback[i] != rdp_code[i]) code_ok = false;
                }
                uart_cli_printf("    Code written: %s\r\n", code_ok ? "OK" : "FAIL");
                if (!code_ok) {
                    for (int i = 0; i < 5; i++)
                        uart_cli_printf("    [%d] w:0x%08X r:0x%08X\r\n",
                                       i, rdp_code[i], readback[i]);
                }

                // Set SP and PC
                swd_write_core_reg(13, 0x20005000);
                swd_write_core_reg(15, 0x20004001);  // Thumb
                uint32_t xpsr;
                swd_read_core_reg(16, &xpsr);
                xpsr |= (1u << 24);
                swd_write_core_reg(16, xpsr);

                // Zero cycle counter right before resume
                swd_write_mem(0xE0001004, &zero, 1);

                // Resume — BKPT instruction will halt the core
                uart_cli_send("    Resuming...\r\n");
                swd_resume();
                sleep_ms(50);

                // Check halt
                swd_read_mem(0xE000EDF0, &dhcsr, 1);
                bool halted = (dhcsr & (1u << 17)) != 0;

                if (!halted) {
                    swd_halt();
                    swd_read_mem(0xE000EDF0, &dhcsr, 1);
                }

                // Read results
                swd_read_core_reg(15, &pc_check);
                uint32_t r0_val, r1_val;
                swd_read_core_reg(0, &r0_val);
                swd_read_core_reg(1, &r1_val);
                uint32_t cyccnt;
                swd_read_mem(0xE0001004, &cyccnt, 1);

                uart_cli_printf("    DHCSR=0x%08X PC=0x%08X\r\n", dhcsr, pc_check);
                uart_cli_printf("    R0=%u (RDP: 0=off, 1=on) R1=0x%08X\r\n", r0_val, r1_val);
                uart_cli_printf("    DWT_CYCCNT=%lu cycles (%.2fms @ 8MHz)\r\n",
                               cyccnt, (float)cyccnt / 8000.0f);

                if (halted && pc_check == 0x2000400C) {
                    uart_cli_send("*** PASS: Boot ROM RDP equivalent halted at BKPT ***\r\n");
                } else {
                    uart_cli_printf("    Expected PC=0x2000400C, got 0x%08X\r\n", pc_check);
                }
            }

            uart_cli_send("\r\n=== BPTEST complete ===\r\n");

        } else {
            api_error("ERROR: Unknown SWD command\r\n");
        }

    } else if (strcmp(parts->parts[0], "JTAG") == 0) {
        // JTAG debug interface commands
        if (parts->count < 2) {
            uart_cli_send("ERROR: Usage: JTAG <RESET|IDCODE|SCAN|IR|DR|TEST>\r\n");
            uart_cli_send("  JTAG RESET             - Reset TAP state machine\r\n");
            uart_cli_send("  JTAG IDCODE            - Read device IDCODE\r\n");
            uart_cli_send("  JTAG SCAN              - Scan chain for devices\r\n");
            uart_cli_send("  JTAG IR <value> [bits] - Shift IR (default 4 bits)\r\n");
            uart_cli_send("  JTAG DR <value> [bits] - Shift DR (default 32 bits)\r\n");
            uart_cli_send("  JTAG TEST              - Test pin connections and RTCK\r\n");
            goto api_response;
        }

        if (strcmp(parts->parts[1], "RESET") == 0) {
            jtag_reset();
            uart_cli_send("OK: TAP reset to Test-Logic-Reset state\r\n");

        } else if (strcmp(parts->parts[1], "TEST") == 0) {
            // Debug: test JTAG connection
            uart_cli_send("JTAG pins: TRST=GP15, TCK=GP17, TMS=GP18, TDI=GP19, TDO=GP20, RTCK=GP21\r\n");
            uart_cli_printf("Adaptive clocking: %s\r\n",
                           jtag_rtck_available() ? "enabled" : "disabled");
            uart_cli_printf("Current pin states: TDO=%d RTCK=%d\r\n",
                           gpio_get(JTAG_TDO_PIN), gpio_get(JTAG_RTCK_PIN));

            // Use the actual jtag functions to test
            uart_cli_send("Testing IR shift (4 bits, sending IDCODE=0xE)...\r\n");
            uint32_t ir_out = jtag_ir_shift(0xE, 4);  // IDCODE instruction
            uart_cli_printf("IR out: 0x%X\r\n", ir_out);

            uart_cli_send("Testing DR shift (32 bits, sending 0)...\r\n");
            uint32_t dr_out = jtag_dr_shift(0, 32);
            uart_cli_printf("DR out: 0x%08X\r\n", dr_out);
            uart_cli_send("OK\r\n");

        } else if (strcmp(parts->parts[1], "IDCODE") == 0) {
            uint32_t idcode = jtag_read_idcode();
            if (idcode != 0 && idcode != 0xFFFFFFFF) {
                uart_cli_printf("OK: IDCODE = 0x%08X\r\n", idcode);
                // Decode IDCODE fields
                uint8_t version = (idcode >> 28) & 0xF;
                uint16_t part = (idcode >> 12) & 0xFFFF;
                uint16_t manuf = (idcode >> 1) & 0x7FF;
                uart_cli_printf("  Version: %u, Part: 0x%04X, Manufacturer: 0x%03X\r\n",
                               version, part, manuf);
            } else {
                api_error_printf("ERROR: No IDCODE (got 0x%08X)\r\n", idcode);
            }

        } else if (strcmp(parts->parts[1], "SCAN") == 0) {
            uint32_t idcodes[8];
            uint8_t count = jtag_scan_chain(idcodes, 8);
            if (count > 0) {
                uart_cli_printf("OK: Found %u device(s):\r\n", count);
                for (uint8_t i = 0; i < count; i++) {
                    uart_cli_printf("  [%u] IDCODE = 0x%08X\r\n", i, idcodes[i]);
                }
            } else {
                api_error("ERROR: No devices found in JTAG chain\r\n");
            }

        } else if (strcmp(parts->parts[1], "IR") == 0) {
            if (parts->count < 3) {
                api_error("ERROR: Usage: JTAG IR <value> [bits]\r\n");
                goto api_response;
            }
            uint32_t value;
            if (!parse_u32(parts->parts[2], 0, &value)) {
                api_error("ERROR: Invalid value. Usage: JTAG IR <value> [bits]\r\n");
                goto api_response;
            }
            uint32_t bits_val = 4;  // Default for ARM7
            if (parts->count >= 4) {
                if (!parse_u32(parts->parts[3], 0, &bits_val)) {
                    api_error("ERROR: Invalid bits. Usage: JTAG IR <value> [bits]\r\n");
                    goto api_response;
                }
            }
            uint8_t bits = (bits_val > 32) ? 32 : (uint8_t)bits_val;

            uint32_t result = jtag_ir_shift(value, bits);
            uart_cli_printf("OK: IR shift: in=0x%X, out=0x%X (%u bits)\r\n", value, result, bits);

        } else if (strcmp(parts->parts[1], "DR") == 0) {
            if (parts->count < 3) {
                api_error("ERROR: Usage: JTAG DR <value> [bits]\r\n");
                goto api_response;
            }
            uint32_t value;
            if (!parse_u32(parts->parts[2], 0, &value)) {
                api_error("ERROR: Invalid value. Usage: JTAG DR <value> [bits]\r\n");
                goto api_response;
            }
            uint32_t bits_val = 32;  // Default
            if (parts->count >= 4) {
                if (!parse_u32(parts->parts[3], 0, &bits_val)) {
                    api_error("ERROR: Invalid bits. Usage: JTAG DR <value> [bits]\r\n");
                    goto api_response;
                }
            }
            uint8_t bits = (bits_val > 32) ? 32 : (uint8_t)bits_val;

            uint32_t result = jtag_dr_shift(value, bits);
            uart_cli_printf("OK: DR shift: in=0x%08X, out=0x%08X (%u bits)\r\n", value, result, bits);

        } else {
            api_error("ERROR: Unknown JTAG command\r\n");
        }

    } else if (strcmp(parts->parts[0], "ERROR") == 0) {
        // Return last error message
        if (last_error[0] != '\0') {
            uart_cli_send(last_error);
            if (last_error[strlen(last_error)-1] != '\n') {
                uart_cli_send("\r\n");
            }
        } else {
            uart_cli_send("No error recorded\r\n");
        }

    } else if (strcmp(parts->parts[0], "TRACE") == 0) {
        extern void trace_start(uint32_t samples, uint32_t pre_pct);
        extern void trace_stop(void);
        extern void trace_status(void);
        extern void trace_dump(void);

        if (parts->count < 2) {
            trace_status();
        } else if (strcmp(parts->parts[1], "RESET") == 0) {
            trace_stop();
        } else if (strcmp(parts->parts[1], "STATUS") == 0) {
            trace_status();
        } else if (strcmp(parts->parts[1], "DUMP") == 0) {
            trace_dump();
        } else if (strcmp(parts->parts[1], "RATE") == 0) {
            extern void trace_set_clkdiv(uint32_t div);
            if (parts->count >= 3) {
                uint32_t div = 0;
                parse_u32(parts->parts[2], 0, &div);
                trace_set_clkdiv(div);
                uart_cli_printf("OK: ADC clkdiv=%lu (~%.0f us/sample)\r\n", div, (div + 1) * 2.0f / 1000.0f * 1000.0f);
            } else {
                uart_cli_send("Usage: TRACE RATE <clkdiv> (0=2us, 50=100us, 500=1ms)\r\n");
            }
        } else if (strcmp(parts->parts[1], "ARM") == 0) {
            if (glitch_arm_trace()) {
                uart_cli_send("OK: Trace armed\r\n");
            } else {
                uart_cli_send("ERROR: Failed to arm trace\r\n");
            }
        } else {
            // TRACE [START] [samples] [pre%]
            uint32_t samp = 0, pre = 0;
            int i = 1;
            if (strcmp(parts->parts[i], "START") == 0) i++;
            if (i < parts->count) parse_u32(parts->parts[i++], 0, &samp);
            if (i < parts->count) parse_u32(parts->parts[i++], 0, &pre);
            trace_start(samp, pre);
        }

    } else {
        api_error_printf("ERROR: Unknown command '%s' (use HELP)\r\n", parts->parts[0]);
    }

api_response:
    // Send API mode response
    if (api_mode) {
        if (command_failed) {
            uart_cli_send("!");
        } else {
            uart_cli_send("+");
        }
    }
}
