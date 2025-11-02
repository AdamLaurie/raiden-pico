#!/bin/bash
# Flash firmware to Pico2

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_DIR="$( cd "$SCRIPT_DIR/.." && pwd )"
BUILD_DIR="$PROJECT_DIR/build"
UF2_FILE="$BUILD_DIR/raiden_pico.uf2"
MOUNT_POINT="/media/${USER}/RP2350"

# Check if UF2 exists
if [ ! -f "$UF2_FILE" ]; then
    echo "✗ Firmware not found: $UF2_FILE"
    echo "  Run 'make' in the build directory first"
    exit 1
fi

# Reboot to bootloader
echo "Rebooting Pico2 to bootloader mode..."
if [ -c "/dev/ttyACM0" ]; then
    python3 -c "import serial, time; s = serial.Serial('/dev/ttyACM0', 115200, timeout=1); time.sleep(0.5); s.write(b'REBOOT BL\r\n'); s.close()"
else
    echo "✗ /dev/ttyACM0 not available"
    echo "  Device may already be in bootloader mode or not connected"
fi

# Wait for mount
echo "Waiting for RP2350 bootloader mount..."
for i in {1..10}; do
    if [ -d "$MOUNT_POINT" ]; then
        echo "✓ Device ready"
        break
    fi
    sleep 1
    if [ $i -eq 10 ]; then
        echo "✗ RP2350 not mounted after 10 seconds"
        echo "  Check if device is in bootloader mode (hold BOOTSEL button)"
        exit 1
    fi
done

# Flash
echo "Flashing raiden_pico.uf2..."
cp "$UF2_FILE" "$MOUNT_POINT/"
sync
echo "✓ Flash complete"
