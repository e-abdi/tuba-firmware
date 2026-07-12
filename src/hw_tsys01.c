/*
 * hw_tsys01.c — TSYS01 water-temperature sensor driver (Blue Robotics Celsius)
 *
 * Protocol (see MEAS TSYS01 datasheet / Blue Robotics Arduino library):
 *   0x1E        soft reset
 *   0xA0..0xAE  read 8 calibration PROM words (k coefficients in words 1..5)
 *   0x48        start ADC temperature conversion (max ~10 ms)
 *   0x00        read 24-bit ADC result
 *
 * Temperature is computed from the 16 MSBs of the ADC value with the
 * 4th-order polynomial from the datasheet:
 *
 *   adc  = D1 / 256
 *   T °C = -2*k4*1e-21*adc^4 + 4*k3*1e-16*adc^3
 *          -2*k2*1e-11*adc^2 + 1*k1*1e-6*adc - 1.5*k0*1e-2
 *
 * Address handling: Blue Robotics ships the Celsius at 0x77, which is the
 * same address as the BMP180. Before claiming 0x77 we read the BMP180
 * chip-id register (0xD0): a 0x55 answer means a BMP180 lives there and the
 * TSYS01 is reported as absent (bus conflict — see sensors.c boot report).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <errno.h>
#include <string.h>

#include "hw_tsys01.h"
#include "app_print.h"
#include "net_console.h"

/* Sensor bus: same i2c0 as all other sensors (GPIO21 SDA / GPIO22 SCL). */
static const struct device *const i2c_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0));

/* TSYS01 command set */
#define TSYS01_CMD_RESET     0x1E
#define TSYS01_CMD_PROM_BASE 0xA0   /* words at 0xA0,0xA2,...,0xAE */
#define TSYS01_CMD_CONVERT   0x48
#define TSYS01_CMD_ADC_READ  0x00
#define TSYS01_CONVERT_MS    10     /* datasheet max conversion time ~9.04 ms */

/* BMP180 identity check (both parts answer at 0x77) */
#define BMP180_REG_CHIPID    0xD0
#define BMP180_CHIPID        0x55

/* Driver state: probe result is cached so mission code can call
 * tsys01_read() every second without re-probing a missing sensor. */
static uint8_t  g_addr;            /* detected address (0x77 or 0x76) */
static uint16_t g_cal[8];          /* PROM words; k coefficients in [1..5] */
static enum {
    TSYS01_UNKNOWN = 0,            /* not probed yet */
    TSYS01_PRESENT,                /* probed OK, calibration loaded */
    TSYS01_ABSENT,                 /* probe failed — don't retry automatically */
} g_state;

/* Read one 16-bit big-endian PROM word. */
static int prom_read_word(uint8_t addr, int idx, uint16_t *out)
{
    uint8_t cmd = TSYS01_CMD_PROM_BASE + (uint8_t)(idx * 2);
    uint8_t buf[2];
    int rc = i2c_write_read(i2c_dev, addr, &cmd, 1, buf, sizeof(buf));
    if (rc == 0) {
        *out = ((uint16_t)buf[0] << 8) | buf[1];
    }
    return rc;
}

/* Heuristic: does a BMP180 (not a TSYS01) answer at this address? */
static bool looks_like_bmp180(uint8_t addr)
{
    uint8_t reg = BMP180_REG_CHIPID;
    uint8_t id = 0;
    if (i2c_write_read(i2c_dev, addr, &reg, 1, &id, 1) == 0 && id == BMP180_CHIPID) {
        return true;
    }
    return false;
}

/* Probe one address: reset, load PROM, sanity-check coefficients.
 * Returns 0 and fills g_cal on success. */
static int probe_addr(uint8_t addr)
{
    /* A BMP180 at this address would ACK everything below and give us
     * garbage calibration — detect and skip it explicitly. */
    if (looks_like_bmp180(addr)) {
        return -EEXIST;
    }

    uint8_t cmd = TSYS01_CMD_RESET;
    int rc = i2c_write(i2c_dev, &cmd, 1, addr);
    if (rc != 0) {
        return -ENODEV;   /* no ACK — nothing at this address */
    }
    k_msleep(10);         /* reset recovery */

    uint16_t cal[8];
    for (int i = 0; i < 8; i++) {
        rc = prom_read_word(addr, i, &cal[i]);
        if (rc != 0) {
            return -EIO;
        }
    }

    /* Coefficient sanity: k1..k5 must not be all-zero / all-ones, which is
     * what a floating bus or a blank device would return. */
    bool any_valid = false;
    for (int i = 1; i <= 5; i++) {
        if (cal[i] != 0x0000 && cal[i] != 0xFFFF) {
            any_valid = true;
        }
    }
    if (!any_valid) {
        return -EIO;
    }

    memcpy(g_cal, cal, sizeof(g_cal));
    g_addr = addr;
    return 0;
}

int tsys01_init(void)
{
    if (g_state == TSYS01_PRESENT) {
        return 0;
    }

    if (i2c_dev == NULL || !device_is_ready(i2c_dev)) {
        app_printk("[TSYS01] i2c0 not ready\r\n");
        g_state = TSYS01_ABSENT;
        return -ENODEV;
    }

    /* Match the bus speed used by the other sensor drivers. */
    (void)i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));

    /* Default address first (0x77), then the CSB-strapped alternative. */
    static const uint8_t candidates[] = { 0x77, 0x76 };
    for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
        int rc = probe_addr(candidates[i]);
        if (rc == 0) {
            g_state = TSYS01_PRESENT;
            app_printk("[TSYS01] detected at 0x%02x (k1..k5 = %u %u %u %u %u)\r\n",
                       g_addr, g_cal[1], g_cal[2], g_cal[3], g_cal[4], g_cal[5]);
            return 0;
        }
        if (rc == -EEXIST) {
            app_printk("[TSYS01] 0x%02x is a BMP180, skipping (address conflict)\r\n",
                       candidates[i]);
        }
    }

    g_state = TSYS01_ABSENT;
    return -ENODEV;
}

bool tsys01_is_present(void)
{
    return g_state == TSYS01_PRESENT;
}

int tsys01_read(double *temp_c)
{
    /* First use probes the sensor; a failed probe is cached so mission
     * loops don't hammer the bus every second for absent hardware. */
    if (g_state == TSYS01_UNKNOWN) {
        (void)tsys01_init();
    }
    if (g_state != TSYS01_PRESENT) {
        return -ENODEV;
    }

    /* Start conversion, wait, read the 24-bit result. */
    uint8_t cmd = TSYS01_CMD_CONVERT;
    int rc = i2c_write(i2c_dev, &cmd, 1, g_addr);
    if (rc != 0) {
        return rc;
    }
    k_msleep(TSYS01_CONVERT_MS);

    cmd = TSYS01_CMD_ADC_READ;
    uint8_t buf[3];
    rc = i2c_write_read(i2c_dev, g_addr, &cmd, 1, buf, sizeof(buf));
    if (rc != 0) {
        return rc;
    }

    uint32_t raw24 = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
    if (raw24 == 0) {
        return -EIO;      /* conversion not done / sensor glitch */
    }

    /* Datasheet polynomial over the 16 MSBs. */
    double adc = (double)(raw24 / 256U);
    double t = -2.0  * (double)g_cal[1] * 1e-21 * adc * adc * adc * adc
             +  4.0  * (double)g_cal[2] * 1e-16 * adc * adc * adc
             + -2.0  * (double)g_cal[3] * 1e-11 * adc * adc
             +  1.0  * (double)g_cal[4] * 1e-6  * adc
             + -1.5  * (double)g_cal[5] * 1e-2;

    if (temp_c) {
        *temp_c = t;
    }
    return 0;
}

int tsys01_selftest(void)
{
    app_printk("\r\n[TSYS01] --- self-test ---\r\n");

    /* 1. Bus */
    if (i2c_dev == NULL || !device_is_ready(i2c_dev)) {
        app_printk("[TSYS01] FAIL: i2c0 bus not ready\r\n");
        return -ENODEV;
    }
    app_printk("[TSYS01] 1. i2c0 bus ready ............ OK\r\n");

    /* 2. Probe + calibration (force a fresh probe so a re-plugged sensor
     *    is picked up without a reboot). */
    g_state = TSYS01_UNKNOWN;
    if (tsys01_init() != 0) {
        app_printk("[TSYS01] 2. probe 0x77/0x76 .......... FAIL (not found)\r\n");
        app_printk("[TSYS01] FAIL: sensor not detected\r\n");
        return -ENODEV;
    }
    app_printk("[TSYS01] 2. probe .................... OK (addr 0x%02x)\r\n", g_addr);
    app_printk("[TSYS01] 3. calibration PROM ......... OK "
               "(k1=%u k2=%u k3=%u k4=%u k5=%u)\r\n",
               g_cal[1], g_cal[2], g_cal[3], g_cal[4], g_cal[5]);

    /* 4. Take a few samples and check they are physically plausible. */
    double sum = 0.0;
    int good = 0;
    for (int i = 0; i < 3; i++) {
        double t;
        if (tsys01_read(&t) == 0) {
            sum += t;
            good++;
        }
        k_msleep(50);
    }
    if (good == 0) {
        app_printk("[TSYS01] 4. sample read ............. FAIL (no samples)\r\n");
        return -EIO;
    }
    double avg = sum / good;
    app_printk("[TSYS01] 4. sample read ............. OK (%d/3 samples, avg %.2f C)\r\n",
               good, avg);

    /* Operating range per datasheet is -40..+125 C; anything outside means
     * a wiring/calibration problem, not a real temperature. */
    if (avg < -40.0 || avg > 125.0) {
        app_printk("[TSYS01] 5. plausibility ............ FAIL (%.2f C out of range)\r\n", avg);
        return -ERANGE;
    }
    app_printk("[TSYS01] 5. plausibility ............ OK\r\n");
    app_printk("[TSYS01] --- PASS: %.2f C ---\r\n", avg);
    return 0;
}

void tsys01_stream_interactive(void)
{
    if (tsys01_init() != 0) {
        app_printk("[TSYS01] sensor not found\r\n");
        return;
    }

    app_printk("[TSYS01] streaming - press 'q' then ENTER to return\r\n");
    int64_t next = k_uptime_get();
    while (1) {
        double t;
        if (tsys01_read(&t) == 0) {
            app_printk("Water T = %.2f C\r\n", t);
        } else {
            app_printk("[TSYS01] read failed\r\n");
        }

        /* 1 Hz cadence with responsive quit polling in between. */
        next += 1000;
        while (k_uptime_get() < next) {
            if (console_quit_requested()) {
                app_printk("[TSYS01] exit requested -> back to menu\r\n");
                return;
            }
            k_sleep(K_MSEC(20));
        }
    }
}
