/*
 * sil_emul_tsys01.c — Emulated TSYS01 (Blue Robotics Celsius) water temperature
 *
 * Lives on the i2c_temp bus like the real part. Commands: 0x1E reset,
 * 0xA0..0xAE PROM (k4..k0 in words 1..5), 0x48 convert, 0x00 read 24-bit
 * ADC. The ADC value is found by bisection so that the datasheet
 * polynomial in hw_tsys01.c returns the bridge's water temperature.
 */
#define DT_DRV_COMPAT tuba_sim_tsys01

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <string.h>

#include "sil_state.h"

struct tsys01_emul_data {
    uint8_t  cmd;
    uint32_t adc24;
    uint16_t prom[8];
};

static double tsys01_poly(const uint16_t *k, double adc)
{
    return -2.0 * k[1] * 1e-21 * adc * adc * adc * adc
         +  4.0 * k[2] * 1e-16 * adc * adc * adc
         + -2.0 * k[3] * 1e-11 * adc * adc
         +  1.0 * k[4] * 1e-6  * adc
         + -1.5 * k[5] * 1e-2;
}

/* The polynomial is monotonically increasing over the 16-bit range with
 * these coefficients (checked offline), so bisection is enough. */
static uint32_t tsys01_adc_for(const uint16_t *k, double temp_c)
{
    double lo = 0.0, hi = 65535.0;
    for (int i = 0; i < 24; i++) {
        double mid = 0.5 * (lo + hi);
        if (tsys01_poly(k, mid) < temp_c) lo = mid; else hi = mid;
    }
    return ((uint32_t)(hi + 0.5)) << 8;
}

static int tsys01_emul_transfer(const struct emul *target, struct i2c_msg *msgs,
                                int num_msgs, int addr)
{
    struct tsys01_emul_data *d = target->data;
    struct sil_sensors s;
    ARG_UNUSED(addr);

    if (!sil_state_get(&s)) {
        return -EIO;
    }
    for (int i = 0; i < num_msgs; i++) {
        struct i2c_msg *m = &msgs[i];
        if (!(m->flags & I2C_MSG_READ)) {
            if (m->len == 0) {
                /* The real TSYS01 NAKs an empty probe (see sensors.c). */
                return -EIO;
            }
            d->cmd = m->buf[0];
            if (d->cmd == 0x48) {
                d->adc24 = tsys01_adc_for(d->prom, s.pkt.water_temp_c);
            }
            continue;
        }
        memset(m->buf, 0, m->len);
        if (d->cmd >= 0xA0 && d->cmd <= 0xAE && m->len >= 2) {
            uint16_t w = d->prom[(d->cmd - 0xA0) / 2];
            m->buf[0] = w >> 8;
            m->buf[1] = w & 0xFF;
        } else if (d->cmd == 0x00 && m->len >= 3) {
            m->buf[0] = (d->adc24 >> 16) & 0xFF;
            m->buf[1] = (d->adc24 >> 8) & 0xFF;
            m->buf[2] = d->adc24 & 0xFF;
        }
    }
    return 0;
}

static int tsys01_emul_init(const struct emul *target, const struct device *parent)
{
    struct tsys01_emul_data *d = target->data;
    ARG_UNUSED(parent);
    /* word0 = serial/CRC (unused), words 1..5 = k4..k0 (typical values) */
    static const uint16_t cal[8] = { 0x1234, 28446, 24926, 36016, 32791, 40781, 0, 0 };
    memcpy(d->prom, cal, sizeof(d->prom));
    return 0;
}

static const struct i2c_emul_api tsys01_emul_api = { .transfer = tsys01_emul_transfer };

#define TSYS01_EMUL(n)                                                             \
    static struct tsys01_emul_data tsys01_emul_data_##n;                           \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);               \
    EMUL_DT_INST_DEFINE(n, tsys01_emul_init, &tsys01_emul_data_##n, NULL,          \
                        &tsys01_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(TSYS01_EMUL)
