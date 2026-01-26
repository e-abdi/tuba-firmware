Tuba ESP32 Firmware (Zephyr)

**License**: CERN-OHL-W v2 (Open Hardware)

## Overview
- Platform: ESP32 DevKitC (WROOM-32U) running Zephyr RTOS
- Networking: WiFi Access Point with telnet console
- Console: WiFi telnet (primary interactive console), UART0 output to OpenLog
- Sensors: BMP180 (internal pressure), MS5837 (external pressure), HMC6343 (compass), u-blox (GPS)
- I2C: SDA=GPIO21, SCL=GPIO22 at 100 kHz
- Limit Switches: GPIO32/33 for pitch motor endstops (automatic stop on trigger)

## Build and Flash
```bash
west build -b esp32_devkitc_wroom --pristine=auto
west flash
```

WiFi AP Console
- SSID: `Tuba-Glider`
- IP: `192.168.4.1`
- Telnet: `telnet 192.168.4.1 23`
- Behavior: Console output mirrored over WiFi; input lines are accepted when you press ENTER.

OpenLog Data Logging
- Port: `uart0` (USB, output-only)
- Baud: `115200`
- Console output is logged to OpenLog via UART0
- **Important**: Remove OpenLog from the board when flashing code. Reconnect after flashing is complete.

Pins Summary
- UART0 (OpenLog output): TX=`GPIO1`, RX=`GPIO3` (RX not used)
- I2C0 (sensors): SDA=`GPIO21`, SCL=`GPIO22`
- Pitch Limit UP: `GPIO32` (endstop switch)
- Pitch Limit DOWN: `GPIO33` (endstop switch)

## Limit Switches (Pitch Motor)
Pitch motor automatically stops when either endstop switch is triggered:
- GPIO32: Pitch UP limit (maximum nose-up)
- GPIO33: Pitch DOWN limit (maximum nose-down)
- Active-low inputs with 4.7kΩ pull-ups to 3.3V
- Real-time monitoring via main event loop
- Test via menu: Hardware Test → Pitch and Roll → Limit Switches

External Pressure + Depth
- Sensor: MS5837 (address 0x76)
- PROM validated via CRC-4; OSR=8192 reads with proper delays.
- Hardware Test menu “External Pressure”:
  - Calibrates baseline sea-level pressure over ~10 seconds.
  - Displays temperature, pressure (kPa), and computed depth (m).
  - Recalibrate anytime: press `b` then ENTER.
  - Depth formula: `depth = (P − P0) / (ρ · g)` with `ρ=1000 kg/m^3`, `g=9.80665 m/s^2`.

Common Tasks
- Connect WiFi console (main interactive console):
  ```bash
  telnet 192.168.4.1 23
  ```
- Flash new firmware:
  1. Remove OpenLog from USB port
  2. Run `west flash`
  3. Reconnect OpenLog after flashing
- I2C scan at boot prints detected addresses.

## License

This project is licensed under the **CERN Open Hardware Licence v2 - Weakly Reciprocal (CERN-OHL-W)**.

See `LICENSE` file for full license text.

### What this means:
- ✅ You can study, modify, and build from the hardware design
- ✅ You can manufacture and distribute the hardware
- ✅ Modifications must be shared under the same license
- ✅ You must document any changes
- ✅ Source code and hardware documentation must remain freely available

For more information about CERN-OHL, visit: https://cern.ch/cern-ohl

Notes
- Previous documentation referenced Raspberry Pi Pico/Pico W. The project now targets ESP32. For legacy notes, see historical docs in this repo.
- AP mode is quiet in logs; console output is mirrored to WiFi clients via TCP echo server.
