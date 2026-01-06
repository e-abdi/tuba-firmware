Tuba ESP32 Firmware (Zephyr)

Overview
- Platform: ESP32 DevKitC (WROOM-32U) running Zephyr RTOS
- Networking: WiFi Access Point with telnet console
- Console: USB UART0 (primary), WiFi telnet (net console), UART1 mirror (OpenLog)
- Sensors: BMP180 (internal pressure), MS5837 (external pressure + depth), HMC6343 (compass), u-blox GPS
- I2C: SDA=GPIO21, SCL=GPIO22 at 100 kHz

Build and Flash
```bash
west build -b esp32_devkitc_wroom --pristine=auto
west flash
```

WiFi AP Console
- SSID: `Tuba-Glider`
- IP: `192.168.4.1`
- Telnet: `telnet 192.168.4.1 23`
- Behavior: Full console mirror over WiFi; input lines are accepted when you press ENTER.

UART Mirror (OpenLog)
- Port: `uart1`
- TX: `GPIO10` (connect to OpenLog RX)
- RX: `GPIO9` (not required for logging)
- Baud: `9600`

Pins Summary
- UART0 (USB console): TX=`GPIO1`, RX=`GPIO3`
- UART1 (GPS/Log mirror): TX=`GPIO10`, RX=`GPIO9`
- I2C0 (sensors): SDA=`GPIO21`, SCL=`GPIO22`

External Pressure + Depth
- Sensor: MS5837 (address 0x76)
- PROM validated via CRC-4; OSR=8192 reads with proper delays.
- Hardware Test menu “External Pressure”:
  - Calibrates baseline sea-level pressure over ~10 seconds.
  - Displays temperature, pressure (kPa), and computed depth (m).
  - Recalibrate anytime: press `b` then ENTER.
  - Depth formula: `depth = (P − P0) / (ρ · g)` with `ρ=1000 kg/m^3`, `g=9.80665 m/s^2`.

Common Tasks
- Connect WiFi console:
  ```bash
  telnet 192.168.4.1 23
  ```
- View GPS output: connected to `uart1` (9600 baud); mirrored logs appear on WiFi and UART1.
- I2C scan at boot prints detected addresses.

Notes
- Previous documentation referenced Raspberry Pi Pico/Pico W. The project now targets ESP32. For legacy notes, see historical docs in this repo.
- AP mode is quiet in logs; the TCP echo server accepts input lines and mirrors console output to connected clients.
