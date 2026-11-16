# Running the firmware as a Linux process (software-in-the-loop)

The whole application — menus, parameters, missions, safety envelope, dive
log — can run on a Linux PC against the Gazebo glider from
[tuba-sim](https://github.com/e-abdi/tuba-sim). No source file in `src/` is
touched for this: Zephyr's `native_sim` board provides emulated GPIO, I2C
and ADC controllers, and `src/sil/` makes those behave like the glider's
chips, fed by Gazebo over UDP.

```
boards/native_sim_native_64.overlay   pins, buses and chips as the ESP32 overlay has them
prj_sil.conf                          build config (no WiFi/OTA/MCUboot; host sockets)
dts/bindings/sensor/tuba,sim-*.yaml   devicetree bindings for the emulated chips
src/sil/sil_link.c                    20 Hz: H-bridge pin states -> bridge, sensor values <- bridge,
                                      limit switches -> gpio1, leak -> adc0
src/sil/sil_emul_*.c                  MS5837, BMP180, HMC6343, TSYS01, u-blox register-level emulators
src/sil/sil_protocol.h                the two UDP packets (copy kept in tuba-sim/bridge/)
src/sil/ota_stub.c                    OTA menu entry says "not available"
```

## Build and run

```bash
./sil_build.sh      # west build -b native_sim/native/64 -d build_sil -- -DCONF_FILE=prj_sil.conf
./sil_run.sh        # console on this terminal; Ctrl-C quits
```

Order: Gazebo (`tuba-sim/scripts/run_sim.sh`, press Play) → bridge
(`tuba-sim/scripts/run_sil.sh`) → `./sil_run.sh`. Without the bridge every
sensor is "missing" and `deploy` is refused, as with unplugged hardware.

`sil_run.sh` puts the terminal in raw mode so the firmware's own echo and
single-key `q` work as on the serial console. Parameters saved with `s`
persist in `~/.tuba_sil_flash.bin` (`TUBA_SIL_FLASH` to change,
`--flash_in_ram` to start fresh).

Parameter values and actuator conventions (which end is "0", stroke times,
what floats and what dives) are documented in `tuba-sim/docs/sil.md`; the
short version is pump 6/4/6 s, pitch 11/4/0 s, strokes 12/12 s.

## How the link works

* Every 50 ms `sil_link.c` reads the emulated outputs of GPIO25/26 (roll),
  27/14 (pitch), 18/19 (pump), integrates signed run time per actuator and
  sends `sil_act_pkt` (UDP 5601). It also reports the firmware's own
  dead-reckoned positions, purely for the bridge's display.
* The bridge answers with `sil_sens_pkt` (UDP 5600): absolute water
  pressure at the pressure port, water/hull temperature and pressure,
  heading/pitch/roll in HMC6343 convention, lat/lon, GPS-fix flag (antenna
  above the surface), limit-switch flags, leak voltage, UTC.
* The emulators compute raw registers from that so the drivers' own
  compensation math (MS5837 CRC-4 PROM and second-order correction, BMP180
  datasheet algorithm, TSYS01 polynomial) returns the intended values.
* No packet for 2 s → every I2C transfer fails with `-EIO` (bus empty) and
  the limit switches read open. The drivers' retry/abort paths therefore
  get exercised by simply stopping the bridge.

The bridge learns the firmware's address from the packets it receives, so
the same protocol serves an ESP32 over the `Tuba-Glider` WiFi later
(hardware-in-the-loop); `sil_act_pkt.flags` is reserved for the per-device
real/simulated switches that build will need.
