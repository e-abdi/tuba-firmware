# Tuba Hardware Pinout & Device Connections

Complete GPIO and I2C/UART device pinout for the Tuba glider system.
Target board: **ESP32 DevKitC (esp32_devkitc/esp32/procpu)**

---

## Overview

The Tuba glider uses:
- **2 DC motors + 1 pump** controlled via GPIO H-bridges (Roll, Pitch, Pump)
- **I2C sensors** for pressure and compass
- **UART interfaces** for GPS and console communication

---

## Pinout

#### GPIO Motor Control Pins (H-Bridge Inputs)
Roll (Motor A)
- IN1: `GPIO25`
- IN2: `GPIO26`

Pitch (Motor B)
- IN1: `GPIO27`
- IN2: `GPIO14`

Pump (Variable Buoyancy)
- IN1: `GPIO18` (OUT)
- IN2: `GPIO19` (IN)

Controller: VMA409 (H-bridge) for motors, ZK-BM1 or similar for pump.

#### Limit Switches (Pitch Motor)
Mechanical endstop switches for safe pitch motor operation:
- **Pitch Limit UP**: `GPIO32` (input-only pin) — triggers at maximum UP position
- **Pitch Limit DOWN**: `GPIO33` (input-only pin) — triggers at maximum DOWN position
- **Active Level**: Active low (pulled to GND when pressed)
- **Function**: When triggered during pitch motor motion, motor stops immediately
- **Wiring**: 4.7kΩ pull-up to 3.3V; switch contact to GND (normally open)
- **Note**: GPIO32/33 are input-only pins (safe from boot strapping conflicts). Status read via direct ESP32 HAL register access

#### I2C0
- SDA: `GPIO21`
- SCL: `GPIO22`
- Speed: 100 kHz
- Sensors: MS5837 (`0x76/0x77`), BMP180 (`0x77`), HMC6343 (`0x19`)

#### UARTs
- UART0 Console: TX `GPIO1`, RX `GPIO3`, 9600 baud
- UART1 GPS: TX `GPIO10`, RX `GPIO9`, 9600 baud
- UART mirroring: Planned (mirror UART0 → UART1), not enabled on ESP32 yet

---

---

## Sensor & Bus Notes

- MS5837: auto-detected at `0x76/0x77` during boot.
- BMP180: manually initialized.
- HMC6343: initialized with UF (Upright Forward) orientation; supports interactive calibration.

I2C lines are open-drain; use 4.7kΩ pull-ups to 3.3V.

---

## UART Notes

- ESP32: UART0 via USB bridge (CH340 or similar) at 9600.
- ESP32: UART2 used for mirroring/GPS at 9600 (TX `GPIO17`, RX `GPIO16`). Avoid UART1 on `GPIO9/10` (SPI flash pins).

---

## WiFi Interface (ESP32)

- Onboard WiFi: ESP32 built-in module
- AP Mode: 192.168.4.1 (default), SSID configured in code
- Telnet: port 23

---

## Motor Control Logic

### Pitch Limit Switches

When the pitch motor moves, GPIO32 and GPIO33 are monitored continuously:

- **GPIO32 (Pitch Limit UP)**: Active low; triggers when pitch reaches full UP (nose up)
- **GPIO33 (Pitch Limit DOWN)**: Active low; triggers when pitch reaches full DOWN (nose down)
- **Action**: Motor stops immediately when switch pressed (real-time monitoring in main loop)
- **Logging**: `[LIMIT] Pitch limit switch triggered (GPIO32 or 33), stopping pitch motor`
- **Implementation**: Direct GPIO register polling + motor stop in main event loop (`limit_switches_check_and_stop()`)

**Wiring each switch**:
```
ESP32 GPIO32 ──┬─── GND (via switch contact, normally open)
               └─ 4.7kΩ pull-up resistor to 3.3V

ESP32 GPIO33 ──┬─── GND (via switch contact, normally open)
               └─ 4.7kΩ pull-up resistor to 3.3V
```

Use micro switches, lever switches, or hall-effect sensors configured as active-low inputs.

**How It Works**:
1. Main event loop calls `limit_switches_check_and_stop()` after each motor command
2. Function reads GPIO32/33 status directly from ESP32 HAL registers
3. If either switch is active (low), immediately calls `motor_run(MOTOR_PITCH, 0, 0)` to stop
4. No ISR complexity - simple, reliable polling in main thread context

### Control Method: PWM via GPIO Relay/H-Bridge

**Principle**: 
- IN1 and IN2 control motor direction via VMA409 H-bridge
- Motor runs continuously for specified duration (in seconds)
- Duration calculated from current position → target position delta

**Motor direction encoding**:
```
Direction = +1:   IN1=HIGH,  IN2=LOW   (forward rotation)
Direction = -1:   IN1=LOW,   IN2=HIGH  (reverse rotation)
Direction =  0:   IN1=LOW,   IN2=LOW   (motor off)
```

**Motor run sequence** (example: pitch motor):
1. Calculate delta: `target_pitch - current_pitch`
2. Determine direction: `+1` if delta > 0, `-1` if delta < 0
3. Calculate duration: `abs(delta)` seconds
4. Call `motor_run(MOTOR_PITCH, direction, duration_sec)`
5. GPIO controlled via VMA409 module triggers motor

**Timing**: Motor holds position until next command (no feedback, open-loop)

---

## Servo Position Encoding

**All three servos use identical position encoding**:

| Position (seconds) | Meaning | Usage |
|-------------------|---------|-------|
| 0 | Neutral/Center | Default starting position |
| 1-10 | Forward/Extended | Maximum deflection |
| Negative allowed | Reverse range | Some configurations use negative |

**Example - Pitch servo**:
- `0s` = Level pitch
- `7s` = Maximum nose-down (dive angle)
- `0s` = Back to level

**Command function**: `motor_run(motor_id, direction, duration_seconds)`

---

## Power Distribution

### ESP32 DevKit Power
- **Input**: USB 5V or external 5V
- **Internal 3.3V regulator**: Supplies ESP32 core logic

### Sensor Power (I2C)
- **Supply**: 3.3V (from ESP32 regulator)
- **Pull-ups**: 4.7kΩ to 3.3V on SDA/SCL

### Motor Power
- **Supply**: Typically 5V (independent from ESP32 logic)
- **Source**: External battery or USB 5V
- **Controller**: VMA409 H-bridge (handles 5V logic input from GPIO)

**Note**: Motor power should be isolated from ESP32 power supply to avoid noise/brownout issues

---

## Block Diagram (ESP32 example)

```
┌─────────────────────────────────────────────────────────────┐
│                    ESP32 DevKitC                             │
│ ┌──────────────┐  ┌──────────┐  ┌───────────────────────┐  │
│ │ GPIO Control │  │ I2C Bus  │  │ UART Interfaces       │  │
│ │              │  │ (0x76)   │  │                       │  │
│ ├──────────────┤  ├──────────┤  ├───────────────────────┤  │
│ │ GPIO25 ──→──┤  │ SDA(21)──┤  │ TX0(1)  ──→ USB       │  │
│ │ GPIO26 ──→──┤  │ SCL(22)──┤  │ RX0(3)  ←── USB       │  │
│ │ GPIO27 ──→──┤  └──────────┘  │                       │  │
│ │ GPIO14 ──→──┤                │ TX1(10) ──→ GPS       │  │
│ │ GPIO18 ──→──┤                │ RX1(9)  ←── GPS       │  │
│ │ GPIO19 ──→──┤                │                       │  │
│ └──────────────┘                │ TX2(17) ──→ [Future] │  │
│                                 │ RX2(16) ←── [Future] │  │
│                                 └───────────────────────┘  │
└──────────────────────┬───────────────────────────────────────┘
                       │
          ┌────────────┼────────────┐
          │            │            │
         ▼            ▼            ▼
    ┌────────┐    ┌────────┐   ┌────────┐
    │ VMA409 │    │ VMA409 │   │ZK-BM1/ │
    │H-Bridge│    │H-Bridge│   │ PWM    │
    │        │    │        │   │        │
    └────┬───┘    └────┬───┘   └────┬───┘
         │             │            │
         ▼             ▼            ▼
    ┌────────┐    ┌────────┐   ┌────────┐
    │  Roll  │    │ Pitch  │   │ Pump   │
    │ Motor  │    │ Motor  │   │ Motor  │
    └────────┘    └────────┘   └────────┘

         ▲             ▲            ▲
         │             │            │
         └─────────────┴────────────┘
              Servo Control
         (pulse duration timing)
         
         
    ┌────────────────────────────────┐
    │    I2C Sensor Cluster (0x76)   │
     ┌────────────────────────────────┐
     │   GPS Receiver (UART1)         │
     │   9600 baud, NMEA              │
     │   RX on GPIO9, TX on GPIO10    │
     └────────────────────────────────┘
         ▲
         │ I2C (SDA21/SCL22)
         │
    ┌────┴──────────────────────────┐
    │   ESP32 I2C0 Bus              │
    │   100 kHz, 3.3V               │
    └───────────────────────────────┘

    ┌────────────────────────────────┐
    │   GPS Receiver (UART1)         │
    │   9600 baud, NMEA              │
    │   RX on GPIO9, TX on GPIO10    │
    └────────────────────────────────┘
```

---

## Connection Checklists

### ESP32 DevKitC
- [x] Roll IN1/IN2 → `GPIO25/26` (VMA409)
- [x] Pitch IN1/IN2 → `GPIO27/14` (VMA409)
- [x] Pitch Limit UP/DOWN → `GPIO32/33` (endstop switches)
- [x] Pump IN1/IN2 → `GPIO18/19` (ZK-BM1/PWM)
- [x] I2C SDA/SCL → `GPIO21/22` (sensor cluster)
- [x] UART1 TX/RX → `GPIO10/9` (GPS)
- [x] USB Console → `GPIO1/3`
- [x] WiFi → Onboard ESP32

### Raspberry Pi Pico
- [x] Roll IN1/IN2 → `GP20/21`
- [x] Pitch IN1/IN2 → `GP18/19`
- [x] Pump IN1/IN2 → `GP26/27`
- [x] I2C0 SDA/SCL → `GP4/5`
- [x] UART1 TX/RX → `GP8/9` (console mirroring)
- [x] Console → USB CDC ACM

---

## Configuration Files

### Device Tree (ESP32)
- Location: `boards/esp32_devkitc_procpu.overlay`
- Defines GPIO aliases and UART/I2C pin mappings

### Devicetree Aliases
- Motor/Pump aliases (DT): `roll-in-1`, `roll-in-2`, `pitch-in-1`, `pitch-in-2`, `pump-in-1`, `pump-in-2`
- C access uses underscores: `DT_ALIAS(roll_in_1)` maps to DT alias `roll-in-1` (hyphen → underscore)
- Ensure alias targets reference nodes with a `gpios` property (e.g., children of a `gpio-leds` node)

### Code References
- Motor control: `src/hw_motors.c`
- Pump control: `src/hw_pump.c`
- **Limit switches**: `src/hw_limit_switches.c`, `include/hw_limit_switches.h`
- Console mirroring: `src/console_mirror.c`
- GPIO initialization: `src/hw_motors.c`, `src/hw_pump.c`, `src/hw_limit_switches.c`

---

## Future Expansion

**Available GPIO pins** (ESP32 DevKitC):
- GPIO 0, 2, 4, 5, 12, 13, 15, 23, 32, 33, 34, 35, 36, 37, 38, 39

**Available UART**: UART2 (GPIO 16/17) at 115200 baud

**I2C bus**: Additional sensors can be added to I2C0 at unique addresses

---

**Last Updated**: January 18, 2026  
**Board Target**: ESP32 DevKitC  
**Status**: Current with codebase
