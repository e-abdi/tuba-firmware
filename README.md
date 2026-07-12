# Tuba — ESP32 Underwater Glider Firmware

**License**: CERN-OHL-W v2 (Open Hardware)
**Platform**: ESP32 DevKitC (WROOM-32U) running Zephyr RTOS v4.3
**Bootloader**: MCUboot with dual-slot OTA support

Tuba loosely follows the operational model of a Seaglider: it changes
buoyancy with a pump and shifts internal masses to pitch (dive/climb) and
roll (steer). After power-up the firmware waits for operator input; if none
arrives it deploys automatically and starts flying dive/climb cycles,
surfacing between cycles for a GPS fix and an operator stop window.

## Quick start

```bash
# Build (increments the build number, then west build):
./build.sh
# or directly:
west build --sysbuild --board=esp32_devkitc/esp32/procpu

# Flash over USB:
west flash
```

The signed OTA image is produced at `build/tuba/zephyr/zephyr.signed.bin`
(sysbuild) — see [OTA updates](#over-the-air-ota-updates).

## Consoles

Two consoles are live at the same time; output is mirrored to both:

| Console | How to connect |
|---------|----------------|
| **USB/serial** | `screen /dev/ttyUSB0 9600` (UART0, 9600 baud) |
| **WiFi** | Join open AP **`Tuba-Glider`**, then `telnet 192.168.4.1 23` |

Which console(s) accept *input* is a runtime setting: PARAMETERS menu →
option `g` (0 = both, 1 = USB/serial only, 2 = WiFi only). It takes effect
immediately and is saved with the rest of the parameters (`s`).

## Operation

On power-up the glider prints a countdown (default 600 s, `STARTUP_TIMEOUT_SEC`
in `include/app_limits.h`):

- **Press ENTER** → main menu.
- **Do nothing** → automatic transition to DEPLOYED (mission starts).

### Menu tree

```
MENU
├── 1 parameters        edit / save / reset mission parameters
├── 2 hardware test
│   ├── 1 pitch and roll    jog motors ±10 s; limit-switch test
│   ├── 2 pump              jog pump ±10 s
│   ├── 3 show positions    dead-reckoned actuator positions
│   ├── 4 internal pressure BMP180 stream (hull pressure)
│   ├── 5 external pressure MS5837 stream (depth)
│   ├── 6 GPS               wait for fix, print position
│   ├── 7 compass           HMC6343 calibrate / stream
│   ├── 8 leak sensor       ADC stream vs threshold
│   └── 9 sensors
│       ├── 1 temperature self-test   TSYS01 probe/PROM/sample checks
│       ├── 2 temperature stream      live water temperature
│       └── 3 I2C sensor scan report  expected vs connected
├── 3 simulate          mission with synthetic depth (bench testing)
├── 4 deploy            real mission (requires external pressure sensor)
└── 5 OTA firmware update
```

The full state machine is documented in `tuba_state_machine.puml`
(render with PlantUML).

### Missions (deploy / simulate)

Deploy and simulate run the **same** cycle code on a background worker
thread; simulate just replaces measured depth with a synthetic profile
(0.5 m/s sink/rise), so the entire sequence can be tested dry.

Each cycle: surface trim → dive trim → sink to `Dive depth` (or `Dive
timeout`) → climb trim → rise to the surface → surface trim → GPS fix →
10-second window where ENTER stops the mission, otherwise the next cycle
starts.

Once per second the mission prints a telemetry line:

```
[SENS] 2026-07-12 14:03:22Z IntP=99321 Pa, Depth=3.20m, WaterT=17.85C, H=181.0,R=0.5,P=-32.0
```

Heading is held by banking the roll mass toward `Desired heading`
(checked every 10 s, ±5° tolerance; bank direction flips between dive and
climb phases).

### System time (UTC)

There is no battery-backed RTC. The wall clock starts from the firmware
build date and is re-anchored to true UTC from every valid GPS fix (RMC
time + date). All timestamps — including `[SENS]` lines — are UTC; the
main menu header shows the current time and its source (`build` or `GPS`).

## Hardware

### GPIO pinout

| Device | Pin(s) | Notes |
|--------|--------|-------|
| **Motor: roll** | GPIO25/26 | H-bridge inputs |
| **Motor: pitch** | GPIO27/14 | H-bridge inputs |
| **Pump** | GPIO18/19 | variable buoyancy |
| **Pitch limit UP** | GPIO32 | input-only, active-low (gpio1 pin 0) |
| **Pitch limit DOWN** | GPIO33 | input-only, active-low (gpio1 pin 1) |
| **Leak sensor** | GPIO34 | ADC1 CH6; below threshold = leak |
| **I2C SDA/SCL** | GPIO21/22 | sensor bus, 100 kHz |
| **UART0 TX/RX** | GPIO1/3 | console, 9600 baud |

### I2C sensors (all on i2c0)

| Sensor | Address | Role |
|--------|---------|------|
| MS5837 (Bar30) | 0x76 | external water pressure + temp → depth |
| BMP180 | 0x77 | internal hull pressure |
| TSYS01 (Celsius) | 0x77 | water temperature |
| HMC6343 | 0x19 | tilt-compensated compass |
| u-blox GPS (DDC) | 0x42 | position + UTC time |

> **⚠ Address conflict**: the TSYS01 and BMP180 both default to **0x77**
> and cannot share the bus. The boot sensor scan detects which one actually
> answers (BMP180 is identified by its chip-id register) and reports the
> conflict explicitly.

At boot (and from *hardware test → sensors → scan*), every expected sensor
is probed individually:

```
I2C sensor check (i2c0, GPIO21/22 @ 100 kHz):
  [ OK ]  MS5837   external pressure/temp   @ 0x76
  [MISS]  BMP180   internal pressure        @ 0x77 (0x77 occupied by another device)
  [ OK ]  TSYS01   water temperature        @ 0x77
  [ OK ]  HMC6343  compass                  @ 0x19
  [ OK ]  u-blox   GPS (DDC)                @ 0x42
```

## Mission parameters

Edited in the PARAMETERS menu, stored in NVS (flash). Edits are RAM-only
until saved with `s`.

| # | Parameter | Meaning |
|---|-----------|---------|
| 1 | Dive depth [m] | target depth per cycle |
| 2 | Wait before dive [s] | surface hold before the first dive |
| 3 | Dive timeout [min] | abort the sink phase after this |
| 4–6 | Dive/Start/Climb pump [s] | pump positions per phase |
| 7–a | Start/Surface/Dive/Climb pitch [s] | pitch-mass positions per phase |
| b–d | Start/Max roll, Roll time [s] | roll-mass neutral/bank positions |
| e | Desired heading [deg] | course to hold (0–359 true) |
| f | Leak threshold [mV] | ADC below this = leak alarm |
| g | Console input mode | 0 = UART+WiFi, 1 = UART, 2 = WiFi |

Positions are dead-reckoned "seconds of motor travel from zero" — there
are no encoders; the pitch limit switches are the only absolute reference.

## Safety systems

- **Leak monitor**: background thread samples the leak ADC at 1 Hz,
  alarms on first detection and every 30 s while wet.
- **Pitch limit switches**: checked on every main-loop cycle; on
  overtravel the pitch motor is stopped and backed off for 1 s.

## Over-The-Air (OTA) updates

MCUboot swaps between two firmware slots; OTA downloads into slot 1 and
reboots into it.

```bash
# 1. Build the new firmware
west build --sysbuild --board=esp32_devkitc/esp32/procpu

# 2. Serve the signed image (from your PC, connected to the glider's AP)
cd build/tuba/zephyr && python3 -m http.server 8000

# 3. On the glider console: menu option 5, then enter:
#    http://<YOUR_PC_IP>:8000/zephyr.signed.bin
```

The device downloads (~670 KB), verifies the MCUboot signature, requests
the upgrade and reboots. Use `zephyr.signed.bin`, never `zephyr.bin`.

## Code map

```
src/
  main.c              boot, WiFi AP + telnet server, state-machine loop
  ui_menu.c           menus: per-state entry/event/line handlers
  deploy.c            mission execution (deploy + simulate, shared cycle)
  app_print.c         console output fan-out (UART + WiFi clients)
  app_params.c        persistent mission parameters (NVS)
  app_time.c          UTC wall clock (build-date default, GPS-synced)
  net_console.c       WiFi console backend + input-mode switch
  sensors.c           expected-sensor registry + I2C scan report
  ota_simple.c        OTA download into MCUboot slot1
  hw_motors.c         pitch/roll mass motors        hw_pump.c   buoyancy pump
  hw_limit_switches.c pitch travel limits           hw_leak.c   leak ADC
  hw_ms5837.c         external pressure/temp        hw_bmp180.c hull pressure
  hw_tsys01.c         water temperature (Celsius)   hw_hmc6343.c compass
  hw_gps.c            u-blox GPS over I2C (DDC)
```

Adding a UI state: one enum value in `include/app_events.h`, handlers in
`src/ui_menu.c`, one row in `STATE_TABLE` in `src/main.c`.
