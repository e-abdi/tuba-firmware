# Tuba Dive Sequence Documentation

This document describes the complete dive deployment sequence for the Tuba glider. It is kept in sync with the actual implementation in `src/deploy.c` and `src/ui_menu.c`.

---

## Overview

The Tuba glider performs autonomous dive-climb cycles:
1. **Pre-dive setup** on the water surface
2. **Descend** to a target depth while monitoring sensors
3. **Climb** back to the surface
4. **Recovery** with GPS acquisition and user decision to restart or stop

The sequence is fully logged via telnet/serial console, with sensor readings printed every second during active diving.

---

## Detailed Sequence

### Phase 1: Initialization & Surface Calibration

#### 1.1 Read Surface Pressure Reference
```
[DEPLOY] surface external pressure: 101.325 kPa (T=20.45 C)
```
- **Component**: MS5837 external pressure sensor
- **Purpose**: Establish baseline pressure at surface
- **Used for**: All depth calculations (depth = (external_pressure - surface_pressure) / (water_density × gravity))
- **Failure mode**: If sensor unavailable, deploy fails with error message suggesting "simulate" mode

#### 1.2 Record Starting Positions
```
[DEPLOY] starting positions: pitch=0.0s, roll=0.0s, pump=0.0s
```
- **Components**: 
  - MOTOR_PITCH: steerable control surface for pitch adjustment
  - MOTOR_ROLL: steerable control surface for roll adjustment
  - PUMP: variable buoyancy pump (servo-controlled)
- **Purpose**: Know where to return to between dive cycles
- **Units**: Servo position in seconds (0-10s typical range)

#### 1.3 Surface Hold Wait
```
[DEPLOY] waiting 15s before first dive
```
- **Duration**: `deploy_wait_s` parameter (default: 15 seconds)
- **Purpose**: Allow time for glider stabilization and manual positioning in water
- **User action needed**: None - just wait

---

### Phase 2: Pre-Dive GPS Acquisition

```
[DEPLOY] acquiring GPS fix before dive
[GPS] acquiring RMC fix (timeout: 30s)
[GPS] acquired fix: lat=40.234567, lon=-74.123456
```
- **Component**: GPS receiver (UBLOX or compatible)
- **Timeout**: 30 seconds
- **Purpose**: Record launch position for drift tracking
- **Logic**: Blocks until valid RMC sentence with 'A' (active) status received
- **Prints**:
  - 'V' character while waiting for fix (every second)
  - Full lat/lon when acquired
  - Timeout message if no fix within 30s

---

### Phase 3: Move to Dive Configuration

#### 3.1 Move to Start Positions
```
[DEPLOY] moving to surface position: pitch target=0s (delta=0.0s), pump target=0s (delta=0.0s)
```
- **Components**: MOTOR_PITCH, PUMP
- **Target positions**: `start_pitch_s`, `start_pump_s` (from parameters)
- **Logic**: Calculate delta from current position, run motors if delta > 0.5s threshold
- **Direction**: +1 for positive delta, -1 for negative delta
- **Duration**: Each motor runs for `abs(delta)` seconds

#### 3.2 Move to Dive Targets
```
[DEPLOY] moving to dive targets: pitch=7s (delta=7.0s), pump=3s (delta=3.0s)
```
- **Components**: MOTOR_PITCH, PUMP
- **Target positions**: `dive_pitch_s`, `dive_pump_s` (from parameters)
- **Purpose**: Set glider attitude and buoyancy for controlled descent
- **Typical values**:
  - `dive_pitch_s`: 7 seconds (nose-down pitch angle)
  - `dive_pump_s`: 3 seconds (pump fluid out to reduce buoyancy)
- **Duration**: Calculated from current position

---

### Phase 4: Dive Monitoring

```
[DEPLOY] monitoring sensors while diving to 5.0m
[SENS] IntP=101984 Pa, ExtDepth=0.53m, H=343.7,R=-134.7,P=81.5
[SENS] IntP=101996 Pa, ExtDepth=1.06m, H=345.0,R=-133.6,P=81.3
[SENS] IntP=101984 Pa, ExtDepth=1.59m, H=343.3,R=-135.5,P=81.6
...
```

#### 4.1 Sensor Readings (Every 1 Second)
- **BMP180** (Internal Pressure): Aircraft altitude sensor, logged for reference
- **MS5837** (External Pressure): Seawater pressure → depth calculation
  - Formula: `depth_m = (external_pa - surface_pa) / (1025.0 kg/m³ × 9.80665 m/s²)`
- **HMC6343** (Compass): Heading, pitch, roll orientation
- **Output format**: `[SENS] IntP=XXX Pa, ExtDepth=X.XXm, H=heading,R=roll,P=pitch`

#### 4.2 Dive Termination Condition (One of Two)

**Option A: Target Depth Reached**
```
[DEPLOY] target depth reached (5.27m) -> start climb
```
- Triggers when: `depth_m >= dive_depth_m`
- Proceeds immediately to climb phase

**Option B: Dive Timeout**
```
[DEPLOY] dive timeout -> start climb
```
- Triggers when: Elapsed time > `dive_timeout_min` × 60 seconds
- Default: 5 minutes
- Safety feature to prevent getting stuck at depth

---

### Phase 5: Climb Sequence

#### 5.1 Move to Climb Configuration
```
[DEPLOY] moving to climb targets: pitch=0s (delta=-7.0s), pump=0s (delta=-3.0s)
```
- **Purpose**: Switch from descent to ascent attitude
- **Typical values**:
  - `climb_pitch_s`: 0 seconds (nose-up/neutral pitch)
  - `climb_pump_s`: 0 seconds (pump neutral to increase buoyancy)
- **Effect**: Glider becomes buoyant and returns to surface

#### 5.2 Monitor Climb to Surface
```
[DEPLOY] depth < 1m reached; moving to surface position
```
- **Trigger**: When `depth_m < 1.0` meter
- **Action**: Move MOTOR_PITCH and PUMP back to `start_pitch_s` and `start_pump_s`
- **Monitor**: Continue reading sensors for 5 more seconds after surface detected
- **Purpose**: Stabilize glider at surface before proceeding

---

### Phase 6: Post-Dive Recovery

#### 6.1 Acquire Post-Dive GPS Fix
```
[DEPLOY] acquired surface position, getting GPS fix
[GPS] acquiring RMC fix (timeout: 30s)
[GPS] acquired fix: lat=40.234890, lon=-74.123678
```
- **Purpose**: Record position after dive to measure distance traveled
- **Same logic as Phase 2**: 30-second timeout, blocks until fix acquired

#### 6.2 User Decision Point
```
[DEPLOY] press ENTER within 10 seconds to stop, or will start another dive...
```
- **Wait duration**: 10 seconds
- **Input source**: Net console (telnet) or serial
- **Polling**: `net_console_poll_line()` with 500ms timeout
- **Possible outcomes**:
  - **User presses ENTER**: `[DEPLOY] user requested stop` → Deployment ends
  - **10 seconds elapsed**: `[DEPLOY] no user input, starting another dive cycle` → Jump back to Phase 3

---

### Phase 7: Return to Menu

```
[DEPLOY] deployment complete, returning to menu
```
- **State transition**: ST_DEPLOYED → ST_MENU
- **Menu reprints**: `on_entry_MENU()` called by main.c
- **User can then**: Select parameters (1), simulate (3), or deploy again (4)

---

## Simulate Mode

Runs identical sequence to real deployment, but with **simulated pressure** instead of reading the real sensor.

### Key Differences

#### Simulated Depth Calculation
```
simulated_depth = 0.5 * elapsed_seconds  (meters)
```
- **Rate**: 50 cm/s descent
- **Example**: 5m depth target reached in ~10 seconds
- **Implementation**: Calculated from monotonic clock, not sensor reads

#### Initial Simulated Pressure
```
[SIMULATE] simulated surface pressure: 101.325 kPa
```
- Uses standard sea-level pressure (101325 Pa)
- Does not perform actual sensor calibration

#### Sequence Flow
1. Acquire simulated GPS fix (prints "acquired (simulated)")
2. Wait on surface for `deploy_wait_s` seconds
3. Run dive cycle with depth = 0.5 × elapsed_time
4. When depth >= target: switch to climb phase
5. Climb phase: depth decreases as time continues (no climb movement, just time)
6. When depth < 1m: move to surface position
7. Acquire simulated GPS fix
8. 10-second wait for ENTER (auto-restart or exit as per real deploy)

### Why Simulate?

- **Lab testing**: Full sequence validation without water
- **Parameter tuning**: Test different motor/pump timings without getting wet
- **Debugging**: Verify motor control without pressure sensor dependency
- **Training**: Understand glider behavior on land

---

## Sensor Output Format

Every second during diving, one line like this appears:

```
[SENS] IntP=101984 Pa, ExtDepth=2.64m, H=343.8,R=-134.4,P=81.6
```

### Field Meanings

| Field | Name | Source | Units | Notes |
|-------|------|--------|-------|-------|
| `IntP` | Internal Pressure | BMP180 | Pa | Not used for depth; reference only |
| `ExtDepth` | Calculated Depth | MS5837 | meters | Real seawater depth (or simulated) |
| `H` | Heading | HMC6343 | degrees | 0-360°, compass direction |
| `R` | Roll | HMC6343 | degrees | Degrees from horizontal |
| `P` | Pitch | HMC6343 | degrees | Nose-up/down angle |

---

## Configuration Parameters

All parameters are stored in non-volatile memory and can be edited via menu option 1.

### Core Dive Parameters

| Parameter | Variable | Default | Units | Purpose |
|-----------|----------|---------|-------|---------|
| Dive depth | `dive_depth_m` | 5 | meters | Target depth for descent phase |
| Wait before dive | `deploy_wait_s` | 15 | seconds | Surface hold time before first dive |
| Dive timeout | `dive_timeout_min` | 5 | minutes | Maximum dive duration (safety limit) |

### Motor/Pump Control

| Parameter | Variable | Default | Units | Purpose |
|-----------|----------|---------|-------|---------|
| Start pitch | `start_pitch_s` | 0 | seconds | Neutral pitch position |
| Dive pitch | `dive_pitch_s` | 7 | seconds | Nose-down pitch for descent |
| Climb pitch | `climb_pitch_s` | 0 | seconds | Neutral/nose-up pitch for ascent |
| Start pump | `start_pump_s` | 0 | seconds | Neutral buoyancy pump position |
| Dive pump | `dive_pump_s` | 3 | seconds | Pump out (reduce buoyancy) for descent |
| Climb pump | `climb_pump_s` | 0 | seconds | Pump neutral (increase buoyancy) for ascent |

### Navigation Parameters

| Parameter | Variable | Default | Units | Purpose |
|-----------|----------|---------|-------|---------|
| Start roll | `start_roll_s` | 0 | seconds | Neutral roll position (not currently controlled) |
| Max roll | `max_roll_s` | 1 | seconds | Maximum roll deflection (not currently controlled) |
| Roll time | `roll_time_s` | 5 | seconds | Time scale for roll maneuvers (not currently controlled) |
| Desired heading | `desired_heading_deg` | 180 | degrees | Target heading for navigation (not currently controlled) |

---

## Error Handling

### Sensor Failures During Dive

If a sensor fails mid-dive, the glider **continues diving** with logged errors:

```
[DEPLOY] Internal pressure read failed
[DEPLOY] External pressure read failed
[DEPLOY] Compass read failed
```

The glider relies on **timeout** to exit the dive in this case.

### Pre-Dive Sensor Check

When user selects deploy (option 4 in menu), sensor availability is checked:

```
[DEPLOY] ERROR: external pressure sensor not available
[DEPLOY] Try option 3 (simulate) instead
```

Deployment is **blocked** if the external pressure sensor (MS5837) cannot be read at startup.

### GPS Timeout

If GPS fix not acquired within 30 seconds:

```
[GPS] timeout waiting for RMC fix
```

The glider **continues anyway** (GPS not critical to diving). Useful for land-based testing.

---

## Telemetry & Logging

All messages are printed to:
- **Serial console** (if connected)
- **Telnet** (WiFi access point on 192.168.4.1)
- **Dual console** (both outputs simultaneously)

### Example Full Dive Session

```
=== MENU ===
1) parameters
2) hardware test
3) simulate
4) deploy
Select [1-4]: 4
[DEPLOY] starting sequence
[DEPLOY] surface external pressure: 101.325 kPa (T=20.45 C)
[DEPLOY] starting positions: pitch=0.0s, roll=0.0s, pump=0.0s
[DEPLOY] waiting 15s before first dive
[DEPLOY] acquiring GPS fix before dive
[GPS] acquired fix: lat=40.234567, lon=-74.123456
[DEPLOY] moving to surface position: pitch target=0s, pump target=0s
[DEPLOY] moving to dive targets: pitch=7s (delta=7.0s), pump=3s (delta=3.0s)
[DEPLOY] monitoring sensors while diving to 5.0m
[SENS] IntP=101984 Pa, ExtDepth=0.53m, H=343.7,R=-134.7,P=81.5
[SENS] IntP=101996 Pa, ExtDepth=1.06m, H=345.0,R=-133.6,P=81.3
[SENS] IntP=101984 Pa, ExtDepth=2.64m, H=343.8,R=-134.4,P=81.6
[SENS] IntP=101987 Pa, ExtDepth=5.27m, H=344.4,R=-133.6,P=81.6
[DEPLOY] target depth reached (5.27m) -> start climb
[DEPLOY] moving to climb targets: pitch=0s (delta=-7.0s), pump=0s (delta=-3.0s)
[SENS] IntP=101993 Pa, ExtDepth=4.84m, H=343.4,R=-134.4,P=81.6
[SENS] IntP=101992 Pa, ExtDepth=3.25m, H=341.8,R=-135.8,P=81.6
[SENS] IntP=101989 Pa, ExtDepth=0.88m, H=343.8,R=-134.4,P=81.5
[DEPLOY] depth < 1m reached; moving to surface position
[SENS] IntP=101992 Pa, ExtDepth=0.00m, H=343.8,R=-134.4,P=81.6
[DEPLOY] acquired surface position, getting GPS fix
[GPS] acquired fix: lat=40.234890, lon=-74.123678
[DEPLOY] press ENTER within 10 seconds to stop, or will start another dive...

[DEPLOY] user requested stop
[DEPLOY] deployment complete, returning to menu

=== MENU ===
1) parameters
2) hardware test
3) simulate
4) deploy
Select [1-4]:
```

---

## Code References

### Main Implementation Files
- **Core dive logic**: `src/deploy.c`
  - `deploy_start()`: Synchronous deployment sequence
  - `deploy_start_async()`: Spawns worker thread
  - `deploy_dive_cycle()`: Single dive/climb cycle
  - `deploy_check_sensor_available()`: Pre-flight sensor check

- **Menu system**: `src/ui_menu.c`
  - `on_entry_DEPLOYED()`: Spawn deploy worker
  - `on_entry_SIMULATE()`: Spawn simulate worker
  - State transitions and user input handling

- **Sensor drivers**: `src/hw_*.c`
  - `hw_ms5837.c`: External pressure sensor
  - `hw_bmp180.c`: Internal pressure sensor
  - `hw_hmc6343.c`: Compass/IMU
  - `hw_motors.c`: Pitch and roll servos
  - `hw_pump.c`: Variable buoyancy pump
  - `hw_gps.c`: GPS receiver

### Parameter Storage
- `src/app_params.c`: NVS storage of dive parameters
- `include/app_params.h`: Parameter structure definition

### Event System
- `include/app_events.h`: State machine and event definitions
- `src/main.c`: Main event loop and state transitions

---

## Future Enhancements

Currently not implemented but planned:
- Roll control during dive
- Compass-based heading hold
- Adaptive descent/ascent rates
- Mission waypoints and corridor tracking
- Data logging to SD card
- Real-time telemetry downlink

---

**Last Updated**: January 6, 2026  
**Document Version**: 1.0  
**Status**: Current with codebase
