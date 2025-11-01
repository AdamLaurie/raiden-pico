#include "command_parser.h"
#include "uart_cli.h"
#include "glitch.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "hardware/regs/sysinfo.h"
#include "hardware/structs/sysinfo.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_CMD_LENGTH 256

// Command parsing buffer
static char parse_buffer[MAX_CMD_LENGTH];
static char *part_buffers[MAX_CMD_PARTS];

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
    // Allocate buffers for command parts
    for (int i = 0; i < MAX_CMD_PARTS; i++) {
        part_buffers[i] = malloc(64);
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

        // Store in part buffer
        strncpy(part_buffers[parts->count], token, 63);
        part_buffers[parts->count][63] = '\0';
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

// Helper function to match and replace a part
static bool match_and_replace(char **part, const char **candidates, uint8_t count, const char *context) {
    const char *matched = command_parser_match(*part, candidates, count);
    if (matched == NULL) {
        uart_cli_printf("ERROR: Ambiguous %s '%s' - be more specific\r\n", context, *part);
        return false;
    }
    *part = (char*)matched;
    return true;
}

void command_parser_execute(cmd_parts_t *parts) {
    if (!parts || parts->count == 0) {
        return;
    }

    // Match primary command
    const char *primary_commands[] = {
        "SET", "GET", "TRIGGER", "OUT", "PINS",
        "STATUS", "RESET", "PLATFORM", "CS", "TARGET", "ARM", "GLITCH",
        "HELP", "REBOOT", "DEBUG"
    };
    if (!match_and_replace(&parts->parts[0], primary_commands, 16, "command")) {
        return;
    }

    // Match sub-commands if present
    if (parts->count >= 2) {
        if (strcmp(parts->parts[0], "PLATFORM") == 0) {
            const char *platform_subcmds[] = {"SET", "VOLTAGE", "CHARGE", "HVPIN", "VPIN"};
            if (!match_and_replace(&parts->parts[1], platform_subcmds, 5, "PLATFORM sub-command")) {
                return;
            }
        } else if (strcmp(parts->parts[0], "CS") == 0) {
            const char *chipshot_subcmds[] = {"ARM", "DISARM", "FIRE", "STATUS", "VOLTAGE", "PULSE", "RESET", "TRIGGER"};
            if (!match_and_replace(&parts->parts[1], chipshot_subcmds, 8, "CS sub-command")) {
                return;
            }
        } else if (strcmp(parts->parts[0], "TARGET") == 0) {
            const char *target_subcmds[] = {"LPC", "STM32", "BOOTLOADER", "SYNC", "SEND", "RESPONSE", "RESET"};
            if (!match_and_replace(&parts->parts[1], target_subcmds, 7, "TARGET sub-command")) {
                return;
            }
        } else if (strcmp(parts->parts[0], "SWEEP") == 0) {
            const char *sweep_subcmds[] = {"START", "STOP", "STATUS", "RANGE"};
            if (!match_and_replace(&parts->parts[1], sweep_subcmds, 4, "SWEEP sub-command")) {
                return;
            }
        } else if (strcmp(parts->parts[0], "RESPONSE") == 0) {
            const char *response_subcmds[] = {"GET", "CLEAR", "COUNT"};
            if (!match_and_replace(&parts->parts[1], response_subcmds, 3, "RESPONSE sub-command")) {
                return;
            }
        }
    }

    // Match arguments
    if (strcmp(parts->parts[0], "TRIGGER") == 0 && parts->count == 2) {
        const char *trigger_types[] = {"NONE", "UART", "GPIO"};
        if (!match_and_replace(&parts->parts[1], trigger_types, 3, "trigger type")) {
            return;
        }
    } else if (strcmp(parts->parts[0], "PLATFORM") == 0 && strcmp(parts->parts[1], "SET") == 0 && parts->count == 3) {
        const char *platform_types[] = {"MANUAL", "CHIPSHOUTER", "EMFI", "CROWBAR"};
        if (!match_and_replace(&parts->parts[2], platform_types, 4, "platform type")) {
            return;
        }
    } else if (strcmp(parts->parts[0], "ARM") == 0 && parts->count == 2) {
        const char *arm_states[] = {"ON", "OFF"};
        if (!match_and_replace(&parts->parts[1], arm_states, 2, "ARM state")) {
            return;
        }
    } else if (strcmp(parts->parts[0], "SET") == 0 && parts->count >= 2) {
        const char *variables[] = {"PAUSE", "COUNT", "WIDTH", "GAP"};
        if (!match_and_replace(&parts->parts[1], variables, 4, "variable name")) {
            return;
        }
    } else if (strcmp(parts->parts[0], "GET") == 0 && parts->count == 2) {
        const char *variables[] = {"PAUSE", "COUNT", "WIDTH", "GAP"};
        if (!match_and_replace(&parts->parts[1], variables, 4, "variable name")) {
            return;
        }
    }

    // Execute commands
    if (strcmp(parts->parts[0], "HELP") == 0) {
        uart_cli_send("=== Raiden Pico Command Reference ===\r\n\r\n");
        uart_cli_send("== Glitch Configuration ==\r\n");
        uart_cli_send("SET PAUSE <cycles>     - Set glitch pause (system cycles @ 150MHz)\r\n");
        uart_cli_send("SET WIDTH <cycles>     - Set glitch width (1 cycle = 6.67ns)\r\n");
        uart_cli_send("SET GAP <cycles>       - Set gap between glitches\r\n");
        uart_cli_send("SET COUNT <n>          - Set number of glitches\r\n");
        uart_cli_send("  Example: SET WIDTH 150 = 1us (150 cycles × 6.67ns)\r\n");
        uart_cli_send("GET PAUSE|WIDTH|GAP|COUNT - Get current values\r\n");
        uart_cli_send("OUT <pin>              - Set glitch output pin\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Trigger Configuration ==\r\n");
        uart_cli_send("TRIGGER NONE           - Disable trigger\r\n");
        uart_cli_send("TRIGGER GPIO <RISING|FALLING> - GPIO trigger on GP3\r\n");
        uart_cli_send("TRIGGER UART <byte>    - UART byte trigger\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Platform Control ==\r\n");
        uart_cli_send("PLATFORM SET <MANUAL|CHIPSHOUTER|EMFI|CROWBAR>\r\n");
        uart_cli_send("PLATFORM VOLTAGE <mv>  - Set platform voltage\r\n");
        uart_cli_send("PLATFORM CHARGE <ms>   - Set charge time\r\n");
        uart_cli_send("PLATFORM HVPIN <pin>   - Set HV enable pin\r\n");
        uart_cli_send("PLATFORM VPIN <pin>    - Set voltage control pin\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Glitch Control ==\r\n");
        uart_cli_send("ARM ON|OFF             - Arm/disarm glitch\r\n");
        uart_cli_send("GLITCH                 - Execute single glitch\r\n");
        uart_cli_send("STATUS                 - Show current status\r\n");
        uart_cli_send("PINS                   - Show pin configuration\r\n");
        uart_cli_send("RESET                  - Reset system\r\n");
        uart_cli_send("REBOOT [BL]            - Reboot Pico (BL = bootloader mode)\r\n");
        uart_cli_send("DEBUG [ON|OFF]         - Toggle target UART debug display\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Target Control ==\r\n");
        uart_cli_send("TARGET <LPC|STM32> - Set target type\r\n");
        uart_cli_send("TARGET BOOTLOADER [baud] [crystal_khz] - Enter bootloader\r\n");
        uart_cli_send("                   (defaults: 115200 baud, 12000 kHz crystal)\r\n");
        uart_cli_send("TARGET SYNC [baud] [crystal_khz] [reset_delay_ms] - Reset + bootloader\r\n");
        uart_cli_send("                   (defaults: 115200, 12000, 10ms)\r\n");
        uart_cli_send("TARGET SEND <hex|\"text\"> - Send hex bytes or quoted text (appends \\r)\r\n");
        uart_cli_send("                   Examples: 3F, 68656C6C6F, \"hello\"\r\n");
        uart_cli_send("TARGET RESPONSE - Show response from target\r\n");
        uart_cli_send("TARGET RESET [PERIOD <ms>] [PIN <n>] [HIGH] - Reset target\r\n");
        uart_cli_send("                   (defaults: 300ms, GP15, active low)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== ChipSHOUTER Control ==\r\n");
        uart_cli_send("CS ARM                    - Arm ChipSHOUTER\r\n");
        uart_cli_send("CS DISARM                 - Disarm ChipSHOUTER\r\n");
        uart_cli_send("CS FIRE                   - Trigger ChipSHOUTER\r\n");
        uart_cli_send("CS STATUS                 - Get ChipSHOUTER status\r\n");
        uart_cli_send("CS VOLTAGE <V>            - Set ChipSHOUTER voltage\r\n");
        uart_cli_send("CS PULSE <us>             - Set ChipSHOUTER pulse width\r\n");
        uart_cli_send("CS TRIGGER HW <HIGH|LOW>  - Set HW trigger (active high/low w/ pull)\r\n");
        uart_cli_send("CS TRIGGER SW             - Set SW trigger (fires via interrupt)\r\n");
        uart_cli_send("CS RESET                  - Reset ChipSHOUTER and verify errors cleared\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Command Shortcuts ==\r\n");
        uart_cli_send("Non-ambiguous shortcuts supported for commands, sub-commands, and arguments.\r\n");
        uart_cli_send("Examples:\r\n");
        uart_cli_send("  STAT → STATUS       GL → GLITCH         P → PINS\r\n");
        uart_cli_send("  TARG I → TARGET INIT                    O 5 → OUT 5\r\n");
        uart_cli_send("  SET P 1000 → SET PAUSE 1000             TRIG G R → TRIGGER GPIO RISING\r\n");
        uart_cli_send("\r\n");

    } else if (strcmp(parts->parts[0], "VERSION") == 0) {
        uart_cli_printf("Raiden Pico Glitcher v0.3\r\n");
        uart_cli_printf("Build: Cycle-based timing, improved ARM/GLITCH handling\r\n");
        uart_cli_printf("Features:\r\n");
        uart_cli_printf(" - System clock cycle timing (150MHz, 6.67ns resolution)\r\n");
        uart_cli_printf(" - TARGET SYNC with 5 retries\r\n");
        uart_cli_printf(" - Fixed ARM after GLITCH issue\r\n");
        uart_cli_printf(" - Ultra-low latency PIO-based UART triggering\r\n");
        uart_cli_printf(" - RP2350B workaround (GP5→GP28 jumper)\r\n");
        uart_cli_send("OK\r\n");
    } else if (strcmp(parts->parts[0], "STATUS") == 0) {
        glitch_config_t *cfg = glitch_get_config();
        system_flags_t *flags = glitch_get_flags();

        // Check chip variant
        uint32_t chip_id = sysinfo_hw->chip_id;
        uint32_t revision = sysinfo_hw->gitref_rp2350;

        uart_cli_send("=== System Status ===\r\n");
        uart_cli_printf("Chip:        RP2350");

        // Debug: print raw values
        uart_cli_printf(" (ID:0x%08x Rev:0x%08x)", chip_id, revision);

        // Check revision bit 28 for A/B variant
        if (revision & (1 << 28)) {
            uart_cli_printf(" - Variant B (PIO bug present)\r\n");
            uart_cli_send("WARNING: RP2350B requires jumper wire GP5→GP28 for UART trigger!\r\n");
        } else {
            uart_cli_printf(" - Variant A (PIO works correctly)\r\n");
        }
        uint32_t count = glitch_get_count();  // Check count first (updates armed flag if PIO fired)
        uart_cli_printf("Armed:       %s\r\n", flags->armed ? "YES" : "NO");
        uart_cli_printf("Glitch Count: %u\r\n", count);
        uart_cli_printf("Pause:       %u cycles (%.2f us)\r\n", cfg->pause_cycles, cfg->pause_cycles / 150.0f);
        uart_cli_printf("Width:       %u cycles (%.2f us)\r\n", cfg->width_cycles, cfg->width_cycles / 150.0f);
        uart_cli_printf("Gap:         %u cycles (%.2f us)\r\n", cfg->gap_cycles, cfg->gap_cycles / 150.0f);
        uart_cli_printf("Count:       %u\r\n", cfg->count);
        const char* trigger_str = "NONE";
        if (cfg->trigger == TRIGGER_GPIO) {
            trigger_str = "GPIO";
        } else if (cfg->trigger == TRIGGER_UART) {
            trigger_str = "UART";
        }
        uart_cli_printf("Trigger:     %s\r\n", trigger_str);
        if (cfg->trigger == TRIGGER_UART) {
            uart_cli_printf("UART Byte:   0x%02X\r\n", cfg->trigger_byte);
        }
        uart_cli_printf("Output Pin:  %u\r\n", cfg->output_pin);

    } else if (strcmp(parts->parts[0], "SET") == 0) {
        if (parts->count != 3) {
            uart_cli_send("ERROR: Usage: SET <PAUSE|WIDTH|GAP|COUNT> <value>\r\n");
            uart_cli_send("       Value is in system clock cycles (150MHz = 6.67ns per cycle)\r\n");
            return;
        }

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
        }

    } else if (strcmp(parts->parts[0], "GET") == 0) {
        if (parts->count != 2) {
            uart_cli_send("ERROR: Usage: GET <PAUSE|WIDTH|GAP|COUNT>\r\n");
            return;
        }

        glitch_config_t *cfg = glitch_get_config();

        if (strcmp(parts->parts[1], "PAUSE") == 0) {
            uart_cli_printf("%u cycles (%.2f us)\r\n", cfg->pause_cycles, cfg->pause_cycles / 150.0f);
        } else if (strcmp(parts->parts[1], "WIDTH") == 0) {
            uart_cli_printf("%u cycles (%.2f us)\r\n", cfg->width_cycles, cfg->width_cycles / 150.0f);
        } else if (strcmp(parts->parts[1], "GAP") == 0) {
            uart_cli_printf("%u cycles (%.2f us)\r\n", cfg->gap_cycles, cfg->gap_cycles / 150.0f);
        } else if (strcmp(parts->parts[1], "COUNT") == 0) {
            uart_cli_printf("%u\r\n", cfg->count);
        }

    } else if (strcmp(parts->parts[0], "ARM") == 0) {
        if (parts->count != 2) {
            uart_cli_send("ERROR: Usage: ARM <ON|OFF>\r\n");
            return;
        }

        if (strcmp(parts->parts[1], "ON") == 0) {
            if (glitch_arm()) {
                uart_cli_send("OK: System armed\r\n");
            } else {
                uart_cli_send("ERROR: Failed to arm system\r\n");
            }
        } else if (strcmp(parts->parts[1], "OFF") == 0) {
            glitch_disarm();
            uart_cli_send("OK: System disarmed\r\n");
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
        uart_cli_send("GP4  - Target UART TX (UART1)\r\n");
        uart_cli_send("GP5  - Target UART RX (UART1)\r\n");
        uart_cli_send("GP28 - PIO Monitor (JUMPER FROM GP5 REQUIRED!)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Glitch Control ==\r\n");
        uart_cli_printf("GP%u  - Glitch Output\r\n", cfg->output_pin);
        uart_cli_printf("GP%u  - Trigger Input\r\n", cfg->trigger_pin);
        uart_cli_send("\r\n");
        uart_cli_send("== Platform Control ==\r\n");
        uart_cli_send("GP6  - HV Enable (default)\r\n");
        uart_cli_send("GP7  - Voltage PWM (default)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Target Control ==\r\n");
        uart_cli_send("GP15 - Target Reset (default)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Status ==\r\n");
        uart_cli_send("GP25 - Status LED\r\n");

    } else if (strcmp(parts->parts[0], "TRIGGER") == 0) {
        if (parts->count == 1) {
            uart_cli_send("ERROR: Usage: TRIGGER <NONE|GPIO|UART>\r\n");
            return;
        }
        // Argument matching already done above
        if (strcmp(parts->parts[1], "NONE") == 0) {
            glitch_set_trigger_type(TRIGGER_NONE);
            uart_cli_send("OK: Trigger disabled\r\n");
        } else if (strcmp(parts->parts[1], "GPIO") == 0) {
            if (parts->count < 3) {
                uart_cli_send("ERROR: Usage: TRIGGER GPIO <RISING|FALLING>\r\n");
                return;
            }
            edge_type_t edge = (strcmp(parts->parts[2], "RISING") == 0) ? EDGE_RISING : EDGE_FALLING;
            glitch_set_trigger_pin(3, edge);  // GPIO trigger is hardcoded to GP3
            glitch_set_trigger_type(TRIGGER_GPIO);
            uart_cli_printf("OK: GPIO trigger on GP3, %s edge\r\n",
                          edge == EDGE_RISING ? "RISING" : "FALLING");
        } else if (strcmp(parts->parts[1], "UART") == 0) {
            if (parts->count < 3) {
                uart_cli_send("ERROR: Usage: TRIGGER UART <byte>\r\n");
                return;
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
            glitch_set_trigger_byte(byte);
            glitch_set_trigger_type(TRIGGER_UART);
            uart_cli_printf("OK: UART trigger on byte 0x%02X (%u)\r\n", byte, byte);
        }

    } else if (strcmp(parts->parts[0], "OUT") == 0) {
        if (parts->count < 2) {
            uart_cli_send("ERROR: Usage: OUT <pin>\r\n");
            return;
        }
        uint8_t pin = atoi(parts->parts[1]);
        glitch_set_output_pin(pin);
        uart_cli_printf("OK: Glitch output set to pin %u\r\n", pin);

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
            uart_cli_send("ERROR: Usage: TARGET <LPC|STM32|BOOTLOADER|SEND|RESPONSE|RESET>\r\n");
            return;
        }

        if (strcmp(parts->parts[1], "LPC") == 0) {
            target_set_type(TARGET_LPC);
            uart_cli_send("OK: Target type set to LPC (NXP ISP protocol)\r\n");
        } else if (strcmp(parts->parts[1], "STM32") == 0) {
            target_set_type(TARGET_STM32);
            uart_cli_send("OK: Target type set to STM32\r\n");
        } else if (strcmp(parts->parts[1], "BOOTLOADER") == 0) {
            uint32_t baud = 115200;      // Default baud
            uint32_t crystal_khz = 12000; // Default 12MHz crystal
            if (parts->count >= 3) {
                baud = atoi(parts->parts[2]);
            }
            if (parts->count >= 4) {
                crystal_khz = atoi(parts->parts[3]);
            }
            target_enter_bootloader(baud, crystal_khz);
        } else if (strcmp(parts->parts[1], "SYNC") == 0) {
            uint32_t baud = 115200;       // Default baud
            uint32_t crystal_khz = 12000; // Default 12MHz crystal
            uint32_t reset_delay_ms = 10; // Default 10ms delay (tested reliable from 1ms-300ms)
            if (parts->count >= 3) {
                baud = atoi(parts->parts[2]);
            }
            if (parts->count >= 4) {
                crystal_khz = atoi(parts->parts[3]);
            }
            if (parts->count >= 5) {
                reset_delay_ms = atoi(parts->parts[4]);
            }

            // Try up to 5 times to sync with target
            bool sync_success = false;
            for (int retry = 0; retry < 5; retry++) {
                if (retry > 0) {
                    uart_cli_printf("Retry %d/5...\r\n", retry);
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
                if (retry < 4) {  // Don't print on last retry
                    uart_cli_send("Sync failed, retrying...\r\n");
                    sleep_ms(100);  // Small delay between retries
                }
            }
            if (!sync_success) {
                uart_cli_send("ERROR: Failed to sync with target after 5 attempts\r\n");
            }
        } else if (strcmp(parts->parts[1], "SEND") == 0) {
            extern void target_uart_send_string(const char *str);
            extern const char* uart_cli_get_command(void);

            if (parts->count < 3) {
                uart_cli_send("ERROR: Usage: TARGET SEND <hex_data or \"text\">\r\n");
                return;
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

            // If no parameters given, also execute the reset
            if (!has_params) {
                target_reset_execute();
            }
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
            return;
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
                uart_cli_send("ERROR: Usage: CS VOLTAGE <value>\r\n");
                return;
            }
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

        } else if (strcmp(parts->parts[1], "PULSE") == 0) {
            if (parts->count < 3) {
                uart_cli_send("ERROR: Usage: CS PULSE <nanoseconds>\r\n");
                return;
            }
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
                return;
            }

            // Match HW/SW argument
            const char *trigger_modes[] = {"HW", "SW"};
            if (!match_and_replace(&parts->parts[2], trigger_modes, 2, "trigger mode")) {
                return;
            }

            if (strcmp(parts->parts[2], "HW") == 0) {
                // Hardware trigger mode requires HIGH or LOW argument
                if (parts->count < 4) {
                    uart_cli_send("ERROR: Usage: CS TRIGGER HW <HIGH|LOW>\r\n");
                    return;
                }

                // Match HIGH/LOW argument
                const char *polarity_args[] = {"HIGH", "LOW"};
                if (!match_and_replace(&parts->parts[3], polarity_args, 2, "trigger polarity")) {
                    return;
                }

                bool active_high = (strcmp(parts->parts[3], "HIGH") == 0);
                chipshot_set_trigger_hw(active_high);
                uart_cli_send("ChipSHOUTER: Sent set trigger hw command (");
                uart_cli_send(active_high ? "active high)\r\n" : "active low)\r\n");
                uart_cli_printf("Configured GPIO%u with %s\r\n",
                    glitch_get_config()->output_pin,
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
        }

    } else {
        uart_cli_printf("ERROR: Unknown command '%s' (use HELP)\r\n", parts->parts[0]);
    }
}
