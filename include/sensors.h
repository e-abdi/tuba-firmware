/*
 * sensors.h — Expected-sensor registry and I2C scan report
 *
 * The glider knows which sensors it expects on the i2c0 bus. At boot (and
 * on demand from the hardware-test menu) each expected sensor is probed
 * individually and a human-readable report is printed:
 *
 *   I2C sensor check (i2c0):
 *     [ OK ]  MS5837   external pressure/temp   @ 0x76
 *     [MISS]  BMP180   internal pressure        @ 0x77
 *     [ OK ]  TSYS01   water temperature        @ 0x77
 *     [ OK ]  HMC6343  compass                  @ 0x19
 *     [MISS]  u-blox   GPS (DDC)                @ 0x42
 *   Unexpected devices: none
 *
 * This replaces the old blind 0x03..0x77 address sweep, whose output gave
 * no hint of *which* sensor was missing.
 *
 * Address-conflict note: the TSYS01 (Blue Robotics Celsius) and the BMP180
 * both live at 0x77. The scan tells them apart via the BMP180 chip-id
 * register, and warns when the conflict prevents one from being seen.
 */
#ifndef SENSORS_H
#define SENSORS_H

/* Probe every expected sensor and print the full report, including a bus
 * sweep for unexpected/unknown devices. Used at boot and from the menu. */
void sensors_report(void);

#endif /* SENSORS_H */
