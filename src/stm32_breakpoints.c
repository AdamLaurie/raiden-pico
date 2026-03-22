#include "stm32_breakpoints.h"
#include "swd.h"
#include "uart_cli.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

// ============================================================
// STM32F1 (DEV_ID 0x410) boot ROM breakpoints
// Addresses from disassembly of 0x1FFFF000 system bootloader
// ============================================================

static const stm32_bp_entry_t stm32f1_breakpoints[] = {
    { "RDP_READ",       0x1FFFF132, "Reads FLASH_OBR bit 1 (RDPRT status)",        BP_CAT_RDP_READ },
    { "RDP_BRANCH",     0x1FFFF13A, "bpl decides RDP state — TOP GLITCH TARGET",   BP_CAT_RDP_GATE },
    { "BOOT_ENTRY",     0x1FFFF010, "First instruction after reset (SP+main)",      BP_CAT_INIT },
    { "MAIN_INIT",      0x1FFFF34C, "Main boot loop — IWDG, RCC, USART setup",     BP_CAT_INIT },
    { "CMD_LOOP",       0x1FFFF3FA, "Command receive loop top",                     BP_CAT_CMD_HANDLER },
    { "CMD_DISPATCH",   0x1FFFF416, "Command byte switch table",                    BP_CAT_CMD_HANDLER },
    { "READ_MEM",       0x1FFFF58A, "Read Memory (0x11) handler entry",             BP_CAT_FLASH_ACCESS },
    { "READ_RDP_GATE",  0x1FFFF59C, "beq rejects read if RDP=1",                   BP_CAT_RDP_GATE },
    { "WRITE_MEM_GATE", 0x1FFFF500, "beq rejects write if RDP=1",                  BP_CAT_RDP_GATE },
    { "ERASE_GATE",     0x1FFFF4AE, "Erase RDP check (inverted logic!)",           BP_CAT_RDP_GATE },
    { "GO_CMD",         0x1FFFF518, "GO command (0x21) handler",                    BP_CAT_JUMP },
    { "USER_JUMP",      0x1FFFF580, "blx r5 — actual jump to user code",           BP_CAT_JUMP },
    { "OPT_WRITE",      0x1FFFF090, "Option byte write function",                  BP_CAT_OPTION },
    { "RDP_REMOVE",     0x1FFFF11C, "Writes 0xA5 to disable RDP",                  BP_CAT_OPTION },
    { "OPT_RDP_GATE",   0x1FFFF678, "Gate before option byte commands",            BP_CAT_RDP_GATE },
};

static const stm32_bp_table_t stm32f1_table = {
    .name = "STM32F1_0x410",
    .entries = stm32f1_breakpoints,
    .count = sizeof(stm32f1_breakpoints) / sizeof(stm32f1_breakpoints[0]),
};

const stm32_bp_table_t *stm32_get_breakpoints(target_type_t type) {
    switch (type) {
        case TARGET_STM32F1: return &stm32f1_table;
        default: return NULL;
    }
}

const stm32_bp_entry_t *stm32_find_breakpoint(const stm32_bp_table_t *table,
                                                const char *name_or_addr) {
    if (!table || !name_or_addr) return NULL;

    // Try hex address first (0x prefix)
    if (name_or_addr[0] == '0' && (name_or_addr[1] == 'x' || name_or_addr[1] == 'X')) {
        uint32_t addr = strtoul(name_or_addr, NULL, 16);
        for (int i = 0; i < table->count; i++) {
            if (table->entries[i].addr == addr)
                return &table->entries[i];
        }
        // Return NULL — caller can still use raw address for FPB
        return NULL;
    }

    // Case-insensitive exact match first
    for (int i = 0; i < table->count; i++) {
        if (strcasecmp(table->entries[i].name, name_or_addr) == 0)
            return &table->entries[i];
    }

    // Case-insensitive prefix match (unique only)
    size_t len = strlen(name_or_addr);
    const stm32_bp_entry_t *match = NULL;
    int match_count = 0;
    for (int i = 0; i < table->count; i++) {
        if (strncasecmp(table->entries[i].name, name_or_addr, len) == 0) {
            match = &table->entries[i];
            match_count++;
        }
    }
    return (match_count == 1) ? match : NULL;
}

const char *stm32_bp_category_name(uint8_t cat) {
    switch (cat) {
        case BP_CAT_RDP_READ:     return "RDP_READ";
        case BP_CAT_RDP_GATE:     return "RDP_GATE";
        case BP_CAT_CMD_HANDLER:  return "CMD_HANDLER";
        case BP_CAT_FLASH_ACCESS: return "FLASH_ACCESS";
        case BP_CAT_JUMP:         return "JUMP";
        case BP_CAT_INIT:         return "INIT";
        case BP_CAT_OPTION:       return "OPTION";
        default:                  return "UNKNOWN";
    }
}

// ============================================================
// Runtime breakpoint slot management
// ============================================================

static bp_slot_t hw_slots[MAX_HW_BREAKPOINTS];
static bp_slot_t soft_slots[MAX_SOFT_BREAKPOINTS];

bp_slot_t *swd_bp_get_hw_slots(void) { return hw_slots; }
bp_slot_t *swd_bp_get_soft_slots(void) { return soft_slots; }

// Cortex-M3 FPBv1 COMP encoding
uint32_t swd_bp_encode_fpb(uint32_t addr) {
    uint32_t comp = (addr & 0x1FFFFFFC) | 1;  // address + ENABLE
    if (addr & 2)
        comp |= (1u << 31);  // REPLACE=10: upper halfword
    else
        comp |= (1u << 30);  // REPLACE=01: lower halfword
    return comp;
}

bool swd_bp_enable_fpb(void) {
    uint32_t val = 0x03;  // KEY + ENABLE
    return swd_write_mem(FP_CTRL, &val, 1) > 0;
}

bool swd_bp_disable_fpb(void) {
    uint32_t val = 0x02;  // KEY only (disable)
    return swd_write_mem(FP_CTRL, &val, 1) > 0;
}

bool swd_bp_set_hard(uint8_t slot, uint32_t addr, const char *name) {
    if (slot >= MAX_HW_BREAKPOINTS) {
        uart_cli_send("ERROR: Slot must be 0-5\r\n");
        return false;
    }
    if (!swd_ensure_connected()) {
        uart_cli_send("ERROR: SWD not connected\r\n");
        return false;
    }

    // Enable FPB unit
    swd_bp_enable_fpb();

    // Program FP_COMPn
    uint32_t comp = swd_bp_encode_fpb(addr);
    uint32_t reg_addr = FP_COMP0 + slot * 4;
    if (swd_write_mem(reg_addr, &comp, 1) == 0) {
        uart_cli_printf("ERROR: Failed to write FP_COMP%u\r\n", slot);
        return false;
    }

    // Verify
    uint32_t readback;
    swd_read_mem(reg_addr, &readback, 1);
    if (readback != comp) {
        uart_cli_printf("ERROR: FP_COMP%u verify failed (wrote 0x%08lX, read 0x%08lX)\r\n",
                        slot, comp, readback);
        return false;
    }

    hw_slots[slot].type = BP_HARD;
    hw_slots[slot].addr = addr;
    hw_slots[slot].enabled = true;
    hw_slots[slot].name = name;

    uart_cli_printf("OK: HW breakpoint %u at 0x%08lX", slot, addr);
    if (name) uart_cli_printf(" (%s)", name);
    uart_cli_printf(" [FP_COMP%u=0x%08lX]\r\n", slot, comp);
    return true;
}

bool swd_bp_set_soft(uint32_t addr, const char *name) {
    // Check address range — ROM (0x1FFFxxxx) is not writable
    if ((addr & 0xFFFF0000) == 0x1FFF0000) {
        uart_cli_send("ERROR: Cannot set soft breakpoint in ROM (use HARD)\r\n");
        return false;
    }

    // Find free soft slot
    int slot = -1;
    for (int i = 0; i < MAX_SOFT_BREAKPOINTS; i++) {
        if (soft_slots[i].type == BP_NONE) { slot = i; break; }
    }
    if (slot < 0) {
        uart_cli_send("ERROR: No free soft breakpoint slots (max 4)\r\n");
        return false;
    }

    if (!swd_ensure_connected()) {
        uart_cli_send("ERROR: SWD not connected\r\n");
        return false;
    }

    // Read original halfword
    uint32_t word;
    if (swd_read_mem(addr & ~3u, &word, 1) == 0) {
        uart_cli_printf("ERROR: Cannot read memory at 0x%08lX\r\n", addr);
        return false;
    }
    uint16_t original = (addr & 2) ? (word >> 16) : (word & 0xFFFF);

    // Patch with BKPT #0 (0xBE00)
    uint32_t patched = word;
    if (addr & 2)
        patched = (patched & 0x0000FFFF) | (0xBE00u << 16);
    else
        patched = (patched & 0xFFFF0000) | 0xBE00u;

    if (swd_write_mem(addr & ~3u, &patched, 1) == 0) {
        uart_cli_printf("ERROR: Cannot write BKPT at 0x%08lX\r\n", addr);
        return false;
    }

    soft_slots[slot].type = BP_SOFT;
    soft_slots[slot].addr = addr;
    soft_slots[slot].original_insn = original;
    soft_slots[slot].enabled = true;
    soft_slots[slot].name = name;

    uart_cli_printf("OK: Soft breakpoint %u at 0x%08lX (saved 0x%04X)", slot, addr, original);
    if (name) uart_cli_printf(" (%s)", name);
    uart_cli_send("\r\n");
    return true;
}

bool swd_bp_clear_hard(uint8_t slot) {
    if (slot >= MAX_HW_BREAKPOINTS) return false;

    if (swd_ensure_connected()) {
        uint32_t zero = 0;
        swd_write_mem(FP_COMP0 + slot * 4, &zero, 1);
    }

    hw_slots[slot].type = BP_NONE;
    hw_slots[slot].enabled = false;
    uart_cli_printf("OK: HW breakpoint %u cleared\r\n", slot);
    return true;
}

bool swd_bp_clear_soft(uint8_t slot) {
    if (slot >= MAX_SOFT_BREAKPOINTS) return false;
    if (soft_slots[slot].type == BP_NONE) return true;

    if (swd_ensure_connected()) {
        // Restore original instruction
        uint32_t addr = soft_slots[slot].addr;
        uint32_t word;
        swd_read_mem(addr & ~3u, &word, 1);
        if (addr & 2)
            word = (word & 0x0000FFFF) | ((uint32_t)soft_slots[slot].original_insn << 16);
        else
            word = (word & 0xFFFF0000) | soft_slots[slot].original_insn;
        swd_write_mem(addr & ~3u, &word, 1);
    }

    soft_slots[slot].type = BP_NONE;
    soft_slots[slot].enabled = false;
    uart_cli_printf("OK: Soft breakpoint %u cleared (restored 0x%04X)\r\n",
                    slot, soft_slots[slot].original_insn);
    return true;
}

void swd_bp_clear_all(void) {
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (hw_slots[i].type != BP_NONE)
            swd_bp_clear_hard(i);
    }
    for (int i = 0; i < MAX_SOFT_BREAKPOINTS; i++) {
        if (soft_slots[i].type != BP_NONE)
            swd_bp_clear_soft(i);
    }
    swd_bp_disable_fpb();
    uart_cli_send("OK: All breakpoints cleared\r\n");
}

void swd_bp_list(void) {
    uart_cli_send("=== Breakpoints ===\r\n");
    uart_cli_send("HW (FPB):\r\n");
    bool any = false;
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (hw_slots[i].type != BP_NONE) {
            uart_cli_printf("  [%u] HARD 0x%08lX %s%s\r\n", i, hw_slots[i].addr,
                            hw_slots[i].name ? hw_slots[i].name : "",
                            hw_slots[i].enabled ? "" : " (disabled)");
            any = true;
        }
    }
    if (!any) uart_cli_send("  (none)\r\n");

    uart_cli_send("SOFT (BKPT patch):\r\n");
    any = false;
    for (int i = 0; i < MAX_SOFT_BREAKPOINTS; i++) {
        if (soft_slots[i].type != BP_NONE) {
            uart_cli_printf("  [%u] SOFT 0x%08lX saved=0x%04X %s\r\n", i,
                            soft_slots[i].addr, soft_slots[i].original_insn,
                            soft_slots[i].name ? soft_slots[i].name : "");
            any = true;
        }
    }
    if (!any) uart_cli_send("  (none)\r\n");
}
