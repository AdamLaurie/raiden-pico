#ifndef STM32_BREAKPOINTS_H
#define STM32_BREAKPOINTS_H

#include <stdint.h>
#include "config.h"

// Breakpoint categories
enum {
    BP_CAT_RDP_READ,     // Where RDP status is read
    BP_CAT_RDP_GATE,     // Decision branch after RDP check
    BP_CAT_CMD_HANDLER,  // Bootloader command entry points
    BP_CAT_FLASH_ACCESS, // Flash read/write/erase operations
    BP_CAT_JUMP,         // User code validation & jump
    BP_CAT_INIT,         // Boot initialization
    BP_CAT_OPTION,       // Option byte operations
};

typedef struct {
    const char *name;       // Short name for CLI ("RDP_CHECK", "RDP_BRANCH", etc.)
    uint32_t addr;          // Breakpoint address
    const char *desc;       // One-line description
    uint8_t  category;      // BP_CAT_* enum
} stm32_bp_entry_t;

typedef struct {
    const char *name;              // "STM32F1_0x410"
    const stm32_bp_entry_t *entries;
    uint8_t count;
} stm32_bp_table_t;

// Get breakpoint table for a target type (NULL if unsupported)
const stm32_bp_table_t *stm32_get_breakpoints(target_type_t type);

// Find breakpoint by name (case-insensitive prefix match) or hex address
// Returns entry pointer or NULL
const stm32_bp_entry_t *stm32_find_breakpoint(const stm32_bp_table_t *table,
                                                const char *name_or_addr);

// Category name string
const char *stm32_bp_category_name(uint8_t cat);

// --- Runtime breakpoint slot management ---

#define MAX_HW_BREAKPOINTS  6   // Cortex-M3 FPB has 6 instruction comparators
#define MAX_SOFT_BREAKPOINTS 4

typedef enum {
    BP_NONE = 0,
    BP_HARD,    // FPB hardware comparator
    BP_SOFT     // BKPT instruction patch (RAM/flash only, not ROM)
} bp_type_t;

typedef struct {
    bp_type_t type;
    uint32_t addr;
    uint16_t original_insn;  // Soft: saved original halfword
    bool enabled;
    const char *name;        // From bp table, or NULL
} bp_slot_t;

// Encode address for Cortex-M3 FPBv1 COMP register
uint32_t swd_bp_encode_fpb(uint32_t addr);

// Set a hardware breakpoint in slot 0-5 via SWD (requires connected + halted)
bool swd_bp_set_hard(uint8_t slot, uint32_t addr, const char *name);

// Set a soft breakpoint (patches BKPT into memory, saves original)
bool swd_bp_set_soft(uint32_t addr, const char *name);

// Clear a hardware breakpoint slot
bool swd_bp_clear_hard(uint8_t slot);

// Clear a soft breakpoint by index (restores original instruction)
bool swd_bp_clear_soft(uint8_t slot);

// Clear all breakpoints
void swd_bp_clear_all(void);

// Enable/disable FPB unit globally
bool swd_bp_enable_fpb(void);
bool swd_bp_disable_fpb(void);

// Print all breakpoint slots
void swd_bp_list(void);

// Get hardware slot table (for external use)
bp_slot_t *swd_bp_get_hw_slots(void);
bp_slot_t *swd_bp_get_soft_slots(void);

#endif // STM32_BREAKPOINTS_H
