#!/bin/bash
# Flash firmware to Pico2

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_DIR="$( cd "$SCRIPT_DIR/.." && pwd )"

# Build dir can be passed as $1 (cmake passes ${CMAKE_CURRENT_BINARY_DIR}), defaults to build/
BUILD_DIR="${1:-$PROJECT_DIR/build}"
UF2_FILE="$BUILD_DIR/raiden_pico.uf2"

# udisks2 mounts removable media under /run/media/$USER on some systems and
# /media/$USER on others — check both rather than hardcoding one.
CANDIDATE_MOUNTS=("/run/media/${USER}/RP2350" "/media/${USER}/RP2350")

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
MOUNT_POINT=""
for i in {1..10}; do
    for candidate in "${CANDIDATE_MOUNTS[@]}"; do
        if [ -d "$candidate" ]; then
            MOUNT_POINT="$candidate"
            break
        fi
    done
    if [ -n "$MOUNT_POINT" ]; then
        echo "✓ Device ready ($MOUNT_POINT)"
        break
    fi
    sleep 1
    if [ $i -eq 10 ]; then
        echo "✗ RP2350 not mounted after 10 seconds"
        echo "  Check if device is in bootloader mode (hold BOOTSEL button)"
        exit 1
    fi
done

# Flash — the mount can appear before it is writable, so retry the copy and
# check cp's exit status (never report success on a failed copy).
echo "Flashing raiden_pico.uf2..."
CP_ERR=$(mktemp)
flashed=0
for attempt in 1 2 3 4 5; do
    if cp "$UF2_FILE" "$MOUNT_POINT/" 2>"$CP_ERR"; then
        sync 2>/dev/null || true   # device reboots on accept; sync is best-effort
        flashed=1
        break
    fi
    echo "  copy attempt $attempt failed, retrying..."
    sleep 1
done
if [ "$flashed" -ne 1 ]; then
    echo "✗ Flash FAILED — could not copy UF2 to $MOUNT_POINT"
    [ -s "$CP_ERR" ] && echo "  $(cat "$CP_ERR")"
    rm -f "$CP_ERR"
    exit 1
fi
rm -f "$CP_ERR"
echo "✓ Flash complete"
