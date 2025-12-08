#!/bin/bash
# Dump LPC2468 flash memory via JTAG using J-Link
# Usage: ./dump_lpc2468_flash.sh [output_file]
# Exit codes: 0=success, 1=connection failed, 2=dump failed

OUTPUT_FILE="${1:-lpc2468_flash.bin}"
FLASH_SIZE="0x80000"  # 512KB
EXPECTED_SIZE=524288

# Remove old file if exists
rm -f "$OUTPUT_FILE"

# Create temp file for JLink output
JLINK_LOG=$(mktemp)
trap "rm -f $JLINK_LOG" EXIT

echo "=== LPC2468 Flash Dump via JTAG ===" >&2
echo "Output: $OUTPUT_FILE" >&2

# Run JLinkExe
JLinkExe -Device LPC2468 -If JTAG -Speed auto -JTAGConf -1,-1 -AutoConnect 1 << EOF > "$JLINK_LOG" 2>&1
h
savebin $OUTPUT_FILE 0x0 $FLASH_SIZE
q
EOF

# Check J-Link USB connection first
if grep -q "Cannot connect to J-Link" "$JLINK_LOG" || \
   grep -q "Connecting to J-Link via USB...FAILED" "$JLINK_LOG"; then
    echo "ERROR: Failed to connect to J-Link via USB" >&2
    exit 1
fi

# Check for no target voltage (target powered off)
if grep -q "VTref=0.000V" "$JLINK_LOG"; then
    echo "ERROR: No target voltage (VTref=0V) - target not powered" >&2
    exit 1
fi

# Check if JTAG chain was detected
if ! grep -q "JTAG chain detection found" "$JLINK_LOG"; then
    echo "ERROR: No JTAG device detected in chain" >&2
    exit 1
fi

# Check for target connection errors
if grep -q "Cannot connect to target" "$JLINK_LOG" || \
   grep -q "Could not connect" "$JLINK_LOG" || \
   grep -q "No connection to target" "$JLINK_LOG"; then
    echo "ERROR: Failed to connect to target via JTAG" >&2
    exit 1
fi

# Check if ARM core was identified
if ! grep -q "ARM7 identified" "$JLINK_LOG"; then
    echo "ERROR: ARM7 core not identified" >&2
    exit 1
fi

# Check if savebin completed successfully
if ! grep -q "Reading.*bytes from addr.*into file...O.K." "$JLINK_LOG"; then
    echo "ERROR: Flash read did not complete" >&2
    exit 2
fi

# Check if file was created and has correct size
if [ ! -f "$OUTPUT_FILE" ]; then
    echo "ERROR: Output file not created" >&2
    exit 2
fi

SIZE=$(stat -c %s "$OUTPUT_FILE")
if [ "$SIZE" -ne "$EXPECTED_SIZE" ]; then
    echo "ERROR: Dump size mismatch (got $SIZE, expected $EXPECTED_SIZE)" >&2
    exit 2
fi

# Success
echo "OK: Flash dump successful ($SIZE bytes)" >&2
exit 0
