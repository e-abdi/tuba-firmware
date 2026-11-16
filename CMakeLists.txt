# Tuba underwater glider firmware — Zephyr application build
#
# Build (with MCUboot, from the repo root):
#   west build --sysbuild --board=esp32_devkitc/esp32/procpu
# or use ./build.sh, which also bumps the build number in include/version.h.

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tuba)

# Float args are promoted to double in app_printk(...) calls everywhere;
# the warning adds noise without value here. Must come after find_package.
zephyr_compile_options(-Wno-double-promotion)

target_sources(app PRIVATE
  # Core application
  src/main.c              # boot, WiFi AP + telnet server, state machine loop
  src/ui_menu.c           # menu system: per-state entry/event/line handlers
  src/deploy.c            # mission execution (deploy + simulate) + safety envelope
  src/dive_log.c          # Seaglider-style structured dive records ($STATE/$GC/$D)
  # Infrastructure
  src/app_print.c         # console output mirroring (UART + WiFi clients)
  src/app_params.c        # persistent mission parameters (NVS)
  src/app_time.c          # UTC wall clock (build-date default, GPS-synced)
  src/net_console.c       # WiFi console backend + input-mode switch
  src/sensors.c           # expected-sensor registry + I2C scan report
  # Hardware drivers
  src/hw_motors.c         # pitch/roll mass motors (H-bridge GPIO)
  src/hw_pump.c           # variable-buoyancy pump
  src/hw_limit_switches.c # pitch travel limit switches
  src/hw_ms5837.c         # external (water) pressure + temperature
  src/hw_bmp180.c         # internal (hull) pressure
  src/hw_tsys01.c         # water temperature (Blue Robotics Celsius)
  src/hw_hmc6343.c        # tilt-compensated compass
  src/hw_gps.c            # u-blox GPS over I2C (DDC)
  src/hw_leak.c           # leak detector (ADC)
)

target_include_directories(app PRIVATE include)

if(CONFIG_BOARD_NATIVE_SIM)
  # Software-in-the-loop build (prj_sil.conf, boards/native_sim.overlay):
  # the hardware drivers above run unchanged against Zephyr's emulated
  # GPIO/I2C/ADC, and src/sil/ connects those emulators to the Gazebo
  # bridge over UDP. See SIL.md.
  target_sources(app PRIVATE
    src/sil/sil_state.c        # latest simulated sensor values
    src/sil/sil_link.c         # UDP link: H-bridge pins out, sensor values in
    src/sil/sil_emul_ms5837.c  # emulated I2C sensors ...
    src/sil/sil_emul_bmp180.c
    src/sil/sil_emul_hmc6343.c
    src/sil/sil_emul_tsys01.c
    src/sil/sil_emul_ublox.c
    src/sil/ota_stub.c         # OTA menu entry: not available in the simulator
  )
  target_include_directories(app PRIVATE src/sil)
  # The host glibc hides strnlen() & co. behind feature macros under -std=c11.
  target_compile_definitions(app PRIVATE _POSIX_C_SOURCE=200809L)
else()
  target_sources(app PRIVATE
    src/ota_simple.c        # OTA firmware download into MCUboot slot1
  )
endif()

# Force-include prototypes for app_printk so all sources see it
target_compile_options(app PRIVATE -include app_print.h)
