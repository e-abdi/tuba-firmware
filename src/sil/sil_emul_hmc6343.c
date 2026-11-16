/*
 * sil_emul_hmc6343.c — Emulated HMC6343 tilt-compensated compass
 *
 * Command-driven like the real part: 0x50 posts heading/pitch/roll (6 bytes,
 * tenths of a degree, big-endian), 0xE1/0xF1 read/write EEPROM (the driver
 * checks the OM1 orientation byte at 0x04 and expects 0x04), everything
 * else (run/standby/calibrate/reset) is acknowledged and ignored.
 */
#define DT_DRV_COMPAT tuba_sim_hmc6343

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <string.h>

#include "sil_state.h"

enum hmc_pending { HMC_NONE, HMC_HPR, HMC_EEPROM };

struct hmc6343_emul_data {
    enum hmc_pending pending;
    uint8_t ee_addr;
    uint8_t eeprom[256];
};

static void put_be16(uint8_t *b, float deg)
{
    int16_t v = (int16_t)(deg * 10.0f);
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)(v & 0xFF);
}

static int hmc6343_emul_transfer(const struct emul *target, struct i2c_msg *msgs,
                                 int num_msgs, int addr)
{
    struct hmc6343_emul_data *d = target->data;
    struct sil_sensors s;
    ARG_UNUSED(addr);

    if (!sil_state_get(&s)) {
        return -EIO;
    }
    for (int i = 0; i < num_msgs; i++) {
        struct i2c_msg *m = &msgs[i];
        if (!(m->flags & I2C_MSG_READ)) {
            if (m->len == 0) {
                continue;        /* address probe: ACK */
            }
            switch (m->buf[0]) {
            case 0x50:
                d->pending = HMC_HPR;
                break;
            case 0xE1:
                if (m->len >= 2) {
                    d->ee_addr = m->buf[1];
                    d->pending = HMC_EEPROM;
                }
                break;
            case 0xF1:
                if (m->len >= 3) {
                    d->eeprom[m->buf[1]] = m->buf[2];
                }
                break;
            default:             /* 0x75 run, 0x74 UF, 0x71/0x7E cal, 0x82 reset */
                break;
            }
            continue;
        }
        memset(m->buf, 0, m->len);
        if (d->pending == HMC_HPR && m->len >= 6) {
            float hdg = s.pkt.heading_deg;
            while (hdg < 0.0f) hdg += 360.0f;
            while (hdg >= 360.0f) hdg -= 360.0f;
            put_be16(&m->buf[0], hdg);
            put_be16(&m->buf[2], s.pkt.pitch_deg);
            put_be16(&m->buf[4], s.pkt.roll_deg);
        } else if (d->pending == HMC_EEPROM && m->len >= 1) {
            m->buf[0] = d->eeprom[d->ee_addr];
        }
        d->pending = HMC_NONE;
    }
    return 0;
}

static int hmc6343_emul_init(const struct emul *target, const struct device *parent)
{
    struct hmc6343_emul_data *d = target->data;
    ARG_UNUSED(parent);
    memset(d->eeprom, 0, sizeof(d->eeprom));
    d->eeprom[0x00] = 0x32;   /* slave address */
    d->eeprom[0x02] = 0x11;   /* software version */
    d->eeprom[0x04] = 0x04;   /* OM1: upright flat — what the driver wants */
    return 0;
}

static const struct i2c_emul_api hmc6343_emul_api = { .transfer = hmc6343_emul_transfer };

#define HMC6343_EMUL(n)                                                            \
    static struct hmc6343_emul_data hmc6343_emul_data_##n;                         \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);               \
    EMUL_DT_INST_DEFINE(n, hmc6343_emul_init, &hmc6343_emul_data_##n, NULL,        \
                        &hmc6343_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(HMC6343_EMUL)
