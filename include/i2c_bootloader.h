#ifndef I2C_BOOTLOADER_H
#define I2C_BOOTLOADER_H

#include <stdint.h>
#include <stdbool.h>

// STM32 system bootloader over I2C (protocol per AN4221).
//
// Bit-banged I2C master reusing the target UART1 pins — the STM32 bootloader
// locks to a single interface, so UART-boot and I2C-boot can never co-exist:
//   SCL = GP4 (= TARGET_UART TX),  SDA = GP5 (= TARGET_UART RX).
// Default 7-bit slave address 0x39 (decoded from the F401 boot ROM: OAR1=0x4072).
//
// The F401 boot ROM initialises and scans I2C1/I2C2/I2C3 even though AN2606 does
// not document an I2C boot interface for the F401 — reachability is the thing
// this module is here to test. See stm32_payloads/f4/stm32f401_bootrom_analysis.md.

#define I2C_BL_ADDR7_DEFAULT 0x39

// Reset the target into the system bootloader (BOOT0=1, BOOT1=0, pulse nRST) and
// switch GP4/GP5 to the bit-banged I2C master. Must be called before any command
// and MUST NOT be mixed with a USART 0x7F sync (that would select the UART iface).
void i2c_bl_enter(void);

// Probe 7-bit addresses 0x08..0x77; prints each that ACKs. Returns true and sets
// *found to the first responder if any (expected 0x39).
bool i2c_bl_scan(uint8_t *found);

// AN4221 commands. Each returns false on NACK / no-ACK / bus error.
bool i2c_bl_get(uint8_t addr7);                                  // 0x00 Get (version + cmd list)
bool i2c_bl_get_id(uint8_t addr7);                               // 0x02 Get ID
bool i2c_bl_read(uint8_t addr7, uint32_t address, uint32_t len); // 0x11 Read Memory (hexdump)
bool i2c_bl_ru(uint8_t addr7);                                   // 0x93 Readout Unprotect (No-Stretch): mass-erase + remove RDP (destructive)
bool i2c_bl_probe(uint8_t addr7, uint8_t cmd);                   // send any command byte, report ACK/NACK (gate mapping)
bool i2c_bl_gv(uint8_t addr7);                                   // 0x01 Get Version & Read Protection Status
bool i2c_bl_go(uint8_t addr7, uint32_t addr);                    // 0x21 Go
bool i2c_bl_write(uint8_t addr7, uint32_t addr, const uint8_t *data, uint32_t len); // 0x31 Write Memory
bool i2c_bl_erase_mass(uint8_t addr7);                           // 0x45 Extended Erase (mass) — destructive
bool i2c_bl_rp(uint8_t addr7);                                   // 0x83 Readout Protect (re-lock RDP1) — destructive-ish

#endif // I2C_BOOTLOADER_H
