#ifndef PSU_H
#define PSU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// External programmable PSU control (TENMA 72-2540 / Korad ASCII protocol).
//
// The PSU is driven over UART1 routed to GP10/GP11 (RP2350 alternate funcsel),
// which are ALSO GP10/11 of the target power group (GP10/11/12). Claiming the PSU
// UART releases those two pins; while it holds them the PSU becomes the target
// supply: TARGET POWER ON/OFF/CYCLE and the auto power-on are routed to the PSU
// output (psu_supply_* below), TARGET POWER INT refuses, and TARGET POWER EXT
// works (it only touches the GP12 crowbar gate). A MAX3232 (or equivalent) RS-232
// transceiver sits between the PSU DB9 and these 3.3V pins.
//
// Protocol notes (Korad/TENMA): commands are ASCII with no terminator; queries
// return fixed-ish-length ASCII with no terminator after a short latency. These
// are bench-tunable (see PSU_CMD_GAP_MS / read timeouts in psu.c).

// Claim GP10/11 for the PSU UART. Returns false (and emits a CLI error) if the
// onboard target power group is currently ON. Idempotent once claimed.
bool psu_begin(void);

// Release GP10/11 back to the target power group.
void psu_release(void);

// True when GP10/11 are currently in PSU-UART mode.
bool psu_is_active(void);

// Runtime UART baud control. Default is PSU_UART_BAUD (config.h). psu_set_baud()
// validates the rate (1200..921600), stores it for the next psu_begin(), and
// re-applies it live if the UART is already claimed; returns false on an
// out-of-range rate.
uint32_t psu_get_baud(void);
bool psu_set_baud(uint32_t baud);

// High-level commands. Each calls psu_begin() first; on a claim failure they
// return false without sending anything.
bool psu_set_voltage_mv(uint32_t mv);   // VSET1:NN.NN
bool psu_set_current_ma(uint32_t ma);   // ISET1:N.NNN
bool psu_output(bool on);               // OUT1 / OUT0

// Queries. Write an ASCII, NUL-terminated response into buf; return the number
// of bytes received (0 = no response / not connected). buf is always terminated.
int psu_id(char *buf, size_t buflen);            // *IDN?
int psu_read_voltage(char *buf, size_t buflen);  // VOUT1? (delivered at the output)
int psu_read_current(char *buf, size_t buflen);  // IOUT1? (delivered at the output)

// Set-point (configured) queries — what the PSU is PROGRAMMED to, as opposed to
// what it is currently delivering. Needed because the onboard controller has no
// local memory of values pushed with PSU VOLT / PSU CURR.
int psu_read_setpoint_voltage(char *buf, size_t buflen);  // VSET1?
int psu_read_setpoint_current(char *buf, size_t buflen);  // ISET1?

// STATUS? — reads the 1-byte status. Returns true on a byte received.
// Bits verified on a TENMA 72-2540 V5.9:
//   bit0 = CH1 mode CV(1)/CC(0)   bit4 = beep on(1)/off(0)
//   bit5 = key lock (1)           bit6 = output ON(1)/OFF(0)
// (bit1 reads 1 on this single-channel unit; bits 2,3,7 unused.)
bool psu_read_status(uint8_t *status_out);

// Target-supply helpers used by the TARGET POWER routing (target_uart.c).
// psu_supply_state: output on/off from the cached state (last OUT sent / STATUS?
//   read), querying STATUS? only when unknown. False = no response.
// psu_supply_set: OUT1/OUT0, then confirm via STATUS? bit6. False = no response
//   or the PSU did not report the requested state.
// psu_setpoint_mv: VSET1? in mV, -1 on no response.
// psu_wait_vout: poll VOUT1? until it is >= (rising) or <= (falling) threshold_mv
//   or timeout_ms passes; returns the last reading in mV (-1 = never answered).
bool psu_supply_state(bool *on);
bool psu_supply_set(bool on);
int32_t psu_setpoint_mv(void);
int32_t psu_wait_vout(bool rising, int32_t threshold_mv, uint32_t timeout_ms);

#endif // PSU_H
