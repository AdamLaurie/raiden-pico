#ifndef GLITCH_H
#define GLITCH_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// Initialize glitch subsystem
void glitch_init(void);

// Get current glitch configuration
glitch_config_t* glitch_get_config(void);

// Get current system flags
system_flags_t* glitch_get_flags(void);

// Set glitch parameters
void glitch_set_pause(uint32_t pause_us);
void glitch_set_width(uint32_t width_us);
void glitch_set_gap(uint32_t gap_us);
void glitch_set_count(uint32_t count);

// Configure trigger
void glitch_set_trigger_type(trigger_type_t type);
void glitch_set_trigger_pin(uint8_t pin, edge_type_t edge);
void glitch_set_trigger_byte(uint8_t byte);

// Output pins are hardwired: PIN_GLITCH_OUT (GP2) and PIN_GLITCH_OUT_INV (GP11)

// Arm/disarm system
bool glitch_arm(void);
void glitch_disarm(void);

// Execute a glitch
bool glitch_execute(void);

// Reset system state
void glitch_reset(void);

// Get glitch count
uint32_t glitch_get_count(void);

// Update flag outputs (call periodically from main loop)
void glitch_update_flags(void);

// Clock generator control
void clock_set_frequency(uint32_t freq_hz);
void clock_enable(void);
void clock_disable(void);
bool clock_is_enabled(void);
uint32_t clock_get_frequency(void);

#endif // GLITCH_H
