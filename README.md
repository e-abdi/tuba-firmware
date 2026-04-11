# Tuba ESP32 Glider Firmware

**License**: CERN-OHL-W v2 (Open Hardware)  
**Platform**: ESP32 DevKitC (WROOM-32U) running Zephyr RTOS v4.3.0  
**Bootloader**: MCUboot with dual-slot OTA support

The current implementation of the Tuba firmware loosely follows the operational model of a Seaglider. After power-up, the system waits for a defined period for user input before entering a recovery mode. In this mode, a simple console menu allows testing of individual hardware components, adjustment of mission parameters, and execution or simulation of dives.

During normal operation, the glider performs a dive cycle and then returns to the surface, where it again waits for user input before repeating the mission.

## Quick Start

### Build Firmware
```bash
west build --sysbuild --board=esp32_devkitc/esp32/procpu
```

The signed firmware will be at: `build/tuba/zephyr/zephyr.signed.bin`

### Flash Device
```bash
west flash
```

## Hardware Overview

**Console**: WiFi telnet (SSID: `Tuba-Glider`, IP: `192.168.4.1`, port 23)  
**Logging**: UART0 to OpenLog (9600 baud) — **remove before flashing**

### GPIO Pinout

| Device | Pin(s) | Notes |
|--------|--------|-------|
| **Motor: Roll** | GPIO25/26 | H-bridge inputs |
| **Motor: Pitch** | GPIO27/14 | H-bridge inputs |
| **Pump** | GPIO18/19 | Variable buoyancy |
| **Pitch Limit UP** | GPIO32 | Input-only, active-low |
| **Pitch Limit DOWN** | GPIO33 | Input-only, active-low |
| **I2C: SDA/SCL** | GPIO21/22 | Sensors: BMP180, MS5837, HMC6343 |
| **UART0: TX/RX** | GPIO1/3 | OpenLog console output |

## Over-The-Air (OTA) Updates

OTA allows wireless firmware updates without physical access. MCUboot handles safe atomic swaps between firmware slots.

### Three-Step Update Process

**1. Modify code** (e.g., change startup timeout):
```bash
# Edit src/app_params.c or prj.conf
west build --sysbuild --board=esp32_devkitc/esp32/procpu
```

**2. Start HTTP server** (from firmware output directory):
```bash
cd build/tuba/zephyr
python3 -m http.server 8000
```

**3. Trigger OTA from device**:
```bash
telnet 192.168.4.1 23
# Select menu option 5: OTA firmware update
# Enter: http://<YOUR_PC_IP>:8000/zephyr.signed.bin
```

Device will:
- Download firmware (~667 KB)
- Verify MCUboot signature
- Request upgrade
- Reboot and swap images
- Run new firmware





