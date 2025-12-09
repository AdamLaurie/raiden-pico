#include "glitch.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/structs/padsbank0.h"  // For direct PADS register access (ISO bit)
#include "pico/stdlib.h"
#include "glitch.pio.h"
#include <string.h>
#include <stdio.h>

// PIO and state machine allocations
static PIO glitch_pio = pio0;
static PIO clock_pio = pio1;     // Separate PIO for clock to avoid instruction memory overflow
static uint sm_edge_detect = 0;
static uint sm_pulse_gen = 1;
static uint sm_flag_output = 2;  // Can reuse for UART trigger since not used simultaneously
static uint sm_clock_gen = 0;    // Clock uses PIO1 SM0
static uint sm_uart_trigger = 2;  // Use SM2 instead of invalid SM4!

// PIO IRQ flag used for triggering (using IRQ 0 - shared between all SMs)
#define GLITCH_IRQ_NUM 0

// Configuration and state
static glitch_config_t config;
static system_flags_t flags;
static clock_config_t clock_config;
static uint32_t glitch_count = 0;
static volatile uint32_t pio_irq5_count = 0;  // Debug counter for PIO IRQ fires
static volatile bool clock_boost_enabled = false;  // Whether clock boost is active

// Pre-calculated cycle values for fast glitch execution
static uint32_t precalc_pause_cycles = 0;
static uint32_t precalc_width_cycles = 0;
static uint32_t precalc_gap_cycles = 0;

// PIO program offsets
static uint offset_edge_detect_rising;   // Rising edge detect program
static uint offset_edge_detect_falling;  // Falling edge detect program
static uint offset_pulse_gen;
static uint offset_clock_gen_delay;  // Clock generator with delay
static uint offset_clock_gen;        // Simple clock generator
static uint offset_clock_gen_boost;  // Clock generator with glitch boost
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
    config.trigger_pin = 3;  // Default trigger pin
    config.trigger_edge = EDGE_RISING;
    config.trigger_byte = 0x00;
    // Glitch output pins are hardwired: PIN_GLITCH_OUT (GP2) and PIN_GLITCH_OUT_INV (GP11)

    // Initialize flags
    memset(&flags, 0, sizeof(flags));

    // Initialize clock configuration
    memset(&clock_config, 0, sizeof(clock_config));
    clock_config.pin = PIN_CLOCK;  // GP6
    clock_config.frequency = 0;
    clock_config.enabled = false;

    // Initialize semaphore pins for clock boost coordination
    // GP9 (ARMED): CPU-controlled, HIGH when armed, LOW when disarmed
    gpio_init(PIN_ARMED);
    gpio_set_dir(PIN_ARMED, GPIO_OUT);
    gpio_put(PIN_ARMED, 0);  // Start disarmed

    // GP12 (GLITCH_FIRED): PIO0 sets HIGH when glitch fires, CPU clears LOW on next ARM
    gpio_init(PIN_GLITCH_FIRED);
    gpio_set_dir(PIN_GLITCH_FIRED, GPIO_OUT);
    gpio_put(PIN_GLITCH_FIRED, 0);  // Start LOW

    // Reset glitch count
    glitch_count = 0;
    pio_irq5_count = 0;

    // Load PIO programs for glitching (PIO0 - must fit in 32 instruction words!)
    // Core programs always loaded
    offset_pulse_gen = pio_add_program(glitch_pio, &pulse_generator_program);
    offset_irq_trigger = pio_add_program(glitch_pio, &irq_trigger_program);
    // Total: pulse_gen(12) + irq_trigger(1) = 13 instructions
    // Trigger programs (GPIO/UART) loaded dynamically in glitch_arm() based on trigger type

    printf("PIO0 init: pulse_gen@%u, irq_trigger@%u\n", offset_pulse_gen, offset_irq_trigger);

    // Load clock generator programs into PIO1 (separate instruction memory)
    offset_clock_gen_delay = pio_add_program(clock_pio, &clock_generator_delay_program);
    offset_clock_gen = pio_add_program(clock_pio, &clock_generator_program);
    offset_clock_gen_boost = pio_add_program(clock_pio, &clock_generator_with_boost_program);
    // Total PIO1: clock_delay(6) + clock(2) + clock_boost(19) = 27 instructions

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

// Output pins are hardwired - no need for setter function

bool glitch_arm(void) {
    if (flags.armed) {
        return false;  // Already armed
    }

    // Clear GLITCH_FIRED from previous trigger (if any)
    gpio_put(PIN_GLITCH_FIRED, 0);

    // Disable all trigger state machines first to ensure clean state
    pio_sm_set_enabled(glitch_pio, sm_edge_detect, false);
    pio_sm_set_enabled(glitch_pio, sm_uart_trigger, false);

    // Clear their FIFOs
    pio_sm_clear_fifos(glitch_pio, sm_edge_detect);
    pio_sm_clear_fifos(glitch_pio, sm_uart_trigger);

    // Remove ALL possible trigger programs to free PIO space before loading new one
    // This is simpler than tracking what's loaded - just remove everything
    pio_remove_program(glitch_pio, &gpio_edge_detect_rising_program, offset_edge_detect_rising);
    pio_remove_program(glitch_pio, &gpio_edge_detect_falling_program, offset_edge_detect_falling);
    pio_remove_program(glitch_pio, &uart_rx_decoder_program, offset_uart_match);

    // Configure trigger based on type
    if (config.trigger == TRIGGER_GPIO) {

        // Initialize GPIO trigger pin
        gpio_init(config.trigger_pin);
        gpio_set_dir(config.trigger_pin, GPIO_IN);
        gpio_pull_up(config.trigger_pin);  // Pull HIGH (typical for reset signals)

        // Load the appropriate edge detect program dynamically to save PIO space
        const pio_program_t *program = (config.trigger_edge == EDGE_RISING) ?
                                       &gpio_edge_detect_rising_program : &gpio_edge_detect_falling_program;

        // Check if program can be added
        if (!pio_can_add_program(glitch_pio, program)) {
            printf("ERROR: PIO0 is full, cannot load GPIO edge detect program!\n");
            return false;
        }

        uint program_offset = pio_add_program(glitch_pio, program);

        // Save offset for later reference
        if (config.trigger_edge == EDGE_RISING) {
            offset_edge_detect_rising = program_offset;
        } else {
            offset_edge_detect_falling = program_offset;
        }

        printf("GPIO edge detect: %s with debouncing (program offset=%u)\n",
               (config.trigger_edge == EDGE_RISING) ? "RISING" : "FALLING", program_offset);

        // Configure edge detect state machine (uses IRQ, no fire pin)
        // Use the program's get_default_config which has correct wrap settings + debounce
        pio_sm_config c_edge = (config.trigger_edge == EDGE_RISING) ?
                                gpio_edge_detect_rising_program_get_default_config(program_offset) :
                                gpio_edge_detect_falling_program_get_default_config(program_offset);
        sm_config_set_in_pins(&c_edge, config.trigger_pin);   // IN pins for 'wait' instruction
        sm_config_set_set_pins(&c_edge, PIN_GLITCH_FIRED, 1);  // SET pins for GP12 (GLITCH_FIRED)

        // Initialize GP12 as output for PIO0 (will set HIGH when trigger fires)
        pio_gpio_init(glitch_pio, PIN_GLITCH_FIRED);
        pio_sm_set_consecutive_pindirs(glitch_pio, sm_edge_detect, PIN_GLITCH_FIRED, 1, true);

        // Clear and initialize edge detect SM
        pio_sm_clear_fifos(glitch_pio, sm_edge_detect);
        pio_sm_restart(glitch_pio, sm_edge_detect);
        pio_sm_init(glitch_pio, sm_edge_detect, program_offset, &c_edge);
        pio_sm_set_enabled(glitch_pio, sm_edge_detect, true);

        // Save offset for disarm cleanup
        if (config.trigger_edge == EDGE_RISING) {
            offset_edge_detect_rising = program_offset;
        } else {
            offset_edge_detect_falling = program_offset;
        }
    }

    // Clear any pending IRQs before starting configuration
    pio_interrupt_clear(glitch_pio, GLITCH_IRQ_NUM);

    // Configure pulse generator FIRST (for all trigger types) - must be ready before trigger!
    pio_sm_config c_pulse = pulse_generator_program_get_default_config(offset_pulse_gen);
    // Set up normal glitch output (SET pins)
    sm_config_set_set_pins(&c_pulse, PIN_GLITCH_OUT, 1);
    // Set up inverted glitch output (SIDE pins)
    sm_config_set_sideset_pins(&c_pulse, PIN_GLITCH_OUT_INV);
    sm_config_set_clkdiv(&c_pulse, 1.0);  // Run at full system clock speed for precise timing

    // Initialize normal output pin (PIO control)
    pio_gpio_init(glitch_pio, PIN_GLITCH_OUT);
    pio_sm_set_consecutive_pindirs(glitch_pio, sm_pulse_gen, PIN_GLITCH_OUT, 1, true);

    // Initialize inverted output pin with hardware inversion (PIO control)
    pio_gpio_init(glitch_pio, PIN_GLITCH_OUT_INV);
    pio_sm_set_consecutive_pindirs(glitch_pio, sm_pulse_gen, PIN_GLITCH_OUT_INV, 1, true);
    gpio_set_outover(PIN_GLITCH_OUT_INV, GPIO_OVERRIDE_INVERT);  // Hardware inversion

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
    // FIFO order: PAUSE, COUNT, WIDTH, GAP (4 values total - fits in FIFO)
    // Note: Load COUNT-1 because the PIO loop executes (COUNT-1)+1 times
    pio_sm_put_blocking(glitch_pio, sm_pulse_gen, precalc_pause_cycles);
    pio_sm_put_blocking(glitch_pio, sm_pulse_gen, config.count > 0 ? config.count - 1 : 0);
    pio_sm_put_blocking(glitch_pio, sm_pulse_gen, precalc_width_cycles);
    pio_sm_put_blocking(glitch_pio, sm_pulse_gen, precalc_gap_cycles);

    // Enable PIO state machine - it will wait for IRQ 0 before executing
    pio_sm_set_enabled(glitch_pio, sm_pulse_gen, true);

    // NOW set up and enable the UART decoder if using UART trigger
    // It must be started AFTER pulse generator is ready to receive IRQ 5
    if (config.trigger == TRIGGER_UART) {
        // Load UART decoder program dynamically
        offset_uart_match = pio_add_program(glitch_pio, &uart_rx_decoder_program);

        // Configure UART RX decoder state machine
        pio_sm_config c_uart = uart_rx_decoder_program_get_default_config(offset_uart_match);

        // RP2350 GPIO Isolation Bit Handling:
        // GP5 is configured for UART function by target_uart.c. The RP2350 has a GPIO
        // isolation latch (ISO bit) that prevents changes to output control signals
        // from reaching the pad. This bit must be explicitly cleared for the PIO to
        // read GP5 while the hardware UART is also using it.
        //
        // According to RP2350 datasheet: "Input from the pin is visible to all
        // peripherals regardless of the function select setting" - but only if the
        // ISO bit is cleared. The SDK gpio_set_function() clears ISO for the selected
        // function, but doesn't clear it globally for all peripherals reading the pin.
        hw_clear_bits(&padsbank0_hw->io[5], PADS_BANK0_GPIO0_ISO_BITS);

        // Configure GP5 as input for PIO monitoring (same pin as hardware UART RX)
        sm_config_set_in_pins(&c_uart, 5);  // IN pins base = GP5 (for 'in pins, 1')
        sm_config_set_jmp_pin(&c_uart, 5);  // JMP pin = GP5 (for 'wait pin 0')
        sm_config_set_in_shift(&c_uart, true, false, 32);  // Shift RIGHT, NO autopush
        // Note: Using ISR directly for comparison, no autopush needed
        sm_config_set_set_pins(&c_uart, PIN_GLITCH_FIRED, 1);  // SET pins for GP12 (GLITCH_FIRED)

        // Initialize GP12 as output for PIO0 (will set HIGH when trigger fires)
        pio_gpio_init(glitch_pio, PIN_GLITCH_FIRED);
        pio_sm_set_consecutive_pindirs(glitch_pio, sm_uart_trigger, PIN_GLITCH_FIRED, 1, true);

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

    // Load clock boost FIFO parameters during ARM transition (DISARM→ARM)
    // This prevents boost from triggering immediately on clock enable
    if (clock_boost_enabled && clock_config.enabled) {
        // Calculate normal half-period from clock frequency
        uint32_t system_clock = clock_get_hz(clk_sys);
        uint32_t target_half_period = (system_clock / 2) / clock_config.frequency;

        // Load FIFO with boost parameters (pulled when GLITCH_FIRED goes HIGH)
        // First push: boost count (pulled at boost_entry)
        // Second push: normal period (pulled after boost completes to restore Y)
        pio_sm_put_blocking(clock_pio, sm_clock_gen, config.count);
        pio_sm_put_blocking(clock_pio, sm_clock_gen, target_half_period - 1);
    }

    // Set ARMED status HIGH (GP9)
    gpio_put(PIN_ARMED, 1);

    flags.armed = true;
    return true;
}

void glitch_disarm(void) {
    if (!flags.armed) {
        return;
    }

    // Clear ARMED status LOW (GP9)
    gpio_put(PIN_ARMED, 0);

    // Disable PIO state machines
    pio_sm_set_enabled(glitch_pio, sm_edge_detect, false);
    pio_sm_set_enabled(glitch_pio, sm_pulse_gen, false);
    pio_sm_set_enabled(glitch_pio, sm_uart_trigger, false);

    // Note: We don't remove programs here - they'll be removed on next arm
    // This keeps disarm simple and fast

    // Clear any pending IRQs to prevent false triggers on next arm
    pio_interrupt_clear(glitch_pio, GLITCH_IRQ_NUM);

    // Clear FIFOs to ensure clean state for next arm
    pio_sm_clear_fifos(glitch_pio, sm_edge_detect);
    pio_sm_clear_fifos(glitch_pio, sm_pulse_gen);
    pio_sm_clear_fifos(glitch_pio, sm_uart_trigger);

    // No need to restore GP5 - PIO was just snooping, UART function never changed
    // Output pins are controlled by PIO, will return to idle state automatically

    flags.armed = false;
}

bool glitch_execute(void) {
    // Manual glitch execution (for TRIGGER_NONE mode or manual testing)
    if (!flags.armed) {
        return false;
    }

    // Trigger IRQ 0 (glitch) via irq_trigger program, pulse GP9 for clock boost
    // Use sm_flag_output (SM2) to trigger
    // SM2 is free when TRIGGER_NONE is used (UART trigger uses SM2 only for TRIGGER_UART)
    pio_sm_config c_irq = irq_trigger_program_get_default_config(offset_irq_trigger);
    sm_config_set_set_pins(&c_irq, PIN_GLITCH_FIRED, 1);  // SET pins for GP12 (GLITCH_FIRED)

    // Initialize GP12 as output for PIO0 (will set HIGH when GLITCH command fires)
    pio_gpio_init(glitch_pio, PIN_GLITCH_FIRED);
    pio_sm_set_consecutive_pindirs(glitch_pio, sm_flag_output, PIN_GLITCH_FIRED, 1, true);

    pio_sm_init(glitch_pio, sm_flag_output, offset_irq_trigger, &c_irq);
    pio_sm_set_enabled(glitch_pio, sm_flag_output, true);

    // Wait for it to execute one cycle
    busy_wait_us(1);

    // Disable it
    pio_sm_set_enabled(glitch_pio, sm_flag_output, false);

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

    // Output pins controlled by PIO, no GPIO reset needed
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

// Clock generator control functions
void clock_set_frequency(uint32_t freq_hz) {
    bool was_enabled = clock_config.enabled;

    // Disable clock if it was running
    if (was_enabled) {
        clock_disable();
    }

    // Update frequency
    clock_config.frequency = freq_hz;

    // Re-enable if it was running
    if (was_enabled) {
        clock_enable();
    }
}

void clock_enable(void) {
    if (clock_config.enabled) {
        return;  // Already enabled
    }

    if (clock_config.frequency == 0) {
        return;  // No frequency set
    }

    // Disable first to ensure clean state
    pio_sm_set_enabled(clock_pio, sm_clock_gen, false);
    pio_sm_clear_fifos(clock_pio, sm_clock_gen);

    // Initialize GPIO for clock output
    pio_gpio_init(clock_pio, clock_config.pin);
    pio_sm_set_consecutive_pindirs(clock_pio, sm_clock_gen, clock_config.pin, 1, true);

    // System clock is 150MHz (6.67ns per cycle)
    // Target frequency determines cycles per half-period
    uint32_t system_clock = clock_get_hz(clk_sys);
    uint32_t target_half_period = (system_clock / 2) / clock_config.frequency;

    // Configure boost-capable clock generator
    pio_sm_config c = clock_generator_with_boost_program_get_default_config(offset_clock_gen_boost);
    sm_config_set_set_pins(&c, clock_config.pin, 1);  // SET pins for clock output
    sm_config_set_jmp_pin(&c, PIN_GLITCH_FIRED);      // JMP pin = GP12 (GLITCH_FIRED signal)
    sm_config_set_in_pins(&c, PIN_GLITCH_FIRED);      // IN pins for WAIT instruction on GP12
    sm_config_set_clkdiv(&c, 1.0);  // Full speed

    // Clear ISO bit so PIO1 can read GP12 (set by PIO0, but readable by PIO1)
    hw_clear_bits(&padsbank0_hw->io[PIN_GLITCH_FIRED], PADS_BANK0_GPIO0_ISO_BITS);

    pio_sm_init(clock_pio, sm_clock_gen, offset_clock_gen_boost, &c);

    // Initialize Y (normal period) and ISR (fast period) registers manually
    uint32_t fast_half_period = (target_half_period / 2);  // 2x frequency
    pio_sm_put_blocking(clock_pio, sm_clock_gen, target_half_period - 1);
    pio_sm_exec(clock_pio, sm_clock_gen, pio_encode_pull(false, false));
    pio_sm_exec(clock_pio, sm_clock_gen, pio_encode_mov(pio_y, pio_osr));

    pio_sm_put_blocking(clock_pio, sm_clock_gen, fast_half_period - 1);
    pio_sm_exec(clock_pio, sm_clock_gen, pio_encode_pull(false, false));
    pio_sm_exec(clock_pio, sm_clock_gen, pio_encode_mov(pio_isr, pio_osr));

    // FIFO loading moved to glitch_arm() to ensure it only loads during DISARM→ARM transition

    // Enable the state machine
    pio_sm_set_enabled(clock_pio, sm_clock_gen, true);

    // Enable clock boost feature
    clock_boost_enabled = true;
    clock_config.enabled = true;
}

void clock_disable(void) {
    if (!clock_config.enabled) {
        return;
    }

    // Disable clock boost
    clock_boost_enabled = false;

    // Disable state machine
    pio_sm_set_enabled(clock_pio, sm_clock_gen, false);
    pio_sm_clear_fifos(clock_pio, sm_clock_gen);

    // Set clock pin LOW
    gpio_put(clock_config.pin, 0);

    clock_config.enabled = false;
}

bool clock_is_enabled(void) {
    return clock_config.enabled;
}

uint32_t clock_get_frequency(void) {
    return clock_config.frequency;
}
