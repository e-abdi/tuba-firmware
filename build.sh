#!/bin/bash
# Build script for Tuba ESP32 firmware
# Automatically increments build counter

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Generating version header..."
bash gen_version.sh

echo ""
echo "Building firmware..."
west build -b esp32_devkitc/esp32/procpu "$@"

echo ""
echo "Build complete!"
ls -lh build/zephyr/zephyr.bin
