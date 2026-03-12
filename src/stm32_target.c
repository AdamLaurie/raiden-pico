/*
 * STM32 target family definitions
 *
 * Per-family register maps for flash controller, option bytes, RDP, etc.
 * Used by SWD high-level operations to support multiple STM32 families.
 */

#include "config.h"
#include <stddef.h>

static const stm32_target_info_t stm32f1_info = {
    .name           = "STM32F1",
    .flash_base     = 0x40022000,
    .flash_keyr     = 0x40022004,
    .flash_optkeyr  = 0x40022008,
    .flash_sr       = 0x4002200C,
    .flash_cr       = 0x40022010,
    .flash_optr     = 0x4002201C,   // FLASH_OBR (shadow)
    .opt_base       = 0x1FFFF800,
    .flash_key1     = 0x45670123,
    .flash_key2     = 0xCDEF89AB,
    .opt_key1       = 0x45670123,   // Same as flash keys on F1
    .opt_key2       = 0xCDEF89AB,
    .rdp_level0     = 0xA5,
    .rdp_level1     = 0x00,
    .rdp_level2     = 0xCC,
    .flash_size     = 128 * 1024,
    .page_size      = 1024,
    .sram_base      = 0x20000000,
    .sram_size      = 20 * 1024,
    .bootrom_base   = 0x1FFFF000,
    .bootrom_size   = 2 * 1024,
};

static const stm32_target_info_t stm32f3_info = {
    .name           = "STM32F3",
    .flash_base     = 0x40022000,
    .flash_keyr     = 0x40022004,
    .flash_optkeyr  = 0x40022008,
    .flash_sr       = 0x4002200C,
    .flash_cr       = 0x40022010,
    .flash_optr     = 0x4002201C,   // FLASH_OBR (shadow)
    .opt_base       = 0x1FFFF800,
    .flash_key1     = 0x45670123,
    .flash_key2     = 0xCDEF89AB,
    .opt_key1       = 0x45670123,
    .opt_key2       = 0xCDEF89AB,
    .rdp_level0     = 0xAA,
    .rdp_level1     = 0xBB,
    .rdp_level2     = 0xCC,
    .flash_size     = 64 * 1024,
    .page_size      = 2048,
    .sram_base      = 0x20000000,
    .sram_size      = 12 * 1024,
    .bootrom_base   = 0x1FFFD800,
    .bootrom_size   = 4 * 1024,
};

static const stm32_target_info_t stm32f4_info = {
    .name           = "STM32F4",
    .flash_base     = 0x40023C00,
    .flash_keyr     = 0x40023C04,
    .flash_optkeyr  = 0x40023C08,
    .flash_sr       = 0x40023C0C,
    .flash_cr       = 0x40023C10,
    .flash_optr     = 0x40023C14,   // FLASH_OPTCR
    .opt_base       = 0x40023C14,   // F4 uses OPTCR register directly
    .flash_key1     = 0x45670123,
    .flash_key2     = 0xCDEF89AB,
    .opt_key1       = 0x08192A3B,
    .opt_key2       = 0x4C5D6E7F,
    .rdp_level0     = 0xAA,
    .rdp_level1     = 0xBB,
    .rdp_level2     = 0xCC,
    .flash_size     = 512 * 1024,
    .page_size      = 16384,        // Sector 0-3 = 16KB
    .sram_base      = 0x20000000,
    .sram_size      = 96 * 1024,
    .bootrom_base   = 0x1FFF0000,
    .bootrom_size   = 30 * 1024,
};

static const stm32_target_info_t stm32l4_info = {
    .name           = "STM32L4",
    .flash_base     = 0x40022000,
    .flash_keyr     = 0x40022008,
    .flash_optkeyr  = 0x4002200C,
    .flash_sr       = 0x40022010,
    .flash_cr       = 0x40022014,
    .flash_optr     = 0x40022020,   // FLASH_OPTR (shadow)
    .opt_base       = 0x1FFF7800,
    .flash_key1     = 0x45670123,
    .flash_key2     = 0xCDEF89AB,
    .opt_key1       = 0x08192A3B,
    .opt_key2       = 0x4C5D6E7F,
    .rdp_level0     = 0xAA,
    .rdp_level1     = 0xBB,
    .rdp_level2     = 0xCC,
    .flash_size     = 256 * 1024,
    .page_size      = 2048,
    .sram_base      = 0x20000000,
    .sram_size      = 160 * 1024,
    .bootrom_base   = 0x1FFF0000,
    .bootrom_size   = 28 * 1024,
};

const stm32_target_info_t *stm32_get_target_info(target_type_t type) {
    switch (type) {
        case TARGET_STM32F1: return &stm32f1_info;
        case TARGET_STM32F3: return &stm32f3_info;
        case TARGET_STM32F4: return &stm32f4_info;
        case TARGET_STM32L4: return &stm32l4_info;
        default: return NULL;
    }
}
