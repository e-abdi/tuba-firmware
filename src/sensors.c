/*
 * sensors.c — Expected-sensor registry and I2C scan report
 *
 * Each expected sensor gets a dedicated, quiet probe that checks not just
 * "does the address ACK" but, where the part allows it, "is it really this
 * chip" (e.g. BMP180 chip-id, TSYS01/MS5837 calibration PROM sanity).
 *
 * The report is intentionally the single place that understands the bus
 * topology, including the 0x77 address conflict between the BMP180 and the
 * TSYS01 — the individual drivers stay simple.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include <stdbool.h>
#include <string.h>

#include "sensors.h"
#include "app_print.h"

/* All sensors share i2c0 (GPIO21 SDA / GPIO22 SCL, 100 kHz). */
static const struct device *const bus = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0));

/* ---- Well-known addresses -------------------------------------------- */
#define ADDR_HMC6343  0x19   /* tilt-compensated compass                  */
#define ADDR_UBLOX    0x42   /* u-blox GPS via DDC (I2C)                  */
#define ADDR_MS5837   0x76   /* external pressure/temperature (Bar30)    */
#define ADDR_BMP180   0x77   /* internal (hull) pressure                  */
#define ADDR_TSYS01   0x77   /* water temperature (Celsius) — same as BMP180! */

/* ---- Low-level helpers ------------------------------------------------ */

/* Generic "does anything ACK at this address" probe. */
static bool addr_acks(uint8_t addr)
{
    uint8_t byte = 0;
    return i2c_write_read(bus, addr, NULL, 0, &byte, 1) == 0;
}

/* Read one big-endian PROM word using the MEAS command scheme shared by
 * the MS5837 and TSYS01 (0xA0 + 2*idx). */
static int meas_prom_word(uint8_t addr, int idx, uint16_t *out)
{
    uint8_t cmd = 0xA0 + (uint8_t)(idx * 2);
    uint8_t buf[2];
    int rc = i2c_write_read(bus, addr, &cmd, 1, buf, sizeof(buf));
    if (rc == 0) {
        *out = ((uint16_t)buf[0] << 8) | buf[1];
    }
    return rc;
}

/* True when the MEAS-style PROM at this address holds plausible data
 * (at least one calibration word that is neither 0x0000 nor 0xFFFF). */
static bool meas_prom_sane(uint8_t addr)
{
    for (int i = 1; i <= 5; i++) {
        uint16_t w;
        if (meas_prom_word(addr, i, &w) != 0) {
            return false;
        }
        if (w != 0x0000 && w != 0xFFFF) {
            return true;
        }
    }
    return false;
}

/* BMP180 identity: chip-id register 0xD0 must read 0x55. */
static bool bmp180_at(uint8_t addr)
{
    uint8_t reg = 0xD0, id = 0;
    return i2c_write_read(bus, addr, &reg, 1, &id, 1) == 0 && id == 0x55;
}

/* ---- Per-sensor probes ------------------------------------------------ */
/* Each returns true when the sensor is confirmed present, and may append a
 * short detail string (e.g. the reason it could not be checked). */

static bool probe_hmc6343(const char **detail)
{
    *detail = "";
    return addr_acks(ADDR_HMC6343);
}

static bool probe_ublox(const char **detail)
{
    *detail = "";
    /* Read the "bytes available" register pair the driver uses (0xFD). */
    uint8_t reg = 0xFD, val = 0;
    return i2c_write_read(bus, ADDR_UBLOX, &reg, 1, &val, 1) == 0;
}

static bool probe_ms5837(const char **detail)
{
    *detail = "";
    if (!addr_acks(ADDR_MS5837)) {
        return false;
    }
    if (!meas_prom_sane(ADDR_MS5837)) {
        *detail = " (ACKs but PROM unreadable)";
        return false;
    }
    return true;
}

static bool probe_bmp180(const char **detail)
{
    *detail = "";
    if (!addr_acks(ADDR_BMP180)) {
        return false;
    }
    if (!bmp180_at(ADDR_BMP180)) {
        /* Something else answers at 0x77 — almost certainly the TSYS01. */
        *detail = " (0x77 occupied by another device)";
        return false;
    }
    return true;
}

static bool probe_tsys01(const char **detail)
{
    *detail = "";
    if (!addr_acks(ADDR_TSYS01)) {
        return false;
    }
    if (bmp180_at(ADDR_TSYS01)) {
        /* The BMP180 owns 0x77 on this bus: the TSYS01 cannot coexist. */
        *detail = " (0x77 taken by BMP180 — address conflict!)";
        return false;
    }
    if (!meas_prom_sane(ADDR_TSYS01)) {
        *detail = " (ACKs but calibration PROM unreadable)";
        return false;
    }
    return true;
}

/* ---- Registry ---------------------------------------------------------- */

struct sensor_entry {
    const char *name;                 /* chip name                     */
    const char *role;                 /* what it does on the glider    */
    uint8_t addr;                     /* expected I2C address          */
    bool (*probe)(const char **detail);
};

static const struct sensor_entry EXPECTED[] = {
    { "MS5837",  "external pressure/temp", ADDR_MS5837,  probe_ms5837  },
    { "BMP180",  "internal pressure",      ADDR_BMP180,  probe_bmp180  },
    { "TSYS01",  "water temperature",      ADDR_TSYS01,  probe_tsys01  },
    { "HMC6343", "compass",                ADDR_HMC6343, probe_hmc6343 },
    { "u-blox",  "GPS (DDC)",              ADDR_UBLOX,   probe_ublox   },
};

/* Is this address claimed by any expected sensor? (For the sweep below.) */
static bool addr_expected(uint8_t addr)
{
    for (size_t i = 0; i < ARRAY_SIZE(EXPECTED); i++) {
        if (EXPECTED[i].addr == addr) {
            return true;
        }
    }
    return false;
}

void sensors_report(void)
{
    if (bus == NULL || !device_is_ready(bus)) {
        app_printk("[SENSORS] i2c0 bus not ready — cannot scan\r\n");
        return;
    }

    /* Consistent bus speed with the drivers. */
    (void)i2c_configure(bus, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));

    app_printk("\r\nI2C sensor check (i2c0, GPIO21/22 @ 100 kHz):\r\n");

    int missing = 0;
    for (size_t i = 0; i < ARRAY_SIZE(EXPECTED); i++) {
        const struct sensor_entry *s = &EXPECTED[i];
        const char *detail = "";
        bool ok = s->probe(&detail);
        app_printk("  [%s]  %-8s %-24s @ 0x%02x%s\r\n",
                   ok ? " OK " : "MISS",
                   s->name, s->role, s->addr, detail);
        if (!ok) {
            missing++;
        }
        k_busy_wait(100);
    }

    /* Sweep the rest of the address space for surprises (mis-strapped
     * address pins, unknown boards left on the bus, ...). */
    int unexpected = 0;
    char list[80] = "";
    size_t used = 0;
    for (uint16_t addr = 0x03; addr <= 0x77; addr++) {
        if (addr_expected((uint8_t)addr)) {
            continue;
        }
        if (addr_acks((uint8_t)addr)) {
            unexpected++;
            int n = snprintk(list + used, sizeof(list) - used, " 0x%02x", addr);
            if (n > 0 && (size_t)n < sizeof(list) - used) {
                used += (size_t)n;
            }
        }
        k_busy_wait(50);
    }
    if (unexpected > 0) {
        app_printk("  [????]  %d unexpected device(s):%s\r\n", unexpected, list);
    }

    if (missing == 0 && unexpected == 0) {
        app_printk("  All expected sensors present.\r\n");
    } else if (missing > 0) {
        app_printk("  %d expected sensor(s) missing — check wiring/power.\r\n", missing);
    }
    app_printk("\r\n");
}
