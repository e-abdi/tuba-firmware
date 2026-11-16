/*
 * sil_emul_bmp180.c — Emulated BMP180 (hull / internal pressure)
 *
 * Register model: 0xD0 chip id (0x55), 0xAA..0xBF calibration EEPROM
 * (datasheet example values), 0xF4 control, 0xF6..0xF8 result. The raw UT /
 * UP values are solved by bisection against the datasheet compensation so
 * that bmp180_read_pa() returns the internal pressure the bridge reports
 * (constant by default; the bridge can raise it to simulate a leak).
 */
#define DT_DRV_COMPAT tuba_sim_bmp180

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <string.h>

#include "sil_state.h"

struct bmp180_cal { int16_t AC1, AC2, AC3; uint16_t AC4, AC5, AC6; int16_t B1, B2, MB, MC, MD; };
static const struct bmp180_cal CAL = { 408, -72, -14383, 32741, 32757, 23153, 6190, 4, -32768, -8711, 2868 };

struct bmp180_emul_data {
    uint8_t reg;      /* register pointer */
    uint8_t ctrl;     /* last 0xF4 write */
    int32_t ut, up;   /* raw results for the pending measurement */
};

/* Datasheet compensation, OSS = 0 (identical to hw_bmp180.c). */
static void bmp180_compensate(int32_t UT, int32_t UP, int32_t *T_cdec, int32_t *P_pa)
{
    const struct bmp180_cal *c = &CAL;
    int32_t X1 = ((UT - (int32_t)c->AC6) * (int32_t)c->AC5) >> 15;
    int32_t X2 = ((int32_t)c->MC << 11) / (X1 + c->MD);
    int32_t B5 = X1 + X2;
    *T_cdec = (B5 + 8) >> 4;
    int32_t B6 = B5 - 4000;
    X1 = ((int32_t)c->B2 * ((B6 * B6) >> 12)) >> 11;
    X2 = ((int32_t)c->AC2 * B6) >> 11;
    int32_t X3 = X1 + X2;
    int32_t B3 = ((((int32_t)c->AC1 * 4 + X3) << 0) + 2) >> 2;
    X1 = ((int32_t)c->AC3 * B6) >> 13;
    X2 = ((int32_t)c->B1 * ((B6 * B6) >> 12)) >> 16;
    X3 = ((X1 + X2) + 2) >> 2;
    uint32_t B4 = ((uint32_t)c->AC4 * (uint32_t)(X3 + 32768)) >> 15;
    uint32_t B7 = ((uint32_t)UP - (uint32_t)B3) * 50000U;
    int32_t p = (B7 < 0x80000000U) ? (int32_t)((B7 << 1) / B4) : (int32_t)((B7 / B4) << 1);
    X1 = (p >> 8) * (p >> 8);
    X1 = (X1 * 3038) >> 16;
    X2 = (-7357 * p) >> 16;
    *P_pa = p + ((X1 + X2 + 3791) >> 4);
}

static void bmp180_solve(float temp_c, float press_pa, int32_t *ut, int32_t *up)
{
    int32_t t, p, lo, hi;
    int32_t t_target = (int32_t)(temp_c * 10.0f);
    lo = 0; hi = 65535;
    for (int i = 0; i < 17; i++) {
        int32_t mid = (lo + hi) / 2;
        bmp180_compensate(mid, 30000, &t, &p);
        if (t < t_target) lo = mid; else hi = mid;
    }
    *ut = hi;
    lo = 0; hi = 65535;
    for (int i = 0; i < 17; i++) {
        int32_t mid = (lo + hi) / 2;
        bmp180_compensate(*ut, mid, &t, &p);
        if (p < (int32_t)press_pa) lo = mid; else hi = mid;
    }
    *up = hi;
}

static uint8_t bmp180_reg_read(struct bmp180_emul_data *d, uint8_t reg)
{
    if (reg == 0xD0) {
        return 0x55;
    }
    if (reg >= 0xAA && reg <= 0xBF) {
        const int16_t w[11] = { CAL.AC1, CAL.AC2, CAL.AC3, (int16_t)CAL.AC4, (int16_t)CAL.AC5,
                                (int16_t)CAL.AC6, CAL.B1, CAL.B2, CAL.MB, CAL.MC, CAL.MD };
        uint16_t v = (uint16_t)w[(reg - 0xAA) / 2];
        return ((reg - 0xAA) & 1) ? (v & 0xFF) : (v >> 8);
    }
    if (reg == 0xF4) {
        return d->ctrl;
    }
    if (reg >= 0xF6 && reg <= 0xF8) {
        /* temperature: 16 bits in F6/F7; pressure (OSS=0): UP<<8 in F6..F8 */
        uint32_t raw = (d->ctrl == 0x2E) ? (uint32_t)d->ut << 8 : (uint32_t)d->up << 8;
        if (d->ctrl == 0x2E) {
            return (reg == 0xF6) ? ((d->ut >> 8) & 0xFF) : (reg == 0xF7) ? (d->ut & 0xFF) : 0;
        }
        return (reg == 0xF6) ? ((raw >> 16) & 0xFF) : (reg == 0xF7) ? ((raw >> 8) & 0xFF) : (raw & 0xFF);
    }
    return 0;
}

static int bmp180_emul_transfer(const struct emul *target, struct i2c_msg *msgs,
                                int num_msgs, int addr)
{
    struct bmp180_emul_data *d = target->data;
    struct sil_sensors s;
    ARG_UNUSED(addr);

    if (!sil_state_get(&s)) {
        return -EIO;
    }
    for (int i = 0; i < num_msgs; i++) {
        struct i2c_msg *m = &msgs[i];
        if (!(m->flags & I2C_MSG_READ)) {
            if (m->len == 0) {
                /* Zero-length probe: the real chip ignores it (sensors.c
                 * notes it does not answer reliably) — report nothing. */
                return -EIO;
            }
            d->reg = m->buf[0];
            if (m->len >= 2 && d->reg == 0xF4) {
                d->ctrl = m->buf[1];
                bmp180_solve(s.pkt.int_temp_c, s.pkt.int_pressure_pa, &d->ut, &d->up);
            }
            continue;
        }
        for (uint32_t k = 0; k < m->len; k++) {
            m->buf[k] = bmp180_reg_read(d, (uint8_t)(d->reg + k));
        }
    }
    return 0;
}

static int bmp180_emul_init(const struct emul *target, const struct device *parent)
{
    struct bmp180_emul_data *d = target->data;
    ARG_UNUSED(parent);
    d->ctrl = 0x2E;
    d->ut = 27898;
    d->up = 34331;
    return 0;
}

static const struct i2c_emul_api bmp180_emul_api = { .transfer = bmp180_emul_transfer };

#define BMP180_EMUL(n)                                                             \
    static struct bmp180_emul_data bmp180_emul_data_##n;                           \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);               \
    EMUL_DT_INST_DEFINE(n, bmp180_emul_init, &bmp180_emul_data_##n, NULL,          \
                        &bmp180_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(BMP180_EMUL)
