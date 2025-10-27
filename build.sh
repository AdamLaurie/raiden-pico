#!/bin/bash

# Raiden Pico C/C++ Build Script

set -e

echo "=== Raiden Pico C/C++ Build ==="

# Check if PICO_SDK_PATH is set
if [ -z "$PICO_SDK_PATH" ]; then
    echo "ERROR: PICO_SDK_PATH environment variable not set"
    echo "Please set it to your Pico SDK installation directory"
    echo "Example: export PICO_SDK_PATH=/path/to/pico-sdk"
    exit 1
fi

echo "Using Pico SDK at: $PICO_SDK_PATH"

# Create build directory
if [ ! -d "build" ]; then
    echo "Creating build directory..."
    mkdir build
fi

cd build

# Run CMake
echo "Running CMake configuration..."
cmake ..

# Build
echo "Building project..."
make -j$(nproc)

# Check if build succeeded
if [ -f "raiden_pico.uf2" ]; then
    echo ""
    echo "=== Build Successful ==="
    echo "Output files:"
    ls -lh raiden_pico.*
    echo ""
    echo "To flash:"
    echo "1. Hold BOOTSEL button on Pico while plugging in USB"
    echo "2. Copy raiden_pico.uf2 to the RPI-RP2 drive"
    echo "   or run: cp raiden_pico.uf2 /media/\$USER/RPI-RP2/"
    echo ""
    echo "To connect to CLI:"
    echo "  screen /dev/ttyACM0 115200"
    echo "  or: minicom -D /dev/ttyACM0 -b 115200"
else
    echo ""
    echo "=== Build Failed ==="
    exit 1
fi
