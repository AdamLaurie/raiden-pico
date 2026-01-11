#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

// UART Configuration
// Note: CLI uses USB CDC (no GPIO pins required)

#define CHIPSHOT_UART_ID uart0
#define CHIPSHOT_UART_TX_PIN 0
#define CHIPSHOT_UART_RX_PIN 1
#define CHIPSHOT_UART_BAUD 115200
#define CHIPSHOT_TRIGGER_PIN 7  // Hardware trigger output to ChipSHOUTER

// Reserved GPIO Pins
// GP0/GP1 used for ChipSHOUTER UART0
// GP4/GP5 used for Target UART1
// GP8/GP9 used for Grbl UART1 (alternate)
#define PIN_GLITCH_OUT 2       // Glitch pulse output (normal)
#define PIN_GLITCH_OUT_INV 11  // Glitch pulse output (inverted)
#define PIN_CLOCK 6            // Clock generator output
#define PIN_ARMED 16           // ARMED status (CPU-controlled, HIGH when armed)
#define PIN_GLITCH_FIRED 12    // GLITCH_FIRED signal (PIO0 pulses when glitch fires)

// Platform Types
typedef enum {
    PLATFORM_MANUAL = 0,
    PLATFORM_CHIPSHOUTER,
    PLATFORM_EMFI,
    PLATFORM_CROWBAR
} platform_type_t;

// Trigger Types
typedef enum {
    TRIGGER_NONE = 0,
    TRIGGER_GPIO,
    TRIGGER_UART
} trigger_type_t;

// Edge Types
typedef enum {
    EDGE_RISING = 0,
    EDGE_FALLING
} edge_type_t;

// Target Types
typedef enum {
    TARGET_NONE = 0,
    TARGET_LPC,
    TARGET_STM32
} target_type_t;

// System State Flags
typedef struct {
    bool armed;
    bool running;
    bool triggered;
    bool finished;
    bool error;
} system_flags_t;

// Glitch Configuration
typedef struct {
    uint32_t pause_cycles;    // Pause in system clock cycles (150MHz = 6.67ns per cycle)
    uint32_t width_cycles;    // Width in system clock cycles
    uint32_t gap_cycles;      // Gap in system clock cycles
    uint32_t count;
    trigger_type_t trigger;
    uint8_t trigger_pin;
    edge_type_t trigger_edge;
    uint8_t trigger_byte;
    // Output pins are hardwired: PIN_GLITCH_OUT (GP2) and PIN_GLITCH_OUT_INV (GP6)
} glitch_config_t;

// Platform Configuration
typedef struct {
    platform_type_t type;
    uint8_t hv_pin;
    uint8_t voltage_pin;
    uint8_t armed_pin;
    uint16_t voltage;
    uint32_t charge_time_us;
} platform_config_t;

// Target UART Configuration
typedef struct {
    uint8_t tx_pin;
    uint8_t rx_pin;
    uint32_t baudrate;
    bool initialized;
} target_uart_config_t;

// Target Reset Configuration
typedef struct {
    uint8_t pin;
    uint32_t period_ms;
    bool active_high;
    bool configured;
} target_reset_config_t;

// Target Power Configuration
typedef struct {
    uint8_t pin;
    uint32_t cycle_time_ms;
    bool configured;
} target_power_config_t;

// Clock Generator Configuration
typedef struct {
    uint8_t pin;
    uint32_t frequency;
    bool enabled;
} clock_config_t;

#endif // CONFIG_H
