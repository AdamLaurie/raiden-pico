#ifndef PSU_H
#define PSU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// External programmable PSU control (TENMA 72-2540 / Korad ASCII protocol).
//
// The PSU is driven over UART1 routed to GP10/GP11 (RP2350 alternate funcsel),
// which are ALSO the target power group (GP10/11/12). The two uses are mutually
// exclusive: claiming the PSU UART releases the power group, and the power-group
// commands refuse while the PSU holds the pins. A MAX3232 (or equivalent) RS-232
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

// High-level commands. Each calls psu_begin() first; on a claim failure they
// return false without sending anything.
bool psu_set_voltage_mv(uint32_t mv);   // VSET1:NN.NN
bool psu_set_current_ma(uint32_t ma);   // ISET1:N.NNN
bool psu_output(bool on);               // OUT1 / OUT0

// Queries. Write an ASCII, NUL-terminated response into buf; return the number
// of bytes received (0 = no response / not connected). buf is always terminated.
int psu_id(char *buf, size_t buflen);            // *IDN?
int psu_read_voltage(char *buf, size_t buflen);  // VOUT1?
int psu_read_current(char *buf, size_t buflen);  // IOUT1?

// STATUS? — reads the 1-byte status. Returns true on a byte received.
// Bits verified on a TENMA 72-2540 V5.9:
//   bit0 = CH1 mode CV(1)/CC(0)   bit4 = beep on(1)/off(0)
//   bit5 = key lock (1)           bit6 = output ON(1)/OFF(0)
// (bit1 reads 1 on this single-channel unit; bits 2,3,7 unused.)
bool psu_read_status(uint8_t *status_out);

#endif // PSU_H
