/*
 * hw_ms5837.h — MS5837 external (water) pressure + temperature sensor
 *
 * Primary depth sensor. I2C address 0x76 (0x77 probed as fallback).
 */
#ifndef HW_MS5837_H
#define HW_MS5837_H
#include <zephyr/kernel.h>

/* Menu streaming view: 1 Hz T/P/depth until the user quits with 'q'. */
void ms5837_stream_interactive(void);

/* Single sample: temperature [°C] and absolute pressure [kPa].
 * Probes and loads calibration on first use. Returns 0 on success. */
int ms5837_read(double *temp_c, double *press_kpa);

#endif /* HW_MS5837_H */
