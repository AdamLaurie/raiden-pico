#ifndef UART_MUX_H
#define UART_MUX_H

// Hardware-UART ownership arbiter (UART0 + UART1).
//
// The RP2350 has two UART peripherals, but each can be routed to several GPIO
// pairs ("alternates"). Raiden multiplexes them between subsystems:
//
//   UART0: ChipSHOUTER  GP0/GP1
//   UART1: Target       GP4/GP5   (bootloaders, SEND, TERM)
//          GRBL         GP8/GP9
//          PSU          GP10/GP11 (via MAX3232)
//
// One peripheral = ONE baud/format/FIFO/IRQ. If two subsystems each call
// uart_init() on it, the second silently re-programs the first's link, and any
// pins still selected to UART keep transmitting (TX) / feeding the receiver (RX).
// So every user goes through this module:
//
//   uart_mux_claim(who)   route `who`'s pins, apply its stored line settings, run
//                         its attach hook. Detaches the previous owner first and
//                         parks that owner's pins (never left selected to UART).
//   uart_mux_borrow(who)  claim for a short transaction; returns the previous owner
//   uart_mux_restore(p)   ...to hand the UART back afterwards (settings + hooks
//                         re-applied), so e.g. a PSU query in the middle of a
//                         target session is invisible to the target code.
//   uart_mux_release(who) give the UART up (pins parked, peripheral off).
//
// ADDING AN OWNER (another device, or a different alternate pair — RP2350 UART0
// can also sit on GP12/13, GP16/17, GP28/29; UART1 on GP20/21, GP24/25, GP26/27):
//   1. add a uart_owner_t value below,
//   2. add its row to the owner table in uart_mux.c (uart, pins, idle policy),
//   3. give it hooks if it uses the UART IRQ or keeps session state,
//   4. check the pins against config.h — several alternates collide with pins
//      already in use (GP12 crowbar gate, GP13/14 BOOT0/1, GP16 ARMED, GP17 SWCLK).

#include <stdbool.h>
#include <stdint.h>
#include "hardware/uart.h"

typedef enum {
    UART_OWNER_NONE = 0,
    UART_OWNER_CHIPSHOT,   // UART0 GP0/GP1
    UART_OWNER_TARGET,     // UART1 GP4/GP5
    UART_OWNER_GRBL,       // UART1 GP8/GP9
    UART_OWNER_PSU,        // UART1 GP10/GP11
    UART_OWNER_COUNT
} uart_owner_t;

// What an owner's pins do while it does NOT hold the UART.
typedef enum {
    UART_PARK_HIZ = 0,     // TX + RX plain inputs (don't back-power an unpowered target)
    UART_PARK_TX_HIGH,     // TX driven HIGH (line idle/mark), RX input — for a level
                           // shifter (MAX3232) whose input must never float
} uart_park_t;

typedef void (*uart_mux_hook_t)(void);

// Attach runs after the pins are routed and the line settings applied (enable the
// owner's IRQ, etc.); detach runs before the owner loses the UART (disable IRQ,
// drop session state). Either may be NULL.
void uart_mux_set_hooks(uart_owner_t who, uart_mux_hook_t attach, uart_mux_hook_t detach);

// Store an owner's line settings; applied live if it currently owns its UART.
void uart_mux_set_line(uart_owner_t who, uint32_t baud, uint data_bits,
                       uart_parity_t parity, uint stop_bits);
// Change only the baud / only the frame format (the other half is kept); live if owner.
void uart_mux_set_baud(uart_owner_t who, uint32_t baud);
void uart_mux_set_format(uart_owner_t who, uint data_bits, uart_parity_t parity, uint stop_bits);
uint32_t uart_mux_get_baud(uart_owner_t who);
// e.g. "8E1"
const char *uart_mux_line_str(uart_owner_t who);

// Claim the UART for `who`. No-op if it already owns it. Returns true if it switched.
bool uart_mux_claim(uart_owner_t who);
// Claim and fully re-initialise even if `who` already owns it (fresh FIFO + settings).
void uart_mux_reclaim(uart_owner_t who);
// Release if `who` owns its UART (detach, peripheral off, pins parked). No-op otherwise.
void uart_mux_release(uart_owner_t who);
// Park `who`'s pins (only if it does not own its UART) — e.g. when a subsystem is
// enabled but does not need the UART yet.
void uart_mux_park(uart_owner_t who);

// Short-transaction access. Returns the owner to restore (UART_OWNER_NONE if `who`
// already owned it or the UART was free — then restore is a no-op and `who` keeps it).
uart_owner_t uart_mux_borrow(uart_owner_t who);
void uart_mux_restore(uart_owner_t prev);

bool uart_mux_is_owner(uart_owner_t who);
uart_owner_t uart_mux_owner(uint uart_num);          // 0 or 1
uart_inst_t *uart_mux_uart(uart_owner_t who);
const char *uart_mux_name(uart_owner_t who);
void uart_mux_pins(uart_owner_t who, uint8_t *tx, uint8_t *rx);

// UART owner that uses `pin` (any registered alternate), or UART_OWNER_NONE.
uart_owner_t uart_mux_pin_owner(uint pin);

#endif // UART_MUX_H
