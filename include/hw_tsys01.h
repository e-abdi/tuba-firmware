/*
 * hw_tsys01.h — TSYS01 water-temperature sensor (Blue Robotics "Celsius")
 *
 * Fast-response ±0.1 °C digital thermometer on the sensor I2C bus (i2c0).
 * Default I2C address is 0x77; 0x76 is probed as a fallback (CSB strap).
 *
 * NOTE: 0x77 collides with the BMP180 internal-pressure sensor. Both cannot
 * share one bus at the same address — the boot sensor scan (sensors.c)
 * detects and reports this condition.
 */
#ifndef HW_TSYS01_H
#define HW_TSYS01_H

#include <stdbool.h>

/* Probe the sensor and load its calibration PROM.
 * Safe to call repeatedly; after the first success it is a no-op.
 * Returns 0 on success, -ENODEV if not found, -EIO on bus/PROM error. */
int tsys01_init(void);

/* True once tsys01_init() has succeeded (sensor present and calibrated). */
bool tsys01_is_present(void);

/* Single temperature sample in °C. Initializes on first use if needed.
 * Returns 0 on success or a negative errno. */
int tsys01_read(double *temp_c);

/* Menu self-test: probe, dump calibration, take samples, report PASS/FAIL.
 * Returns 0 on PASS, negative errno on FAIL. */
int tsys01_selftest(void);

/* Menu streaming view: 1 Hz readings until the user quits with 'q'. */
void tsys01_stream_interactive(void);

#endif /* HW_TSYS01_H */
