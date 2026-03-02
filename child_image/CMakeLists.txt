# MCUboot bootloader child image configuration
cmake_minimum_required(VERSION 3.20.0)
project(mcuboot LANGUAGES C ASM)

# Use Zephyr's MCUboot implementation
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# MCUboot configuration is handled by the bootloader kconfig
