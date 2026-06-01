#ifndef LPC_TARGET_H
#define LPC_TARGET_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// Per-family info — picked up by lpc_get_target_info() based on the
// currently-selected target type. The CRP word address is the main thing
// that varies across LPC families (see comment block on LPC_CRP_WORD_ADDR
// below for the table).
typedef struct {
    const char *name;             // "LPC2xxx (ARM7)" / "LPC Cortex-M"
    uint32_t    crp_word_addr;    // 0x000001FC or 0x000002FC
    uint32_t    crp_word_end;     // crp_word_addr + 3
    bool        has_crp;          // false for LPC55xx-style (CMPA/PUF based)
} lpc_target_info_t;

// Return the info block for the current target type. Falls back to the
// ARM7 (LPC2xxx) layout when no LPC target is selected, so callers can
// always dereference the result safely.
const lpc_target_info_t *lpc_get_target_info(void);
const lpc_target_info_t *lpc_get_target_info_for(target_type_t t);

// NXP LPC ISP bootloader commands (UM10237 §32, UM10360 §32).
// All commands assume TARGET SYNC has already entered the bootloader and
// enabled echo (A 1). The dispatcher in command_parser.c handles auto-sync.

void lpc_bl_get(void);                                       // K — boot version
void lpc_bl_gid(void);                                       // J — part ID
void lpc_bl_read(uint32_t addr, uint32_t count);             // R — read memory
void lpc_bl_write(uint32_t addr, const uint8_t *data, uint32_t len);  // W + P + C
void lpc_bl_go(uint32_t addr, bool thumb);                   // G
void lpc_bl_erase(int sector_start, int sector_end);         // P + E (start<0 = mass)
void lpc_bl_unlock(void);                                    // U 23130
void lpc_bl_crp(void);                                       // Read 0x1FC, decode
void lpc_bl_crp_info(void);                                  // Print all CRP levels + magic values (no target talk)
void lpc_bl_crp_check(uint32_t value);                       // Decode a hypothetical CRP word (no target talk)

// CRP word — writing the magic values there permanently locks the chip.
// The runtime address is family-dependent and comes from lpc_target_info_t:
//   LPC2xxx (ARM7TDMI-S)            -> 0x000001FC
//   LPC17xx / 11xx / 13xx / 18xx /
//   43xx / 54xxx (Cortex-M*)        -> 0x000002FC
//   LPC55xx and newer (Cortex-M33)  -> no fixed word (CMPA / PUF based)
// Callers should reach for info->crp_word_addr rather than hardcoding.

#endif // LPC_TARGET_H
