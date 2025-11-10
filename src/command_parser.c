#include "command_parser.h"
#include "uart_cli.h"
#include "glitch.h"
#include "target_uart.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "hardware/regs/sysinfo.h"
#include "hardware/structs/sysinfo.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

#define MAX_CMD_LENGTH 256
#define MAX_ERROR_LENGTH 256

// Command parsing buffer
static char parse_buffer[MAX_CMD_LENGTH];
static char *part_buffers[MAX_CMD_PARTS];

// API mode state
static bool api_mode = false;
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
        "HELP", "REBOOT", "DEBUG", "API", "ERROR"
    };
    if (!match_and_replace(&parts->parts[0], primary_commands, 17, "command")) {
        goto api_response;
    }

    // Match sub-commands if present
    if (parts->count >= 2) {
        if (strcmp(parts->parts[0], "PLATFORM") == 0) {
            const char *platform_subcmds[] = {"SET", "VOLTAGE", "CHARGE", "HVPIN", "VPIN"};
            if (!match_and_replace(&parts->parts[1], platform_subcmds, 5, "PLATFORM sub-command")) {
                goto api_response;
            }
        } else if (strcmp(parts->parts[0], "CS") == 0) {
            const char *chipshot_subcmds[] = {"ARM", "DISARM", "FIRE", "STATUS", "VOLTAGE", "PULSE", "RESET", "TRIGGER"};
            if (!match_and_replace(&parts->parts[1], chipshot_subcmds, 8, "CS sub-command")) {
                goto api_response;
            }
        } else if (strcmp(parts->parts[0], "TARGET") == 0) {
            const char *target_subcmds[] = {"LPC", "STM32", "BOOTLOADER", "SYNC", "SEND", "RESPONSE", "RESET", "TIMEOUT", "POWER"};
            if (!match_and_replace(&parts->parts[1], target_subcmds, 9, "TARGET sub-command")) {
                goto api_response;
            }
        } else if (strcmp(parts->parts[0], "SWEEP") == 0) {
            const char *sweep_subcmds[] = {"START", "STOP", "STATUS", "RANGE"};
            if (!match_and_replace(&parts->parts[1], sweep_subcmds, 4, "SWEEP sub-command")) {
                goto api_response;
            }
        } else if (strcmp(parts->parts[0], "RESPONSE") == 0) {
            const char *response_subcmds[] = {"GET", "CLEAR", "COUNT"};
            if (!match_and_replace(&parts->parts[1], response_subcmds, 3, "RESPONSE sub-command")) {
                goto api_response;
            }
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
        const char *arm_states[] = {"ON", "OFF"};
        if (!match_and_replace(&parts->parts[1], arm_states, 2, "ARM state")) {
            goto api_response;
        }
    } else if (strcmp(parts->parts[0], "SET") == 0 && parts->count >= 2) {
        const char *variables[] = {"PAUSE", "COUNT", "WIDTH", "GAP"};
        if (!match_and_replace(&parts->parts[1], variables, 4, "variable name")) {
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
        uart_cli_send("CS PULSE [<us>]           - Set/get ChipSHOUTER pulse width\r\n");
        uart_cli_send("CS RESET                  - Reset ChipSHOUTER and verify errors cleared\r\n");
        uart_cli_send("CS STATUS                 - Get ChipSHOUTER status\r\n");
        uart_cli_send("CS TRIGGER HW <HIGH|LOW>  - Set HW trigger (active high/low w/ pull)\r\n");
        uart_cli_send("CS TRIGGER SW             - Set SW trigger (fires via interrupt)\r\n");
        uart_cli_send("CS VOLTAGE [<V>]          - Set/get ChipSHOUTER voltage\r\n");
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
        uart_cli_send("\r\n");
        uart_cli_send("== Glitch Control ==\r\n");
        uart_cli_send("ARM [ON|OFF]           - Arm/disarm/show glitch system\r\n");
        uart_cli_send("DEBUG [ON|OFF]         - Toggle/show target UART debug display\r\n");
        uart_cli_send("GLITCH                 - Execute single glitch\r\n");
        uart_cli_send("PINS                   - Show pin configuration\r\n");
        uart_cli_send("REBOOT [BL]            - Reboot Pico (BL = bootloader mode)\r\n");
        uart_cli_send("RESET                  - Reset system\r\n");
        uart_cli_send("STATUS                 - Show current status\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Platform Control ==\r\n");
        uart_cli_send("PLATFORM CHARGE <ms>   - Set charge time\r\n");
        uart_cli_send("PLATFORM HVPIN <pin>   - Set HV enable pin\r\n");
        uart_cli_send("PLATFORM SET <MANUAL|CHIPSHOUTER|EMFI|CROWBAR>\r\n");
        uart_cli_send("PLATFORM VOLTAGE <mv>  - Set platform voltage\r\n");
        uart_cli_send("PLATFORM VPIN <pin>    - Set voltage control pin\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Target Control ==\r\n");
        uart_cli_send("TARGET <LPC|STM32>     - Set target type\r\n");
        uart_cli_send("TARGET BOOTLOADER [baud] [crystal_khz] - Enter bootloader\r\n");
        uart_cli_send("                   (defaults: 115200 baud, 12000 kHz crystal)\r\n");
        uart_cli_send("TARGET POWER [ON|OFF|CYCLE] [ms] - Control/show target power on GP10\r\n");
        uart_cli_send("                   (CYCLE default: 300ms, default state: ON)\r\n");
        uart_cli_send("TARGET RESET [PERIOD <ms>] [PIN <n>] [HIGH] - Reset target\r\n");
        uart_cli_send("                   (defaults: 300ms, GP15, active low)\r\n");
        uart_cli_send("TARGET RESPONSE        - Show response from target\r\n");
        uart_cli_send("TARGET SEND <hex|\"text\"> - Send hex bytes or quoted text (appends \\r)\r\n");
        uart_cli_send("                   Examples: 3F, 68656C6C6F, \"hello\"\r\n");
        uart_cli_send("TARGET SYNC [baud] [crystal_khz] [reset_delay_ms] [retries] - Reset + bootloader\r\n");
        uart_cli_send("                   (defaults: 115200, 12000, 10ms, 5 retries)\r\n");
        uart_cli_send("TARGET TIMEOUT [<ms>]  - Get/set transparent bridge timeout (default: 50ms)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Trigger Configuration ==\r\n");
        uart_cli_send("TRIGGER [NONE|GPIO|UART] - Configure/show trigger\r\n");
        uart_cli_send("TRIGGER GPIO <RISING|FALLING> - GPIO trigger on GP3\r\n");
        uart_cli_send("TRIGGER NONE           - Disable trigger\r\n");
        uart_cli_send("TRIGGER UART <byte>    - UART byte trigger\r\n");
        uart_cli_send("\r\n");

    } else if (strcmp(parts->parts[0], "VERSION") == 0) {
        uart_cli_printf("Raiden Pico Glitcher v0.3\r\n");
        uart_cli_printf("Build: Cycle-based timing, improved ARM/GLITCH handling\r\n");
        uart_cli_printf("Features:\r\n");
        uart_cli_printf(" - System clock cycle timing (150MHz, 6.67ns resolution)\r\n");
        uart_cli_printf(" - TARGET SYNC with 5 retries\r\n");
        uart_cli_printf(" - Fixed ARM after GLITCH issue\r\n");
        uart_cli_printf(" - Ultra-low latency PIO-based UART triggering\r\n");
        uart_cli_printf(" - RP2350 GPIO ISO bit handling for PIO/UART sharing\r\n");
        uart_cli_send("OK\r\n");
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
        } else if (target_type == TARGET_STM32) {
            target_type_str = "STM32";
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
            uart_cli_send("Pin:          GP8\r\n");
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
            } else if (strcmp(parts->parts[1], "OFF") == 0) {
                glitch_disarm();
                uart_cli_send("OK: System disarmed\r\n");
            } else {
                api_error("ERROR: Usage: ARM <ON|OFF>\r\n");
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
        uart_cli_send("GP5  - Target UART RX (UART1, also PIO monitored)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Clock Generator ==\r\n");
        uart_cli_send("GP8  - Clock Output\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Glitch Control ==\r\n");
        uart_cli_printf("GP%u  - Glitch Output (normal)\r\n", PIN_GLITCH_OUT);
        uart_cli_printf("GP%u  - Glitch Output (inverted)\r\n", PIN_GLITCH_OUT_INV);
        uart_cli_printf("GP%u  - Trigger Input\r\n", cfg->trigger_pin);
        uart_cli_send("\r\n");
        uart_cli_send("== Platform Control ==\r\n");
        uart_cli_send("GP6  - HV Enable (default)\r\n");
        uart_cli_send("GP7  - Voltage PWM (default)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Target Control ==\r\n");
        uart_cli_send("GP10 - Target Power (default ON)\r\n");
        uart_cli_send("GP15 - Target Reset (default HIGH, LOW 300ms pulse)\r\n");
        uart_cli_send("\r\n");
        uart_cli_send("== Status ==\r\n");
        uart_cli_send("GP25 - Status LED\r\n");

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
                uart_cli_printf("UART (Byte: 0x%02X)\r\n", cfg->trigger_byte);
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
                uart_cli_send("ERROR: Usage: TRIGGER UART <byte>\r\n");
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
            glitch_set_trigger_byte(byte);
            glitch_set_trigger_type(TRIGGER_UART);
            uart_cli_printf("OK: UART trigger on byte 0x%02X (%u)\r\n", byte, byte);
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
            uart_cli_send("ERROR: Usage: TARGET <LPC|STM32|BOOTLOADER|SYNC|SEND|RESPONSE|RESET|TIMEOUT|POWER>\r\n");
            goto api_response;
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

            // If no parameters given, also execute the reset
            if (!has_params) {
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
                // Show current power state
                bool power_state = target_power_get_state();
                uart_cli_printf("Target power (GP10): %s\r\n", power_state ? "ON" : "OFF");
            } else {
                // Match power command
                const char *power_cmds[] = {"ON", "OFF", "CYCLE"};
                if (!match_and_replace(&parts->parts[2], power_cmds, 3, "POWER command")) {
                    goto api_response;
                }

                if (strcmp(parts->parts[2], "ON") == 0) {
                    target_power_on();
                } else if (strcmp(parts->parts[2], "OFF") == 0) {
                    target_power_off();
                } else if (strcmp(parts->parts[2], "CYCLE") == 0) {
                    uint32_t cycle_time_ms = 300;  // Default 300ms
                    if (parts->count >= 4) {
                        cycle_time_ms = atoi(parts->parts[3]);
                    }
                    target_power_cycle(cycle_time_ms);
                }
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
