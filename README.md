# Tuba ESP32 Glider Firmware

**License**: CERN-OHL-W v2 (Open Hardware)  
**Platform**: ESP32 DevKitC (WROOM-32U) running Zephyr RTOS v4.3.0  
**Bootloader**: MCUboot with dual-slot OTA support

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

### What's Working

✅ MCUboot bootloader integration (separate image)  
✅ Dual-slot OTA with atomic swaps  
✅ HTTP download with robust header parsing (split packets)  
✅ Direct flash_area writes (no buffering issues)  
✅ MCUboot header validation  
✅ Verified firmware signature check  
✅ Parameter persistence through OTA  

### OTA Architecture

```
Download Phase:
  Device (WiFi AP) ← HTTP GET ← Server
  Device writes to slot1 (0x170000) via flash_area_write()

Verification Phase:
  Device reads MCUboot header from slot1
  boot_read_bank_header() validates signature

Apply Phase:
  boot_request_upgrade(1) sets permanent swap flag
  Device reboots → MCUboot swaps images
  slot1 (new) → slot0 (running), old slot0 archived
```


