
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>
#include <string.h>
#include "net_console.h"

#define BMP180_ADDR     0x77
#define REG_CHIPID      0xD0
#define REG_CALIB_START 0xAA
#define REG_CTRL_MEAS   0xF4
#define REG_DATA_MSB    0xF6

/* Oversampling (we use ultra low power = 0) */
#define OSS 0

/* Console UART (for non-blocking 'q' detection) */
static const struct device *const uart_cons = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

/* I2C0 for sensors (optional; guard at runtime) */
static const struct device *const i2c0_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0));

static int i2c_reg_read_u8(const struct device *i2c, uint8_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_write_read(i2c, dev, &reg, 1, val, 1);
}
static int i2c_reg_read_u16_be(const struct device *i2c, uint8_t dev, uint8_t reg, uint16_t *val)
{
    uint8_t buf[2];
    int ret = i2c_write_read(i2c, dev, &reg, 1, buf, 2);
    if (ret == 0) {
        *val = ((uint16_t)buf[0] << 8) | buf[1];
    }
    return ret;
}
static int i2c_reg_write_u8(const struct device *i2c, uint8_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_write(i2c, buf, sizeof(buf), dev);
}

struct bmp180_cal {
    int16_t AC1, AC2, AC3;
    uint16_t AC4, AC5, AC6;
    int16_t B1, B2, MB, MC, MD;
};

/* Read factory calibration (11 * 2 bytes) */
static int bmp180_read_cal(struct bmp180_cal *c)
{
    uint8_t reg = REG_CALIB_START;
    uint8_t buf[22];
    int ret = i2c_write_read(i2c0_dev, BMP180_ADDR, &reg, 1, buf, sizeof(buf));
    if (ret) return ret;

    c->AC1 = (int16_t)((buf[0] << 8) | buf[1]);
    c->AC2 = (int16_t)((buf[2] << 8) | buf[3]);
    c->AC3 = (int16_t)((buf[4] << 8) | buf[5]);
    c->AC4 = (uint16_t)((buf[6] << 8) | buf[7]);
    c->AC5 = (uint16_t)((buf[8] << 8) | buf[9]);
    c->AC6 = (uint16_t)((buf[10] << 8) | buf[11]);
    c->B1  = (int16_t)((buf[12] << 8) | buf[13]);
    c->B2  = (int16_t)((buf[14] << 8) | buf[15]);
    c->MB  = (int16_t)((buf[16] << 8) | buf[17]);
    c->MC  = (int16_t)((buf[18] << 8) | buf[19]);
    c->MD  = (int16_t)((buf[20] << 8) | buf[21]);
    return 0;
}

static int bmp180_read_uncomp_temp(int32_t *UT)
{
    int ret = i2c_reg_write_u8(i2c0_dev, BMP180_ADDR, REG_CTRL_MEAS, 0x2E);
    if (ret) return ret;
    k_sleep(K_MSEC(5));
    uint16_t ut;
    ret = i2c_reg_read_u16_be(i2c0_dev, BMP180_ADDR, REG_DATA_MSB, &ut);
    if (ret) return ret;
    *UT = (int32_t)ut;
    return 0;
}

static int bmp180_read_uncomp_press(int32_t *UP)
{
    int ret = i2c_reg_write_u8(i2c0_dev, BMP180_ADDR, REG_CTRL_MEAS, 0x34 + (OSS << 6));
    if (ret) return ret;
    k_sleep(K_MSEC(8)); /* 4.5ms typical at OSS=0 */
    uint8_t reg = REG_DATA_MSB;
    uint8_t buf[3];
    ret = i2c_write_read(i2c0_dev, BMP180_ADDR, &reg, 1, buf, 3);
    if (ret) return ret;
    *UP = (((int32_t)buf[0] << 16) | ((int32_t)buf[1] << 8) | buf[2]) >> (8 - OSS);
    return 0;
}

/* Compensation algorithm from BMP180 datasheet */
static void bmp180_compensate(const struct bmp180_cal *c, int32_t UT, int32_t UP,
                              int32_t *T_cdec, int32_t *P_pa)
{
    int32_t X1 = ((UT - (int32_t)c->AC6) * (int32_t)c->AC5) >> 15;
    int32_t X2 = ((int32_t)c->MC << 11) / (X1 + c->MD);
    int32_t B5 = X1 + X2;

    *T_cdec = (B5 + 8) >> 4; /* 0.1 C */

    int32_t B6 = B5 - 4000;
    X1 = ((int32_t)c->B2 * ((B6 * B6) >> 12)) >> 11;
    X2 = ((int32_t)c->AC2 * B6) >> 11;
    int32_t X3 = X1 + X2;
    int32_t B3 = ((((int32_t)c->AC1 * 4 + X3) << OSS) + 2) >> 2;
    X1 = ((int32_t)c->AC3 * B6) >> 13;
    X2 = ((int32_t)c->B1 * ((B6 * B6) >> 12)) >> 16;
    X3 = ((X1 + X2) + 2) >> 2;
    uint32_t B4 = ((uint32_t)c->AC4 * (uint32_t)(X3 + 32768)) >> 15;
    uint32_t B7 = ((uint32_t)UP - (uint32_t)B3) * (50000U >> OSS);

    int32_t p;
    if (B7 < 0x80000000U) {
        p = (int32_t)((B7 << 1) / B4);
    } else {
        p = (int32_t)((B7 / B4) << 1);
    }

    X1 = (p >> 8) * (p >> 8);
    X1 = (X1 * 3038) >> 16;
    X2 = (-7357 * p) >> 16;
    p = p + ((X1 + X2 + 3791) >> 4);

    *P_pa = p;
}

/* Public API expected by the menu */
int bmp180_init(void)
{
    if (i2c0_dev == NULL || !device_is_ready(i2c0_dev)) {
        app_printk("[Internal Pressure] i2c0 not ready\r\n");
        return -ENODEV;
    }
    /* Ensure bus speed is at 100 kHz for BMP180 */
    (void)i2c_configure(i2c0_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_STANDARD));
    /* Probe sensor by reading CHIPID (should be 0x55) */
    uint8_t id = 0;
    if (i2c_reg_read_u8(i2c0_dev, BMP180_ADDR, REG_CHIPID, &id) || id != 0x55) {
        app_printk("[Internal Pressure] BMP180 not found (id=0x%02x)\r\n", id);
        return -ENODEV;
    }
    app_printk("[Internal Pressure] BMP180 detected (id=0x%02x) on i2c0\r\n", id);
    return 0;
}

static bool quit_requested(void)
{
    /* Prefer net console (WiFi) input: requires ENTER */
    char line[128];
    if (net_console_poll_line(line, sizeof(line), K_NO_WAIT)) {
        if ((line[0] == 'q' || line[0] == 'Q') && line[1] == '\0') return true;
    }
    if (!device_is_ready(uart_cons)) return false;
    uint8_t c;
    int rc = uart_poll_in(uart_cons, &c);
    return (rc == 0 && (c == 'q' || c == 'Q'));
}

void bmp180_stream_interactive(void)
{
    if (bmp180_init() != 0) {
        app_printk("[Internal Pressure] init failed\r\n");
        return;
    }

    struct bmp180_cal cal;
    if (bmp180_read_cal(&cal) != 0) {
        app_printk("[Internal Pressure] read calibration failed\r\n");
        return;
    }

    app_printk("[Internal Pressure] streaming — press 'q' then ENTER to return\r\n");

    int64_t next = k_uptime_get();
    while (1) {
        int32_t UT, UP, T_cdec, P_pa;
        if (bmp180_read_uncomp_temp(&UT) == 0 && bmp180_read_uncomp_press(&UP) == 0) {
            bmp180_compensate(&cal, UT, UP, &T_cdec, &P_pa);
            int32_t T_whole = T_cdec / 10;
            int32_t T_frac  = T_cdec >= 0 ? (T_cdec % 10) : -(T_cdec % 10);
            int32_t P_whole = P_pa / 1000;
            int32_t P_frac  = P_pa >= 0 ? (P_pa % 1000) : -(P_pa % 1000);
            app_printk("T=%d.%01d C, P=%d.%03d kPa\r\n", T_whole, T_frac, P_whole, P_frac);
        } else {
            app_printk("[Internal Pressure] read failed\r\n");
        }

        next += 1000;
        while (k_uptime_get() < next) {
            if (quit_requested()) {
                app_printk("[Internal Pressure] exit requested → back to menu\r\n");
                return;
            }
            k_sleep(K_MSEC(20));
        }
    }
}

/* Read a single compensated pressure sample (Pa). Returns 0 on success. */
int bmp180_read_pa(int32_t *out_pa)
{
    static struct bmp180_cal cal;
    static bool cal_done = false;
    if (!cal_done) {
        if (bmp180_read_cal(&cal) != 0) return -EIO;
        cal_done = true;
    }

    int32_t UT, UP, T_cdec, P_pa;
    if (bmp180_read_uncomp_temp(&UT) != 0) return -EIO;
    if (bmp180_read_uncomp_press(&UP) != 0) return -EIO;
    bmp180_compensate(&cal, UT, UP, &T_cdec, &P_pa);
    if (out_pa) *out_pa = P_pa;
    return 0;
}
