/*
 * sensors.c — Expected-sensor registry and I2C scan report
 *
 * Each expected sensor gets a dedicated, quiet probe that checks not just
 * "does the address ACK" but, where the part allows it, "is it really this
 * chip" (e.g. BMP180 chip-id, TSYS01/MS5837 calibration PROM sanity).
 *
 * Bus topology (see the board overlay):
 *   i2c0 (GPIO21/22): MS5837, BMP180, HMC6343, u-blox GPS
 *   i2c_temp (GPIO4/13): TSYS01 water temperature — on its own bus because
 *                     its address (0x77) collides with the BMP180
 *
 * The report is intentionally the single place that understands this
 * topology, including the 0x77 conflict — the individual drivers stay
 * simple. The TSYS01 is also detected on i2c0 (with a conflict warning)
 * in case it gets wired to the main bus.
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

/* The two sensor buses. */
static const struct device *const bus0 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0));
static const struct device *const bus_temp = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c_temp));

/* ---- Well-known addresses -------------------------------------------- */
#define ADDR_HMC6343  0x19   /* tilt-compensated compass                  */
#define ADDR_UBLOX    0x42   /* u-blox GPS via DDC (I2C)                  */
#define ADDR_MS5837   0x76   /* external pressure/temperature (Bar30)    */
#define ADDR_BMP180   0x77   /* internal (hull) pressure                  */
#define ADDR_TSYS01   0x77   /* water temperature (Celsius), on i2c_temp      */

/* ---- Low-level helpers ------------------------------------------------ */

static bool bus_ok(const struct device *bus)
{
    return bus != NULL && device_is_ready(bus);
}

/* Generic "does anything ACK at this address" probe. */
static bool addr_acks(const struct device *bus, uint8_t addr)
{
    uint8_t byte = 0;
    return i2c_write_read(bus, addr, NULL, 0, &byte, 1) == 0;
}

/* Read one big-endian PROM word using the MEAS command scheme shared by
 * the MS5837 and TSYS01 (0xA0 + 2*idx). */
static int meas_prom_word(const struct device *bus, uint8_t addr, int idx, uint16_t *out)
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
static bool meas_prom_sane(const struct device *bus, uint8_t addr)
{
    for (int i = 1; i <= 5; i++) {
        uint16_t w;
        if (meas_prom_word(bus, addr, i, &w) != 0) {
            return false;
        }
        if (w != 0x0000 && w != 0xFFFF) {
            return true;
        }
    }
    return false;
}

/* BMP180 identity: chip-id register 0xD0 must read 0x55. */
static bool bmp180_at(const struct device *bus, uint8_t addr)
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
    return bus_ok(bus0) && addr_acks(bus0, ADDR_HMC6343);
}

static bool probe_ublox(const char **detail)
{
    *detail = "";
    if (!bus_ok(bus0)) {
        return false;
    }
    /* Read the "bytes available" register pair the driver uses (0xFD). */
    uint8_t reg = 0xFD, val = 0;
    return i2c_write_read(bus0, ADDR_UBLOX, &reg, 1, &val, 1) == 0;
}

static bool probe_ms5837(const char **detail)
{
    *detail = "";
    if (!bus_ok(bus0) || !addr_acks(bus0, ADDR_MS5837)) {
        return false;
    }
    if (!meas_prom_sane(bus0, ADDR_MS5837)) {
        *detail = " (ACKs but PROM unreadable)";
        return false;
    }
    return true;
}

static bool probe_bmp180(const char **detail)
{
    *detail = "";
    if (!bus_ok(bus0) || !addr_acks(bus0, ADDR_BMP180)) {
        return false;
    }
    if (!bmp180_at(bus0, ADDR_BMP180)) {
        /* Something else answers at 0x77 on i2c0 — likely a TSYS01 that
         * was wired to the main bus instead of i2c_temp. */
        *detail = " (0x77 on i2c0 occupied by another device)";
        return false;
    }
    return true;
}

/* TSYS01: expected on its own bus (i2c_temp), also detected on i2c0. */
static bool probe_tsys01(const char **detail)
{
    /* Preferred: dedicated bus i2c_temp (GPIO4 SDA / GPIO13 SCL). */
    if (bus_ok(bus_temp)) {
        for (uint8_t addr = ADDR_TSYS01; addr >= 0x76; addr--) {
            if (addr_acks(bus_temp, addr) && meas_prom_sane(bus_temp, addr)) {
                *detail = " (on i2c_temp)";
                return true;
            }
        }
    }

    /* Fallback: main bus, where 0x77 may be the BMP180. */
    if (bus_ok(bus0) && addr_acks(bus0, ADDR_TSYS01)) {
        if (bmp180_at(bus0, ADDR_TSYS01)) {
            *detail = " (not on i2c_temp; i2c0 0x77 is the BMP180)";
            return false;
        }
        if (meas_prom_sane(bus0, ADDR_TSYS01)) {
            *detail = " (on i2c0 — consider moving to i2c_temp, GPIO4/13)";
            return true;
        }
    }

    *detail = " (wire to i2c_temp: SDA=GPIO4, SCL=GPIO13)";
    return false;
}

/* ---- Registry ---------------------------------------------------------- */

struct sensor_entry {
    const char *name;                 /* chip name                     */
    const char *role;                 /* what it does on the glider    */
    const char *bus;                  /* expected bus label            */
    uint8_t addr;                     /* expected I2C address          */
    bool (*probe)(const char **detail);
};

static const struct sensor_entry EXPECTED[] = {
    { "MS5837",  "external pressure/temp", "i2c0", ADDR_MS5837,  probe_ms5837  },
    { "BMP180",  "internal pressure",      "i2c0", ADDR_BMP180,  probe_bmp180  },
    { "TSYS01",  "water temperature",      "i2c_temp", ADDR_TSYS01,  probe_tsys01  },
    { "HMC6343", "compass",                "i2c0", ADDR_HMC6343, probe_hmc6343 },
    { "u-blox",  "GPS (DDC)",              "i2c0", ADDR_UBLOX,   probe_ublox   },
};

/* Is this address claimed by an expected sensor on the given bus label?
 * (Used to filter the "unexpected devices" sweep below.) */
static bool addr_expected(const char *bus_label, uint8_t addr)
{
    for (size_t i = 0; i < ARRAY_SIZE(EXPECTED); i++) {
        if (EXPECTED[i].addr == addr && strcmp(EXPECTED[i].bus, bus_label) == 0) {
            return true;
        }
    }
    /* The TSYS01 may legitimately sit at 0x76/0x77 on either bus. */
    if (addr == 0x76 || addr == 0x77) {
        return true;
    }
    return false;
}

/* Sweep one bus for devices at addresses we don't expect there. */
static void sweep_unexpected(const struct device *bus, const char *label)
{
    if (!bus_ok(bus)) {
        return;
    }
    int unexpected = 0;
    char list[80] = "";
    size_t used = 0;
    for (uint16_t addr = 0x03; addr <= 0x77; addr++) {
        if (addr_expected(label, (uint8_t)addr)) {
            continue;
        }
        if (addr_acks(bus, (uint8_t)addr)) {
            unexpected++;
            int n = snprintk(list + used, sizeof(list) - used, " 0x%02x", addr);
            if (n > 0 && (size_t)n < sizeof(list) - used) {
                used += (size_t)n;
            }
        }
        k_busy_wait(50);
    }
    if (unexpected > 0) {
        app_printk("  [????]  %d unexpected device(s) on %s:%s\r\n",
                   unexpected, label, list);
    }
}

void sensors_report(void)
{
    if (!bus_ok(bus0) && !bus_ok(bus_temp)) {
        app_printk("[SENSORS] no I2C bus ready — cannot scan\r\n");
        return;
    }

    /* Consistent bus speed with the drivers. */
    if (bus_ok(bus0)) {
        (void)i2c_configure(bus0, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
    }
    if (bus_ok(bus_temp)) {
        (void)i2c_configure(bus_temp, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
    }

    app_printk("\r\nI2C sensor check (i2c0 GPIO21/22, i2c_temp GPIO4/13, 100 kHz):\r\n");

    int missing = 0;
    for (size_t i = 0; i < ARRAY_SIZE(EXPECTED); i++) {
        const struct sensor_entry *s = &EXPECTED[i];
        const char *detail = "";
        bool ok = s->probe(&detail);
        app_printk("  [%s]  %-8s %-24s %s @ 0x%02x%s\r\n",
                   ok ? " OK " : "MISS",
                   s->name, s->role, s->bus, s->addr, detail);
        if (!ok) {
            missing++;
        }
        k_busy_wait(100);
    }

    /* Sweep both buses for surprises (mis-strapped address pins, unknown
     * boards left on a bus, ...). */
    sweep_unexpected(bus0, "i2c0");
    sweep_unexpected(bus_temp, "i2c_temp");

    if (missing == 0) {
        app_printk("  All expected sensors present.\r\n");
    } else {
        app_printk("  %d expected sensor(s) missing — check wiring/power.\r\n", missing);
    }
    app_printk("\r\n");
}
