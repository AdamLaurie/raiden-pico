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
#define PIN_FLAG_ERROR 2
#define PIN_FLAG_RUNNING 3
#define PIN_FLAG_TRIGGERED 4
#define PIN_FLAG_FINISHED 5
#define PIN_CLOCK 6

// PIO State Machine Allocation
#define SM_TRIGGER 0
#define SM_PULSE 1
#define SM_FLAGS 2
#define SM_CLOCK 3
#define SM_VOLTAGE 4
#define SM_HV_ENABLE 5
#define SM_STATUS 6
#define SM_TARGET_TX 7

// Buffer Sizes
#define CMD_BUFFER_SIZE 256
#define RESPONSE_BUFFER_SIZE 256
#define UART_RX_BUFFER_SIZE 128

// Glitch Configuration Limits
#define MAX_PAUSE_US 1000000
#define MAX_WIDTH_US 1000000
#define MAX_GAP_US 1000000
#define MAX_COUNT 1000
#define MAX_VOLTAGE 500

// PIO Clock Frequency
#define PIO_CLOCK_HZ 1000000  // 1 MHz for microsecond timing

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
    uint8_t output_pin;
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

#endif // CONFIG_H
