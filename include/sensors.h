/*
 * sensors.h — Expected-sensor registry and I2C scan report
 *
 * The glider knows which sensors it expects on which I2C bus. At boot (and
 * on demand from the hardware-test menu) each expected sensor is probed
 * individually and a human-readable report is printed:
 *
 *   I2C sensor check (i2c0 GPIO21/22, i2c1 GPIO16/17, 100 kHz):
 *     [ OK ]  MS5837   external pressure/temp   i2c0 @ 0x76
 *     [ OK ]  BMP180   internal pressure        i2c0 @ 0x77
 *     [ OK ]  TSYS01   water temperature        i2c1 @ 0x77 (on i2c1)
 *     [ OK ]  HMC6343  compass                  i2c0 @ 0x19
 *     [MISS]  u-blox   GPS (DDC)                i2c0 @ 0x42
 *
 * This replaces the old blind 0x03..0x77 address sweep, whose output gave
 * no hint of *which* sensor was missing.
 *
 * Address-conflict note: the TSYS01 (Blue Robotics Celsius) and the BMP180
 * both answer at 0x77, which is why the TSYS01 lives on its own bus
 * (i2c1). The scan still tells them apart via the BMP180 chip-id register
 * if both end up sharing i2c0, and says so explicitly.
 */
#ifndef SENSORS_H
#define SENSORS_H

/* Probe every expected sensor and print the full report, including a bus
 * sweep for unexpected/unknown devices. Used at boot and from the menu. */
void sensors_report(void);

#endif /* SENSORS_H */
