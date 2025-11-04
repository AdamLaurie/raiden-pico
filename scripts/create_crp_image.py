#!/usr/bin/env python3
"""
Create a valid LPC2468 bootable image with CRP protection

Generates a minimal bootable image with:
- Valid ARM exception vectors
- Correct boot checksum at 0x14
- CRP value at 0x1FC
"""

import struct
import sys

# CRP protection values (NXP LPC2xxx standard values)
CRP_VALUES = {
    "CRP0": 0xDEADBEEF,    # No protection
    "CRP1": 0x12345678,    # JTAG/SWD disabled, limited ISP
    "CRP2": 0x87654321,    # JTAG/SWD disabled, only full chip erase
    "CRP3": 0x43218765,    # Maximum protection
    "NO_ISP": 0x4E697370,  # ISP disabled, JTAG/SWD enabled
}

def calculate_checksum(vectors):
    """
    Calculate LPC2xxx boot checksum

    The checksum is the two's complement of the sum of the first 7 vectors.
    It's placed at offset 0x14 (5th vector location).
    """
    # Sum first 7 vectors (excluding the 5th one at offset 0x14)
    vector_sum = sum(vectors[i] for i in [0, 1, 2, 3, 5, 6, 7])

    # Two's complement (32-bit)
    checksum = (~vector_sum + 1) & 0xFFFFFFFF

    return checksum

def create_crp_image(crp_level, output_file):
    """Create a bootable LPC2468 image with CRP protection"""

    if crp_level not in CRP_VALUES:
        print(f"✗ Invalid CRP level: {crp_level}")
        print(f"Valid levels: {', '.join(CRP_VALUES.keys())}")
        return False

    crp_value = CRP_VALUES[crp_level]

    print(f"Creating LPC2468 bootable image with {crp_level} protection")
    print(f"CRP value: 0x{crp_value:08x}")
    print(f"Output file: {output_file}")
    print()

    # Create 512-byte image (first flash sector)
    image = bytearray(512)

    # Fill with 0xFF (erased flash state)
    for i in range(len(image)):
        image[i] = 0xFF

    # ARM exception vectors (minimal valid vectors)
    # These point to a simple infinite loop at 0x40
    vectors = [
        0x00000040,  # 0x00: Reset vector -> infinite loop at 0x40
        0x00000040,  # 0x04: Undefined instruction
        0x00000040,  # 0x08: Software interrupt (SWI)
        0x00000040,  # 0x0C: Prefetch abort
        0x00000040,  # 0x10: Data abort
        0x00000000,  # 0x14: Reserved (checksum will go here)
        0x00000040,  # 0x18: IRQ
        0x00000040,  # 0x1C: FIQ
    ]

    # Calculate and insert checksum
    checksum = calculate_checksum(vectors)
    vectors[5] = checksum

    # Write vectors to image (little-endian)
    for i, vector in enumerate(vectors):
        offset = i * 4
        image[offset:offset+4] = struct.pack('<I', vector)

    # Add simple infinite loop code at 0x40
    # ARM instruction: B 0x40 (branch to self)
    # Encoding: 0xEAFFFFFE
    image[0x40:0x44] = struct.pack('<I', 0xEAFFFFFE)

    # Write CRP value at 0x1FC (little-endian)
    image[0x1FC:0x200] = struct.pack('<I', crp_value)

    # Write binary image to file
    try:
        with open(output_file, 'wb') as f:
            f.write(image)
        print(f"✓ Binary image created: {output_file} ({len(image)} bytes)")
    except Exception as e:
        print(f"✗ Failed to write binary image: {e}")
        return False

    # Also create Intel HEX file
    hex_file = output_file.rsplit('.', 1)[0] + '.hex'
    try:
        with open(hex_file, 'w') as f:
            # Write data records (16 bytes per line)
            addr = 0
            for i in range(0, len(image), 16):
                chunk = image[i:i+16]
                byte_count = len(chunk)

                # Build record
                record = [byte_count, (addr >> 8) & 0xFF, addr & 0xFF, 0x00]  # Type 00 = data
                record.extend(chunk)

                # Calculate checksum (two's complement of sum)
                checksum = (~sum(record) + 1) & 0xFF
                record.append(checksum)

                # Write record as hex
                hex_line = ':' + ''.join(f'{b:02X}' for b in record) + '\n'
                f.write(hex_line)

                addr += byte_count

            # Write EOF record
            f.write(':00000001FF\n')

        print(f"✓ Intel HEX created: {hex_file}")
    except Exception as e:
        print(f"✗ Failed to write HEX file: {e}")
        return False

    # Verify image
    print("\nImage contents:")
    print(f"  Exception vectors: 0x00-0x1F")
    print(f"  Boot checksum:     0x{checksum:08x} at offset 0x14")
    print(f"  CRP value:         0x{crp_value:08x} at offset 0x1FC")
    print(f"  Infinite loop:     0xEAFFFFFE at offset 0x40")

    return True

def main():
    if len(sys.argv) < 3:
        print("Usage: ./create_crp_image.py <crp_level> <output_file>")
        print()
        print("CRP Levels:")
        for name, value in CRP_VALUES.items():
            print(f"  {name:8s} = 0x{value:08x}")
        print()
        print("Example:")
        print("  ./create_crp_image.py CRP1 crp1_image.bin")
        return 1

    crp_level = sys.argv[1].upper()
    output_file = sys.argv[2]

    if create_crp_image(crp_level, output_file):
        print("\n✓ SUCCESS!")
        print("\nTo flash this image:")
        print("  OpenOCD: flash write_image <file> 0x0")
        print("  J-Flash: Load and program the image")
        print("  ISP:     Use Flash Magic or lpc21isp")
        return 0
    else:
        return 1

if __name__ == "__main__":
    sys.exit(main())
