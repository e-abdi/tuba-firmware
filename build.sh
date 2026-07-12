#!/bin/bash
# Build script for Tuba ESP32 firmware.
#
# - Increments the build counter (include/version.h)
# - Builds with --sysbuild so MCUboot + the signed app are produced together.
#   IMPORTANT: a plain non-sysbuild build makes an app image only; flashing
#   it onto an erased chip leaves no bootloader and nothing boots.
#
# Flash everything (bootloader @0x1000 + app @0x20000):  west flash
# OTA image:  build/tuba/zephyr/zephyr.signed.bin

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "Generating version header..."
bash gen_version.sh

echo ""
echo "Building firmware (sysbuild: MCUboot + app)..."
west build --sysbuild -b esp32_devkitc/esp32/procpu "$@"

echo ""
echo "Build complete!"
ls -lh build/mcuboot/zephyr/zephyr.bin build/tuba/zephyr/zephyr.signed.bin
