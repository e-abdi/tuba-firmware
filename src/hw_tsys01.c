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
 * Bus placement: the Celsius ships at address 0x77 — the same as the
 * BMP180 on the main sensor bus. To sidestep that conflict the TSYS01 has
 * its own bus: a bit-banged software I2C ("i2c_temp") on GPIO4 (SDA) /
 * GPIO13 (SCL). It's software rather than the SoC's second hardware I2C
 * controller because enabling that hardware peripheral caused an
 * intermittent cold-boot freeze (see the board overlay for details) —
 * the bit-banged driver only toggles plain GPIOs, sidestepping whatever
 * in the hardware controller's init path was responsible. The driver
 * probes i2c_temp first and falls back to i2c0 (where a BMP180 at 0x77
 * is detected via its chip-id register and skipped), so either wiring
 * works.
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

/* Candidate buses, in probe order: the dedicated (bit-banged) temperature
 * bus first, then the shared sensor bus as a fallback. */
static const struct device *const i2c_buses[] = {
    DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c_temp)),  /* GPIO4/13 — preferred */
    DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0)),      /* GPIO21/22 — fallback  */
};
static const char *const i2c_bus_names[] = { "i2c_temp", "i2c0" };

/* Bus the sensor was actually found on (set by tsys01_init). */
static const struct device *i2c_dev;
static const char *g_bus_name = "?";

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
static int prom_read_word(const struct device *bus, uint8_t addr, int idx, uint16_t *out)
{
    uint8_t cmd = TSYS01_CMD_PROM_BASE + (uint8_t)(idx * 2);
    uint8_t buf[2];
    int rc = i2c_write_read(bus, addr, &cmd, 1, buf, sizeof(buf));
    if (rc == 0) {
        *out = ((uint16_t)buf[0] << 8) | buf[1];
    }
    return rc;
}

/* Heuristic: does a BMP180 (not a TSYS01) answer at this address? */
static bool looks_like_bmp180(const struct device *bus, uint8_t addr)
{
    uint8_t reg = BMP180_REG_CHIPID;
    uint8_t id = 0;
    if (i2c_write_read(bus, addr, &reg, 1, &id, 1) == 0 && id == BMP180_CHIPID) {
        return true;
    }
    return false;
}

/* Probe one bus+address: reset, load PROM, sanity-check coefficients.
 * Returns 0 and fills g_cal on success. */
static int probe_addr(const struct device *bus, uint8_t addr)
{
    /* A BMP180 at this address would ACK everything below and give us
     * garbage calibration — detect and skip it explicitly. */
    if (looks_like_bmp180(bus, addr)) {
        return -EEXIST;
    }

    uint8_t cmd = TSYS01_CMD_RESET;
    int rc = i2c_write(bus, &cmd, 1, addr);
    if (rc != 0) {
        return -ENODEV;   /* no ACK — nothing at this address */
    }
    k_msleep(10);         /* reset recovery */

    uint16_t cal[8];
    for (int i = 0; i < 8; i++) {
        rc = prom_read_word(bus, addr, i, &cal[i]);
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

    /* Try each candidate bus (i2c_temp first) and each address (0x77 first). */
    static const uint8_t candidates[] = { 0x77, 0x76 };
    for (size_t b = 0; b < ARRAY_SIZE(i2c_buses); b++) {
        const struct device *bus = i2c_buses[b];
        if (bus == NULL || !device_is_ready(bus)) {
            continue;
        }

        /* Match the bus speed used by the other sensor drivers. */
        (void)i2c_configure(bus, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));

        for (size_t i = 0; i < ARRAY_SIZE(candidates); i++) {
            int rc = probe_addr(bus, candidates[i]);
            if (rc == 0) {
                i2c_dev = bus;
                g_bus_name = i2c_bus_names[b];
                g_state = TSYS01_PRESENT;
                app_printk("[TSYS01] detected at 0x%02x on %s (k1..k5 = %u %u %u %u %u)\r\n",
                           g_addr, g_bus_name,
                           g_cal[1], g_cal[2], g_cal[3], g_cal[4], g_cal[5]);
                return 0;
            }
            if (rc == -EEXIST) {
                app_printk("[TSYS01] %s 0x%02x is a BMP180, skipping\r\n",
                           i2c_bus_names[b], candidates[i]);
            }
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

    /* 1. At least one candidate bus must be up (i2c_temp preferred, i2c0 ok). */
    int buses_ready = 0;
    for (size_t b = 0; b < ARRAY_SIZE(i2c_buses); b++) {
        if (i2c_buses[b] != NULL && device_is_ready(i2c_buses[b])) {
            buses_ready++;
        }
    }
    if (buses_ready == 0) {
        app_printk("[TSYS01] FAIL: no I2C bus ready (i2c_temp/i2c0)\r\n");
        return -ENODEV;
    }
    app_printk("[TSYS01] 1. I2C buses ready ........... OK (%d of 2)\r\n", buses_ready);

    /* 2. Probe + calibration (force a fresh probe so a re-plugged sensor
     *    is picked up without a reboot). */
    g_state = TSYS01_UNKNOWN;
    if (tsys01_init() != 0) {
        app_printk("[TSYS01] 2. probe 0x77/0x76 .......... FAIL (not found on i2c_temp or i2c0)\r\n");
        app_printk("[TSYS01]    expected wiring: SDA=GPIO4, SCL=GPIO13 (i2c_temp)\r\n");
        app_printk("[TSYS01] FAIL: sensor not detected\r\n");
        return -ENODEV;
    }
    app_printk("[TSYS01] 2. probe .................... OK (addr 0x%02x on %s)\r\n",
               g_addr, g_bus_name);
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
