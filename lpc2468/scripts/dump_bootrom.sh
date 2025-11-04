#!/bin/bash
# Dump LPC2468 boot ROM via JTAG using OpenOCD
# The boot ROM is located at 0x7FFEE000-0x7FFFFFFF (72KB)

set -e

OUTPUT_DIR="${1:-../bootrom}"
INTERFACE="${2:-/usr/share/openocd/scripts/interface/jlink.cfg}"
TARGET="${3:-/usr/share/openocd/scripts/target/lpc2478.cfg}"

echo "LPC2468 Boot ROM Dumper"
echo "======================="
echo "Output directory: $OUTPUT_DIR"
echo "Interface config: $INTERFACE"
echo "Target config:    $TARGET"
echo

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Dump boot ROM
echo "Dumping boot ROM (72KB @ 0x7FFEE000)..."
openocd \
    -f "$INTERFACE" \
    -f "$TARGET" \
    -c "init" \
    -c "arm7_9 fast_memory_access enable" \
    -c "halt" \
    -c "dump_image $OUTPUT_DIR/lpc2468_bootrom_full.bin 0x7ffee000 0x12000" \
    -c "exit"

echo
echo "Boot ROM dumped successfully!"
echo "File: $OUTPUT_DIR/lpc2468_bootrom_full.bin"
echo "Size: $(du -h $OUTPUT_DIR/lpc2468_bootrom_full.bin | cut -f1)"
echo

# Generate disassembly
if command -v arm-none-eabi-objdump &> /dev/null; then
    echo "Generating ARM mode disassembly..."
    arm-none-eabi-objdump -D -b binary -m arm7tdmi \
        "$OUTPUT_DIR/lpc2468_bootrom_full.bin" \
        --adjust-vma=0x7ffee000 \
        > "$OUTPUT_DIR/bootrom_arm_disasm.txt"

    echo "Generating Thumb mode disassembly..."
    arm-none-eabi-objdump -D -b binary -m arm7tdmi -Mforce-thumb \
        "$OUTPUT_DIR/lpc2468_bootrom_full.bin" \
        --adjust-vma=0x7ffee000 \
        > "$OUTPUT_DIR/bootrom_thumb_disasm.txt"

    echo "Disassembly files created:"
    echo "  - $OUTPUT_DIR/bootrom_arm_disasm.txt"
    echo "  - $OUTPUT_DIR/bootrom_thumb_disasm.txt"
else
    echo "Warning: arm-none-eabi-objdump not found, skipping disassembly"
fi

echo
echo "Done!"
