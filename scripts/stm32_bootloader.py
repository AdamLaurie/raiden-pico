#!/usr/bin/env python3
"""
STM32 Bootloader communication via Raiden-Pico

Usage:
    python3 stm32_bootloader.py [port]
"""

import serial
import time
import sys

class STM32Bootloader:
    ACK = 0x79
    NACK = 0x1F
    
    def __init__(self, port='/dev/ttyACM0', baud=115200):
        self.ser = serial.Serial(port, baud, timeout=2)
        time.sleep(0.3)
        self.ser.read(self.ser.in_waiting)  # Clear buffer
        self.cmd("API ON")  # Enable API mode for cleaner responses
        
    def close(self):
        self.ser.close()
        
    def cmd(self, c, delay=0.3):
        """Send CLI command and return response"""
        self.ser.write((c + '\r\n').encode())
        time.sleep(delay)
        return self.ser.read(self.ser.in_waiting or 1000)
    
    def sync(self):
        """Sync with STM32 bootloader"""
        self.cmd("TARGET STM32")
        resp = self.cmd("TARGET SYNC", delay=2.0)
        return b"ACK received" in resp
    
    def send_cmd(self, cmd_byte, complement=None):
        """Send bootloader command and return response bytes"""
        if complement is None:
            complement = cmd_byte ^ 0xFF

        hex_str = f"{cmd_byte:02X}{complement:02X}"
        cmd_str = f'TARGET SEND {hex_str}'
        self.ser.write((cmd_str + '\r\n').encode())
        time.sleep(0.5)
        resp = self.ser.read(self.ser.in_waiting or 1000)

        # In API mode, response is:
        # [command echo]\r\n[raw bootloader bytes][API status: + or -][\r\n]
        #
        # Don't split on \r\n as binary data may contain those bytes!
        # Instead: skip known prefix (echo), strip known suffix (API status)

        # Skip the command echo at start
        cmd_bytes = cmd_str.encode()
        if resp.startswith(cmd_bytes):
            resp = resp[len(cmd_bytes):]
            # Skip \r\n after echo
            if resp.startswith(b'\r\n'):
                resp = resp[2:]
            elif resp.startswith(b'\r') or resp.startswith(b'\n'):
                resp = resp[1:]

        # Skip API raw data prefix '.' if present
        if resp.startswith(b'.'):
            resp = resp[1:]

        # Strip trailing prompt and API status from end
        # Format: [data]+> \r\n>  or similar
        while resp.endswith(b' ') or resp.endswith(b'>') or resp.endswith(b'\r') or resp.endswith(b'\n'):
            resp = resp[:-1]
        if resp.endswith(b'+') or resp.endswith(b'-'):
            resp = resp[:-1]

        return list(resp)
    
    def get_id(self):
        """Get chip ID"""
        resp = self.send_cmd(0x02)
        if resp and resp[0] == self.ACK and len(resp) >= 4:
            # ACK, N, PID[0], PID[1], ACK
            pid = (resp[2] << 8) | resp[3]
            return pid
        return None
    
    def get_version(self):
        """Get bootloader version and commands"""
        # Response format: ACK, N, Version, Cmd0..CmdN, ACK
        # N = number of bytes - 1 (version + N commands)
        resp = self.send_cmd(0x00)
        if resp and resp[0] == self.ACK and len(resp) >= 3:
            n = resp[1]  # Number of bytes - 1
            version = resp[2]  # Version is byte 2
            # Commands are bytes 3 to 3+n-1 (n-1 commands after version)
            cmds = resp[3:3+n] if len(resp) > 3 else []
            return version, cmds
        return None, []
    
    def get_version_rps(self):
        """Get version and read protection status"""
        resp = self.send_cmd(0x01)
        if resp and resp[0] == self.ACK:
            return resp[1:]
        return None

    def send_raw(self, hex_str, timeout=0.5):
        """Send raw hex bytes and return response"""
        cmd_str = f'TARGET SEND {hex_str}'
        self.ser.write((cmd_str + '\r\n').encode())
        time.sleep(timeout)
        resp = self.ser.read(self.ser.in_waiting or 4096)

        # Parse API mode response
        cmd_bytes = cmd_str.encode()
        if resp.startswith(cmd_bytes):
            resp = resp[len(cmd_bytes):]
            if resp.startswith(b'\r\n'):
                resp = resp[2:]
            elif resp.startswith(b'\r') or resp.startswith(b'\n'):
                resp = resp[1:]

        if resp.startswith(b'.'):
            resp = resp[1:]

        while resp.endswith(b' ') or resp.endswith(b'>') or resp.endswith(b'\r') or resp.endswith(b'\n'):
            resp = resp[:-1]
        if resp.endswith(b'+') or resp.endswith(b'-'):
            resp = resp[:-1]

        return list(resp)

    def read_memory(self, address, length):
        """Read memory from target (max 256 bytes per call)"""
        if length > 256:
            length = 256

        # Step 1: Send Read Memory command (0x11 + 0xEE)
        resp = self.send_cmd(0x11)
        if not resp or resp[0] != self.ACK:
            return None

        # Step 2: Send address (4 bytes) + XOR checksum
        addr_bytes = [
            (address >> 24) & 0xFF,
            (address >> 16) & 0xFF,
            (address >> 8) & 0xFF,
            address & 0xFF
        ]
        checksum = addr_bytes[0] ^ addr_bytes[1] ^ addr_bytes[2] ^ addr_bytes[3]
        hex_str = ''.join(f'{b:02X}' for b in addr_bytes) + f'{checksum:02X}'
        resp = self.send_raw(hex_str)
        if not resp or resp[0] != self.ACK:
            return None

        # Step 3: Send number of bytes - 1 + checksum (N ^ 0xFF = N ^ 0xFF)
        n = length - 1
        hex_str = f'{n:02X}{n ^ 0xFF:02X}'
        resp = self.send_raw(hex_str, timeout=1.0)
        if not resp or resp[0] != self.ACK:
            return None

        # Data follows the ACK
        return resp[1:1+length] if len(resp) > 1 else []

    def dump_flash(self, start_addr, size, filename=None):
        """Dump flash memory to file"""
        data = bytearray()
        addr = start_addr
        chunk_size = 256

        print(f"Dumping {size} bytes from 0x{start_addr:08X}...")

        while len(data) < size:
            remaining = size - len(data)
            to_read = min(chunk_size, remaining)

            chunk = self.read_memory(addr, to_read)
            if chunk is None:
                print(f"\nRead failed at 0x{addr:08X}")
                break

            data.extend(chunk)
            addr += len(chunk)

            # Progress
            pct = len(data) * 100 // size
            print(f"\r  {len(data)}/{size} bytes ({pct}%)", end='', flush=True)

        print()

        if filename:
            with open(filename, 'wb') as f:
                f.write(data)
            print(f"Saved to {filename}")

        return bytes(data)


def main():
    import argparse
    parser = argparse.ArgumentParser(description='STM32 Bootloader communication via Raiden-Pico')
    parser.add_argument('port', nargs='?', default='/dev/ttyACM0', help='Serial port')
    parser.add_argument('--dump', action='store_true', help='Dump entire flash')
    parser.add_argument('--size', type=lambda x: int(x, 0), default=0x100000, help='Flash size (default 1MB)')
    parser.add_argument('-o', '--output', default='flash_dump.bin', help='Output filename')
    args = parser.parse_args()

    print(f"Connecting to {args.port}...")
    bl = STM32Bootloader(args.port)

    print("Syncing with bootloader...")
    if not bl.sync():
        print("ERROR: Failed to sync with bootloader")
        bl.close()
        return 1

    print("Sync OK!\n")

    # Get ID
    pid = bl.get_id()
    flash_size = args.size
    if pid:
        print(f"Product ID: 0x{pid:04X}")
        if pid == 0x0415:
            print("  -> STM32F42xxx/STM32F43xxx")
        elif pid == 0x0419:
            print("  -> STM32F42xxx/STM32F43xxx (Rev 3)")
    else:
        print("Failed to get Product ID")

    # Get version and commands
    version, cmds = bl.get_version()
    if version:
        print(f"\nBootloader version: {version >> 4}.{version & 0xF}")
        cmd_names = {
            0x00: "Get", 0x01: "GetVer", 0x02: "GetID", 0x11: "Read",
            0x21: "Go", 0x31: "Write", 0x44: "Erase", 0x63: "WrProt",
            0x73: "WrUnprot", 0x82: "RdProt", 0x92: "RdUnprot"
        }
        print("Supported commands:")
        for c in cmds:
            name = cmd_names.get(c, f"0x{c:02X}")
            print(f"  0x{c:02X} - {name}")

    # Dump flash if requested
    if args.dump:
        print(f"\n--- Flash Dump ---")
        if 0x11 not in cmds:
            print("ERROR: Read command not supported (RDP active?)")
            bl.close()
            return 1

        start_addr = 0x08000000  # Flash start for STM32
        bl.dump_flash(start_addr, flash_size, args.output)

    bl.close()
    print("\nDone.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
