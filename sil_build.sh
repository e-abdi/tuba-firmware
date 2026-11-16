#!/bin/bash
# sil_build.sh — build the firmware as a Linux executable (software-in-the-loop)
#
# Produces build_sil/zephyr/zephyr.exe. Needs the Zephyr workspace this repo
# lives in (west + SDK), and a host gcc; no ESP32 toolchain involved.
# See SIL.md for how to run it against Gazebo.
set -e
cd "$(dirname "${BASH_SOURCE[0]}")"
west build -b native_sim/native/64 -d build_sil . -- -DCONF_FILE=prj_sil.conf "$@"
echo
echo "Built: $(pwd)/build_sil/zephyr/zephyr.exe"
