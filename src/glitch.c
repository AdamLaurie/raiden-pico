#include "glitch.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"
#include "glitch.pio.h"
#include <string.h>

// PIO and state machine allocations
static PIO glitch_pio = pio0;
static uint sm_edge_detect = 0;
static uint sm_pulse_gen = 1;
static uint sm_flag_output = 2;  // Can reuse for UART trigger since not used simultaneously
static uint sm_clock_gen = 3;
static uint sm_uart_trigger = 2;  // Use SM2 instead of invalid SM4!

// PIO IRQ flag used for triggering (using IRQ 0 - shared between all SMs)
#define GLITCH_IRQ_NUM 0

// Configuration and state
static glitch_config_t config;
static system_flags_t flags;
static uint32_t glitch_count = 0;
static volatile uint32_t pio_irq5_count = 0;  // Debug counter for PIO IRQ fires

// Pre-calculated cycle values for fast glitch execution
static uint32_t precalc_pause_cycles = 0;
static uint32_t precalc_width_cycles = 0;
static uint32_t precalc_gap_cycles = 0;

// PIO program offsets
static uint offset_edge_detect;
static uint offset_pulse_gen;
static uint offset_flag_output;
static uint offset_clock_gen;
static uint offset_uart_match;
static uint offset_irq_trigger;

void glitch_init(void) {
    // Initialize configuration with defaults
    memset(&config, 0, sizeof(config));
    config.pause_cycles = 0;     // No pause by default for minimum latency
    config.width_cycles = 100;   // 100 cycles = 0.67us at 150MHz
    config.gap_cycles = 100;     // 100 cycles = 0.67us at 150MHz
    config.count = 1;
    config.trigger = TRIGGER_NONE;
    config.output_pin = 2;  // Default glitch output pin
    config.trigger_pin = 3;  // Default trigger pin
    config.trigger_edge = EDGE_RISING;
    config.trigger_byte = 0x00;

    // Initialize flags
    memset(&flags, 0, sizeof(flags));

    // Reset glitch count
    glitch_count = 0;
    pio_irq5_count = 0;

    // Load PIO programs
    offset_edge_detect = pio_add_program(glitch_pio, &gpio_edge_detect_program);
    offset_pulse_gen = pio_add_program(glitch_pio, &pulse_generator_program);
    offset_flag_output = pio_add_program(glitch_pio, &flag_output_program);
    offset_clock_gen = pio_add_program(glitch_pio, &clock_generator_program);
    offset_uart_match = pio_add_program(glitch_pio, &uart_rx_decoder_program);
    offset_irq_trigger = pio_add_program(glitch_pio, &irq_trigger_program);

    // PIO state machines are configured and started in glitch_arm() based on trigger type
    // Output pin is configured for PIO control in glitch_arm()
}

glitch_config_t* glitch_get_config(void) {
    return &config;
}

system_flags_t* glitch_get_flags(void) {
    return &flags;
}

void glitch_set_pause(uint32_t pause_cycles) {
    config.pause_cycles = pause_cycles;
}

void glitch_set_width(uint32_t width_cycles) {
    config.width_cycles = width_cycles;
}

void glitch_set_gap(uint32_t gap_cycles) {
    config.gap_cycles = gap_cycles;
}

void glitch_set_count(uint32_t count) {
    config.count = count;
}

void glitch_set_trigger_type(trigger_type_t type) {
    config.trigger = type;
}

void glitch_set_trigger_pin(uint8_t pin, edge_type_t edge) {
    config.trigger_pin = pin;
    config.trigger_edge = edge;
}

void glitch_set_trigger_byte(uint8_t byte) {
    config.trigger_byte = byte;
}

void glitch_set_output_pin(uint8_t pin) {
    config.output_pin = pin;
}

bool glitch_arm(void) {
    if (flags.armed) {
        return false;  // Already armed
    }

    // Disable all trigger state machines first to ensure clean state
    pio_sm_set_enabled(glitch_pio, sm_edge_detect, false);
    pio_sm_set_enabled(glitch_pio, sm_uart_trigger, false);

    // Clear their FIFOs
    pio_sm_clear_fifos(glitch_pio, sm_edge_detect);
    pio_sm_clear_fifos(glitch_pio, sm_uart_trigger);

    // Configure trigger based on type
    if (config.trigger == TRIGGER_GPIO) {
        // Initialize GPIO trigger pin
        gpio_init(config.trigger_pin);
        gpio_set_dir(config.trigger_pin, GPIO_IN);
        gpio_pull_up(config.trigger_pin);

        // Configure edge detect PIO
        pio_sm_config c_edge;
        if (config.trigger_edge == EDGE_RISING) {
            c_edge = gpio_edge_detect_program_get_default_config(offset_edge_detect);
        } else {
            c_edge = gpio_falling_detect_program_get_default_config(offset_edge_detect);
        }
        sm_config_set_in_pins(&c_edge, config.trigger_pin);
        pio_sm_init(glitch_pio, sm_edge_detect, offset_edge_detect, &c_edge);
        pio_sm_set_enabled(glitch_pio, sm_edge_detect, true);

    }

    // Clear any pending IRQs before starting configuration
    pio_interrupt_clear(glitch_pio, GLITCH_IRQ_NUM);

    // Configure pulse generator FIRST (for all trigger types) - must be ready before UART decoder!
    pio_sm_config c_pulse = pulse_generator_program_get_default_config(offset_pulse_gen);
    sm_config_set_set_pins(&c_pulse, config.output_pin, 1);
    sm_config_set_clkdiv(&c_pulse, 1.0);  // Run at full system clock speed for precise timing
    pio_gpio_init(glitch_pio, config.output_pin);
    pio_sm_set_consecutive_pindirs(glitch_pio, sm_pulse_gen, config.output_pin, 1, true);

    // Clear and initialize pulse generator - MUST be done every ARM for clean state
    pio_sm_clear_fifos(glitch_pio, sm_pulse_gen);
    pio_sm_restart(glitch_pio, sm_pulse_gen);
    pio_sm_init(glitch_pio, sm_pulse_gen, offset_pulse_gen, &c_pulse);

    // Use cycle values directly - no conversion needed
    // System runs at 150MHz, so 1 cycle = 6.67ns
    precalc_pause_cycles = config.pause_cycles;
    precalc_width_cycles = config.width_cycles;
    precalc_gap_cycles = config.gap_cycles;

    // Account for PIO instruction overhead (optional, can be tuned)
    if (precalc_width_cycles > 5) precalc_width_cycles -= 5;
    if (precalc_gap_cycles > 5) precalc_gap_cycles -= 5;

    // Pre-load PIO FIFO with timing values so they're ready when IRQ fires
    // For PIO-based UART trigger, this enables hardware-only triggering with no CPU involvement
    pio_sm_put_blocking(glitch_pio, sm_pulse_gen, precalc_pause_cycles);
    for (uint32_t i = 0; i < config.count; i++) {
        pio_sm_put_blocking(glitch_pio, sm_pulse_gen, precalc_width_cycles);
        pio_sm_put_blocking(glitch_pio, sm_pulse_gen, precalc_gap_cycles);
    }

    // Enable PIO state machine - it will wait for IRQ 5 before executing
    pio_sm_set_enabled(glitch_pio, sm_pulse_gen, true);

    // NOW set up and enable the UART decoder if using UART trigger
    // It must be started AFTER pulse generator is ready to receive IRQ 5
    if (config.trigger == TRIGGER_UART) {
        // RP2350B SILICON BUG WORKAROUND: GP5→GP28 jumper required
        // Bug: PIO and UART peripheral cannot coexist on the same GPIO pin
        // - UART1 hardware uses GP5 (for target RX/TX communication)
        // - PIO monitors GP28 (for UART trigger detection)
        // - Jumper wire connects GP5→GP28 to share the UART RX signal
        // Hardware setup: Jumper wire from GP5 (Pin 7) to GP28 (Pin 34)

        // Configure UART RX decoder state machine
        pio_sm_config c_uart = uart_rx_decoder_program_get_default_config(offset_uart_match);

        // Configure GP28 for PIO0 usage - use pio_gpio_init for proper PIO control
        pio_gpio_init(glitch_pio, 28);
        // Set as input (false = input, true = output)
        pio_sm_set_consecutive_pindirs(glitch_pio, sm_uart_trigger, 28, 1, false);

        // IMPORTANT: Disable pullup/pulldown on GP28 to allow jumper signal through
        gpio_set_pulls(28, false, false);  // No pullup, no pulldown

        // Configure GP28 as input for PIO monitoring (jumpered from GP5)
        sm_config_set_in_pins(&c_uart, 28);  // IN pins base = GP28 (for 'in pins, 1')
        sm_config_set_jmp_pin(&c_uart, 28);  // JMP pin = GP28 (for 'wait pin 0')
        sm_config_set_in_shift(&c_uart, true, false, 32);  // Shift RIGHT, NO autopush
        // Note: Using ISR directly for comparison, no autopush needed

        // Clock at 8× baud rate for proper UART sampling
        // System clock: 150 MHz, Target: 8 × 115200 = 921600 Hz
        float clkdiv = 150000000.0f / (8.0f * 115200.0f);  // = 162.76
        sm_config_set_clkdiv(&c_uart, clkdiv);

        // Clear and initialize state machine
        pio_sm_clear_fifos(glitch_pio, sm_uart_trigger);
        pio_sm_restart(glitch_pio, sm_uart_trigger);
        pio_sm_init(glitch_pio, sm_uart_trigger, offset_uart_match, &c_uart);

        // Load trigger byte into FIFO
        // With right shift autopush at 8 bits, byte ends in [24:31]
        // So send trigger byte in same position for comparison
        uint32_t trigger_word = ((uint32_t)config.trigger_byte) << 24;  // Byte in bits [24:31]
        pio_sm_put_blocking(glitch_pio, sm_uart_trigger, trigger_word);

        // Clear IRQ flag before enabling to avoid false trigger
        pio_interrupt_clear(glitch_pio, GLITCH_IRQ_NUM);

        // Enable the UART decoder - pulse generator is now ready to receive IRQ
        pio_sm_set_enabled(glitch_pio, sm_uart_trigger, true);
    }

    flags.armed = true;
    return true;
}

void glitch_disarm(void) {
    if (!flags.armed) {
        return;
    }

    // Disable PIO state machines
    pio_sm_set_enabled(glitch_pio, sm_edge_detect, false);
    pio_sm_set_enabled(glitch_pio, sm_pulse_gen, false);
    pio_sm_set_enabled(glitch_pio, sm_uart_trigger, false);

    // Clear any pending IRQs to prevent false triggers on next arm
    pio_interrupt_clear(glitch_pio, GLITCH_IRQ_NUM);

    // Clear FIFOs to ensure clean state for next arm
    pio_sm_clear_fifos(glitch_pio, sm_edge_detect);
    pio_sm_clear_fifos(glitch_pio, sm_pulse_gen);
    pio_sm_clear_fifos(glitch_pio, sm_uart_trigger);

    // No need to restore GP5 - PIO was just snooping, UART function never changed

    // Reset output pin
    gpio_put(config.output_pin, 0);

    flags.armed = false;
}

bool glitch_execute(void) {
    // Manual glitch execution (for TRIGGER_NONE mode or manual testing)
    // For GPIO and UART triggers, PIO handles everything autonomously
    if (!flags.armed) {
        return false;
    }

    // Use sm_clock_gen (which is unused) to trigger IRQ 5
    // Configure and enable it briefly with the irq_trigger program
    pio_sm_config c_irq = irq_trigger_program_get_default_config(offset_irq_trigger);
    pio_sm_init(glitch_pio, sm_clock_gen, offset_irq_trigger, &c_irq);
    pio_sm_set_enabled(glitch_pio, sm_clock_gen, true);

    // Wait for it to execute one cycle
    busy_wait_us(1);

    // Disable it
    pio_sm_set_enabled(glitch_pio, sm_clock_gen, false);

    // Increment glitch count
    glitch_count++;

    // Properly disarm after executing glitch (cleans up PIO state machines)
    glitch_disarm();

    return true;
}

void glitch_reset(void) {
    // Disarm if armed
    if (flags.armed) {
        glitch_disarm();
    }

    // Reset configuration to defaults
    config.pause_cycles = 0;     // No pause by default for minimum latency
    config.width_cycles = 100;   // 100 cycles = 0.67us at 150MHz
    config.gap_cycles = 100;     // 100 cycles = 0.67us at 150MHz
    config.count = 1;
    config.trigger = TRIGGER_NONE;

    // Reset flags
    memset(&flags, 0, sizeof(flags));

    // Reset glitch count
    glitch_count = 0;

    // Reset output pin
    gpio_put(config.output_pin, 0);
}

uint32_t glitch_get_count(void) {
    // Check if PIO-triggered glitch has fired (for UART/GPIO triggers)
    // When pulse generator fires, it consumes all FIFO data
    // If FIFO is empty and we were armed, a glitch fired
    if (flags.armed && (config.trigger == TRIGGER_UART || config.trigger == TRIGGER_GPIO)) {
        // Check if pulse generator FIFO is empty (means it fired)
        if (pio_sm_is_tx_fifo_empty(glitch_pio, sm_pulse_gen)) {
            // Glitch fired! Increment count and disarm
            glitch_count++;
            flags.armed = false;

            // Disable the trigger state machines since we fired
            if (config.trigger == TRIGGER_UART) {
                pio_sm_set_enabled(glitch_pio, sm_uart_trigger, false);
            } else if (config.trigger == TRIGGER_GPIO) {
                pio_sm_set_enabled(glitch_pio, sm_edge_detect, false);
            }
            pio_sm_set_enabled(glitch_pio, sm_pulse_gen, false);
        }
    }
    return glitch_count;
}

uint32_t glitch_get_irq5_count(void) {
    return pio_irq5_count;
}

// Called from target_uart_process() for each received byte (for debugging/logging only)
// With PIO-based UART triggering, actual glitch triggering happens in hardware
void glitch_check_uart_trigger(uint8_t byte) {
    // PIO handles all triggering in hardware now
    // This function is kept for compatibility but does nothing
    // Software glitch counter is no longer incremented (glitch happens in hardware)
    (void)byte;  // Unused
}

void glitch_update_flags(void) {
    // Update flag outputs if configured
    // This can be called periodically from main loop

    // Check if glitch fired and auto-disarm if needed
    // This calls glitch_get_count() which checks FIFO empty and disarms
    glitch_get_count();

    // Note: UART trigger is now handled in target_uart_process()
    // where hardware UART data is already being read

    // Update LED or other status indicators based on flags
    if (flags.armed) {
        // Blink LED or set status pin
    }
}
