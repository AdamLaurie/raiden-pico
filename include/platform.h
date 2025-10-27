#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// Initialize platform subsystem
void platform_init(void);

// Platform type control
void platform_set_type(platform_type_t type);
platform_type_t platform_get_type(void);

// Voltage control
void platform_set_voltage(uint32_t voltage_mv);
uint32_t platform_get_voltage(void);

// Charge time control
void platform_set_charge_time(uint32_t charge_ms);
uint32_t platform_get_charge_time(void);

// Platform enable/disable
void platform_enable(void);
void platform_disable(void);

// Status monitoring
bool platform_get_status(void);

// Pin configuration
void platform_set_pins(uint8_t hv_pin, uint8_t voltage_pin);

#endif // PLATFORM_H
