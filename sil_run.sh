#!/bin/bash
# sil_run.sh — run the software-in-the-loop firmware on this terminal
#
# Start Gazebo and the tuba-sim bridge first (tuba-sim/scripts/run_sil.sh),
# then this. The glider's console is this terminal: it behaves like the
# serial console (raw keystrokes, firmware echo) so the menus work as on the
# real hardware. Ctrl-C quits.
#
# Parameters saved from the PARAMETERS menu persist in $TUBA_SIL_FLASH
# (default ~/.tuba_sil_flash.bin), like the ESP32's NVS. Delete the file
# for a factory-fresh glider.
cd "$(dirname "${BASH_SOURCE[0]}")"
EXE=build_sil/zephyr/zephyr.exe
if [ ! -x "$EXE" ]; then
    echo "No $EXE — run ./sil_build.sh first" >&2
    exit 1
fi
FLASH="${TUBA_SIL_FLASH:-$HOME/.tuba_sil_flash.bin}"
if [ -t 0 ]; then
    stty -icanon -echo
    trap 'stty sane' EXIT
fi
"$EXE" --flash="$FLASH" "$@"
