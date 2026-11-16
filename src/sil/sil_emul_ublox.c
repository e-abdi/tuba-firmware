/*
 * sil_emul_ublox.c — Emulated u-blox GPS on the I2C "DDC" port
 *
 * Registers 0xFD/0xFE report the buffered byte count, 0xFF streams the
 * data. Once a second a $GPRMC sentence (plus $GPGGA for completeness) is
 * queued from the bridge's position and UTC time. Status is 'A' only while
 * the bridge says the antenna is above the surface (SIL_F_GPS_FIX), so a
 * submerged glider waits for a fix exactly like the real one.
 */
#define DT_DRV_COMPAT tuba_sim_ublox

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2c_emul.h>
#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "sil_state.h"

#define UBLOX_FIFO 512

struct ublox_emul_data {
    uint8_t reg;
    uint8_t fifo[UBLOX_FIFO];
    uint16_t head, count;
    int64_t last_gen_ms;
};

static void fifo_push(struct ublox_emul_data *d, const char *s)
{
    size_t n = strlen(s);
    if (d->count + n > UBLOX_FIFO) {
        return;              /* reader is not keeping up: drop the sentence */
    }
    for (size_t i = 0; i < n; i++) {
        d->fifo[(d->head + d->count) % UBLOX_FIFO] = (uint8_t)s[i];
        d->count++;
    }
}

static uint8_t fifo_pop(struct ublox_emul_data *d)
{
    if (d->count == 0) {
        return 0xFF;         /* u-blox returns 0xFF when the buffer is empty */
    }
    uint8_t c = d->fifo[d->head];
    d->head = (d->head + 1) % UBLOX_FIFO;
    d->count--;
    return c;
}

/* Days since 1970-01-01 -> civil date (Howard Hinnant's algorithm). */
static void civil_from_days(int64_t z, int *y, int *m, int *dd)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (mm <= 2));
    *m = (int)mm;
    *dd = (int)d;
}

static void nmea_finish(char *buf, size_t len)
{
    /* buf holds "$....", append *CS\r\n */
    unsigned cs = 0;
    for (const char *p = buf + 1; *p; p++) {
        cs ^= (unsigned char)*p;
    }
    size_t n = strlen(buf);
    snprintf(buf + n, len - n, "*%02X\r\n", cs & 0xFF);
}

static void deg_to_nmea(double deg, bool is_lat, char *out, size_t len, char *hemi)
{
    *hemi = is_lat ? (deg < 0 ? 'S' : 'N') : (deg < 0 ? 'W' : 'E');
    deg = fabs(deg);
    int whole = (int)deg;
    double minutes = (deg - whole) * 60.0;
    snprintf(out, len, is_lat ? "%02d%07.4f" : "%03d%07.4f", whole, minutes);
}

static void ublox_generate(struct ublox_emul_data *d, const struct sil_sens_pkt *p)
{
    int64_t t = p->utc_unix_s;
    int64_t days = (t >= 0 ? t : t - 86399) / 86400;
    int secs = (int)(t - days * 86400);
    int y, mo, dd;
    civil_from_days(days, &y, &mo, &dd);
    int hh = secs / 3600, mi = (secs / 60) % 60, ss = secs % 60;
    bool fix = (p->flags & SIL_F_GPS_FIX) != 0;

    char lat[16], lon[16], ns, ew;
    deg_to_nmea(p->lat_deg, true, lat, sizeof(lat), &ns);
    deg_to_nmea(p->lon_deg, false, lon, sizeof(lon), &ew);

    char line[128];
    if (fix) {
        snprintf(line, sizeof(line), "$GPRMC,%02d%02d%02d.00,A,%s,%c,%s,%c,0.0,%.1f,%02d%02d%02d,,,A",
                 hh, mi, ss, lat, ns, lon, ew, (double)p->heading_deg, dd, mo, y % 100);
    } else {
        snprintf(line, sizeof(line), "$GPRMC,%02d%02d%02d.00,V,,,,,,,%02d%02d%02d,,,N",
                 hh, mi, ss, dd, mo, y % 100);
    }
    nmea_finish(line, sizeof(line));
    fifo_push(d, line);

    if (fix) {
        snprintf(line, sizeof(line), "$GPGGA,%02d%02d%02d.00,%s,%c,%s,%c,1,08,1.0,0.0,M,0.0,M,,",
                 hh, mi, ss, lat, ns, lon, ew);
    } else {
        snprintf(line, sizeof(line), "$GPGGA,%02d%02d%02d.00,,,,,0,00,99.9,,M,,M,,", hh, mi, ss);
    }
    nmea_finish(line, sizeof(line));
    fifo_push(d, line);
}

static int ublox_emul_transfer(const struct emul *target, struct i2c_msg *msgs,
                               int num_msgs, int addr)
{
    struct ublox_emul_data *d = target->data;
    struct sil_sensors s;
    ARG_UNUSED(addr);

    if (!sil_state_get(&s)) {
        return -EIO;
    }
    int64_t now = k_uptime_get();
    if (now - d->last_gen_ms >= 1000) {
        d->last_gen_ms = now;
        ublox_generate(d, &s.pkt);
    }

    for (int i = 0; i < num_msgs; i++) {
        struct i2c_msg *m = &msgs[i];
        if (!(m->flags & I2C_MSG_READ)) {
            if (m->len >= 1) {
                d->reg = m->buf[0];
            }
            continue;
        }
        for (uint32_t k = 0; k < m->len; k++) {
            switch (d->reg) {
            case 0xFD: m->buf[k] = d->count & 0xFF; d->reg = 0xFE; break;
            case 0xFE: m->buf[k] = (d->count >> 8) & 0xFF; d->reg = 0xFF; break;
            default:   m->buf[k] = fifo_pop(d); d->reg = 0xFF; break;
            }
        }
    }
    return 0;
}

static int ublox_emul_init(const struct emul *target, const struct device *parent)
{
    struct ublox_emul_data *d = target->data;
    ARG_UNUSED(parent);
    memset(d, 0, sizeof(*d));
    d->reg = 0xFF;
    return 0;
}

static const struct i2c_emul_api ublox_emul_api = { .transfer = ublox_emul_transfer };

#define UBLOX_EMUL(n)                                                              \
    static struct ublox_emul_data ublox_emul_data_##n;                             \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);               \
    EMUL_DT_INST_DEFINE(n, ublox_emul_init, &ublox_emul_data_##n, NULL,            \
                        &ublox_emul_api, NULL)

DT_INST_FOREACH_STATUS_OKAY(UBLOX_EMUL)
