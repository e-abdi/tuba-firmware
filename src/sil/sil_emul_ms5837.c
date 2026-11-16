/*
 * sil_emul_ms5837.c — Emulated MS5837-30BA (external pressure / temperature)
 *
 * Answers the exact MEAS command set hw_ms5837.c uses (0x1E reset, 0xA0..
 * 0xAC PROM with CRC-4, 0x4x/0x5x conversions, 0x00 ADC read) with raw D1/D2
 * values chosen so that the firmware's own compensation math yields the
 * pressure and temperature the Gazebo bridge reports. The PROM is a
 * plausible real-sensor calibration (model detection -> 30BA).
 */
#define DT_DRV_COMPAT tuba_sim_ms5837

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <string.h>

#include "sil_state.h"

struct ms5837_emul_data {
    uint8_t  cmd;
    uint32_t adc;        /* result of the last conversion */
    uint16_t prom[8];
};

/* Same CRC-4 the driver verifies (MS5837 datasheet). */
static uint8_t ms_crc4(const uint16_t prom_in[8])
{
    uint16_t n_prom[8];
    uint16_t n_rem = 0;
    memcpy(n_prom, prom_in, sizeof(n_prom));
    n_prom[0] &= 0x0FFF;
    n_prom[7] = 0;
    for (int i = 0; i < 16; i++) {
        n_rem ^= (i & 1) ? (n_prom[i >> 1] & 0x00FF) : (n_prom[i >> 1] >> 8);
        for (int b = 8; b > 0; b--) {
            n_rem = (n_rem & 0x8000) ? ((n_rem << 1) ^ 0x3000) : (n_rem << 1);
        }
    }
    return (n_rem >> 12) & 0x000F;
}

/* Raw D1/D2 for a wanted temperature [C] and pressure [mbar], inverting the
 * 30BA compensation from hw_ms5837.c (second-order terms included). */
static void ms5837_inverse(const uint16_t *C, double temp_c, double p_mbar,
                           uint32_t *d1, uint32_t *d2)
{
    int32_t temp_target = (int32_t)(temp_c * 100.0);
    int32_t dT = (int32_t)(((int64_t)(temp_target - 2000) * 8388608LL) / C[6]);

    /* One refinement so the firmware's Ti correction lands on target. */
    for (int it = 0; it < 2; it++) {
        int32_t TEMP = 2000 + (int32_t)((int64_t)dT * C[6] / 8388608LL);
        int32_t Ti = (TEMP / 100 < 20) ? (int32_t)((3LL * dT * dT) / 8589934592LL)
                                       : (int32_t)((2LL * dT * dT) / 137438953472LL);
        int32_t err = (TEMP - Ti) - temp_target;
        dT -= (int32_t)(((int64_t)err * 8388608LL) / C[6]);
    }

    int32_t TEMP = 2000 + (int32_t)((int64_t)dT * C[6] / 8388608LL);
    int64_t SENS = (int64_t)C[1] * 32768LL + ((int64_t)C[3] * dT) / 256LL;
    int64_t OFF  = (int64_t)C[2] * 65536LL + ((int64_t)C[4] * dT) / 128LL;
    int32_t OFFi, SENSi;
    if (TEMP / 100 < 20) {
        OFFi  = (int32_t)((3LL * (TEMP - 2000) * (TEMP - 2000)) / 2);
        SENSi = (int32_t)((5LL * (TEMP - 2000) * (TEMP - 2000)) / 8);
        if (TEMP / 100 < -15) {
            OFFi  += (int32_t)(7LL * (TEMP + 1500) * (TEMP + 1500));
            SENSi += (int32_t)(4LL * (TEMP + 1500) * (TEMP + 1500));
        }
    } else {
        OFFi  = (int32_t)(((TEMP - 2000) * (TEMP - 2000)) / 16);
        SENSi = 0;
    }
    int64_t OFF2 = OFF - OFFi, SENS2 = SENS - SENSi;
    int64_t P_int = (int64_t)(p_mbar * 10.0 + 0.5);
    int64_t D1 = ((P_int * 8192LL + OFF2) * 2097152LL) / SENS2;
    if (D1 < 0) D1 = 0;
    if (D1 > 16777215) D1 = 16777215;

    *d1 = (uint32_t)D1;
    *d2 = (uint32_t)(dT + (int32_t)C[5] * 256);
}

static int ms5837_emul_transfer(const struct emul *target, struct i2c_msg *msgs,
                                int num_msgs, int addr)
{
    struct ms5837_emul_data *d = target->data;
    struct sil_sensors s;
    ARG_UNUSED(addr);

    if (!sil_state_get(&s)) {
        return -EIO;    /* bridge not running: nothing on the bus */
    }

    for (int i = 0; i < num_msgs; i++) {
        struct i2c_msg *m = &msgs[i];
        if (!(m->flags & I2C_MSG_READ)) {
            if (m->len == 0) {
                continue;
            }
            d->cmd = m->buf[0];
            if ((d->cmd & 0xF0) == 0x40 || (d->cmd & 0xF0) == 0x50) {
                uint32_t d1, d2;
                ms5837_inverse(d->prom, s.pkt.water_temp_c,
                               s.pkt.ext_pressure_pa / 100.0, &d1, &d2);
                d->adc = ((d->cmd & 0xF0) == 0x40) ? d1 : d2;
            }
            continue;
        }
        memset(m->buf, 0, m->len);
        if (d->cmd >= 0xA0 && d->cmd <= 0xAE && m->len >= 2) {
            uint16_t w = d->prom[(d->cmd - 0xA0) / 2];
            m->buf[0] = w >> 8;
            m->buf[1] = w & 0xFF;
        } else if (d->cmd == 0x00 && m->len >= 3) {
            m->buf[0] = (d->adc >> 16) & 0xFF;
            m->buf[1] = (d->adc >> 8) & 0xFF;
            m->buf[2] = d->adc & 0xFF;
        }
    }
    return 0;
}

static int ms5837_emul_init(const struct emul *target, const struct device *parent)
{
    struct ms5837_emul_data *d = target->data;
    ARG_UNUSED(parent);
    static const uint16_t cal[8] = { 0x00A2, 34982, 36352, 20328, 22354, 26646, 26146, 0 };
    memcpy(d->prom, cal, sizeof(d->prom));
    d->prom[0] = (d->prom[0] & 0x0FFF) | ((uint16_t)ms_crc4(d->prom) << 12);
    return 0;
}

static const struct i2c_emul_api ms5837_emul_api = { .transfer = ms5837_emul_transfer };

#define MS5837_EMUL(n)                                                             \
    static struct ms5837_emul_data ms5837_emul_data_##n;                           \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);               \
    EMUL_DT_INST_DEFINE(n, ms5837_emul_init, &ms5837_emul_data_##n, NULL,          \
                        &ms5837_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(MS5837_EMUL)
