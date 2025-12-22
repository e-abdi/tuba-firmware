cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tuba)

# Suppress warnings - must come after find_package(Zephyr)
zephyr_compile_options(-Wno-double-promotion -Wno-main)

target_sources(app PRIVATE
  src/app_print.c
  src/app_params.c
  src/main.c
  src/net_console.c
  src/ui_menu.c
  src/hw_motors.c
  src/hw_pump.c
  src/hw_ms5837.c
  src/deploy.c
  src/hw_bmp180.c
  src/hw_gps.c
  src/hw_hmc6343.c
)

target_include_directories(app PRIVATE include)

# Force-include prototypes for app_printk so all sources see it
target_compile_options(app PRIVATE -include app_print.h)
