#include "config.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "platform.pio.h"
#include <string.h>

// PIO and state machine allocations
static PIO platform_pio = pio1;
static uint sm_voltage_pwm = 0;
static uint sm_platform_enable = 1;
static uint sm_status_monitor = 2;

// Configuration
static platform_config_t config;

// PIO program offsets
static uint offset_voltage_pwm;
static uint offset_platform_enable;
static uint offset_status_monitor;

void platform_init(void) {
    // Initialize configuration with defaults
    memset(&config, 0, sizeof(config));
    config.type = PLATFORM_MANUAL;
    config.hv_pin = 6;           // Default HV enable pin (moved from GP8 for Grbl UART)
    config.voltage_pin = 7;      // Default voltage control pin (moved from GP9 for Grbl UART)
    config.armed_pin = 11;       // Default armed status pin
    config.voltage = 3300;       // Default 3.3V (in mV)
    config.charge_time_us = 100000;  // Default 100ms charge time (in us)

    // Load PIO programs
    offset_voltage_pwm = pio_add_program(platform_pio, &voltage_pwm_program);
    offset_platform_enable = pio_add_program(platform_pio, &platform_enable_program);
    offset_status_monitor = pio_add_program(platform_pio, &status_monitor_program);

    // Initialize pins
    gpio_init(config.hv_pin);
    gpio_set_dir(config.hv_pin, GPIO_OUT);
    gpio_put(config.hv_pin, 0);

    gpio_init(config.armed_pin);
    gpio_set_dir(config.armed_pin, GPIO_IN);
}

void platform_set_type(platform_type_t type) {
    config.type = type;
}

platform_type_t platform_get_type(void) {
    return config.type;
}

void platform_set_voltage(uint32_t voltage_mv) {
    config.voltage = voltage_mv;

    // Calculate PWM parameters for voltage control
    // Assuming voltage control is via PWM, 0-5V range
    uint32_t clock_freq = clock_get_hz(clk_sys);
    uint32_t pwm_period = 1000;  // 1000 cycles for PWM period

    // Calculate duty cycle (voltage_mv / 5000 * pwm_period)
    uint32_t high_cycles = (voltage_mv * pwm_period) / 5000;
    uint32_t low_cycles = pwm_period - high_cycles;

    // Configure voltage PWM PIO
    pio_sm_config c = voltage_pwm_program_get_default_config(offset_voltage_pwm);
    sm_config_set_set_pins(&c, config.voltage_pin, 1);
    pio_gpio_init(platform_pio, config.voltage_pin);
    pio_sm_set_consecutive_pindirs(platform_pio, sm_voltage_pwm, config.voltage_pin, 1, true);
    pio_sm_init(platform_pio, sm_voltage_pwm, offset_voltage_pwm, &c);
    pio_sm_set_enabled(platform_pio, sm_voltage_pwm, true);

    // Send PWM parameters: upper 16 bits = high_time, lower 16 bits = low_time
    uint32_t pwm_params = (high_cycles << 16) | low_cycles;
    pio_sm_put_blocking(platform_pio, sm_voltage_pwm, pwm_params);
}

uint32_t platform_get_voltage(void) {
    return config.voltage;
}

void platform_set_charge_time(uint32_t charge_ms) {
    config.charge_time_us = charge_ms * 1000;  // Convert ms to us
}

uint32_t platform_get_charge_time(void) {
    return config.charge_time_us / 1000;  // Convert us to ms
}

void platform_enable(void) {
    // Calculate charge cycles
    uint32_t clock_freq = clock_get_hz(clk_sys);
    uint32_t charge_cycles = (config.charge_time_us * clock_freq) / 1000000;

    // Configure platform enable PIO
    pio_sm_config c = platform_enable_program_get_default_config(offset_platform_enable);
    sm_config_set_set_pins(&c, config.hv_pin, 1);
    pio_gpio_init(platform_pio, config.hv_pin);
    pio_sm_set_consecutive_pindirs(platform_pio, sm_platform_enable, config.hv_pin, 1, true);
    pio_sm_init(platform_pio, sm_platform_enable, offset_platform_enable, &c);
    pio_sm_set_enabled(platform_pio, sm_platform_enable, true);

    // Send enable command with charge cycles
    pio_sm_put_blocking(platform_pio, sm_platform_enable, charge_cycles);
}

void platform_disable(void) {
    // Send disable command (0 cycles)
    pio_sm_put_blocking(platform_pio, sm_platform_enable, 0x00000000);

    // Also directly set pin low
    gpio_put(config.hv_pin, 0);
}

bool platform_get_status(void) {
    // Read armed pin
    return gpio_get(config.armed_pin);
}

void platform_set_pins(uint8_t hv_pin, uint8_t voltage_pin) {
    config.hv_pin = hv_pin;
    config.voltage_pin = voltage_pin;

    // Reinitialize pins
    gpio_init(config.hv_pin);
    gpio_set_dir(config.hv_pin, GPIO_OUT);
    gpio_put(config.hv_pin, 0);

    gpio_init(config.voltage_pin);
    gpio_set_dir(config.voltage_pin, GPIO_OUT);
}
