#include "hw_gps.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>   /* atof, strtod */
#include <stdio.h>
#include <ctype.h>

/* u-blox DDC (I2C) */
#define UBLOX_I2C_ADDR 0x42
#define REG_LEN_LSB    0xFD
#define REG_LEN_MSB    0xFE
#define REG_STREAM     0xFF
#define BURST_MAX      64

/* I2C1 (GP4/GP5 on Pico per board overlay) */
static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

/* Console UART for nonblocking keypress checks */
static const struct device *const uart_console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static bool quit_requested(void)
{
    if (!device_is_ready(uart_console)) {
        return false;
    }
    unsigned char c;
    int rc = uart_poll_in(uart_console, &c);
    return (rc == 0 && (c == 'q' || c == 'Q'));
}

static int ublox_len(uint16_t *out)
{
    uint8_t lsb=0, msb=0;
    int rc = i2c_reg_read_byte(i2c_dev, UBLOX_I2C_ADDR, REG_LEN_LSB, &lsb);
    if (rc) return rc;
    rc = i2c_reg_read_byte(i2c_dev, UBLOX_I2C_ADDR, REG_LEN_MSB, &msb);
    if (rc) return rc;
    *out = (uint16_t)lsb | ((uint16_t)msb << 8);
    return 0;
}

static int ublox_read(uint8_t *buf, size_t n)
{
    return i2c_burst_read(i2c_dev, UBLOX_I2C_ADDR, REG_STREAM, buf, n);
}

/* NMEA checksum: XOR of characters between '$' and '*' must match the two hex digits after '*' */
static bool nmea_checksum_ok(const char *line)
{
    if (!line || line[0] != '$') return false;
    unsigned int sum = 0;
    const char *p = line + 1;
    while (*p && *p != '*' && *p != '\r' && *p != '\n') {
        sum ^= (unsigned char)(*p);
        p++;
    }
    if (*p != '*') return false;
    p++;
    unsigned int want = 0;
    for (int i = 0; i < 2; i++) {
        char c = p[i];
        if (!c) return false;
        unsigned int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = 10 + (c - 'A');
        else if (c >= 'a' && c <= 'f') v = 10 + (c - 'a');
        else return false;
        want = (want << 4) | v;
    }
    return (sum & 0xFFu) == (want & 0xFFu);
}

/* Robust convert NMEA ddmm.mmmm (or dddmm.mmmm) -> decimal degrees */
static bool nmea_to_deg(const char *field, char hemi, double *out_deg)
{
    if (!field || !*field) return false;
    char *endp = NULL;
    double raw = strtod(field, &endp);    /* e.g., 5959.1234 for 59° 59.1234' */
    if (endp == field) return false;      /* parse failed */
    int degs = (int)(raw / 100.0);
    double minutes = raw - (double)degs * 100.0;
    if (minutes < 0.0 || minutes >= 60.0) return false;
    double deg = (double)degs + (minutes / 60.0);
    if (hemi == 'S' || hemi == 'W') deg = -deg;
    *out_deg = deg;
    return true;
}

/* Parse a $--RMC line: out_status=A/V, out_lat/lon set when A */
static bool parse_rmc(const char *line, char *out_status, double *out_lat, double *out_lon, bool *has_coords)
{
    if ((strncmp(line, "$GNRMC", 6) != 0 && strncmp(line, "$GPRMC", 6) != 0) ||
        !nmea_checksum_ok(line)) {
        return false;
    }

    int field = 0;
    char status = 'V';
    char lat[20] = {0}, lon[20] = {0};
    char ns='N', ew='E';
    size_t idx_lat=0, idx_lon=0;

    for (size_t i=0; line[i] && line[i] != '*'; i++) {
        char c = line[i];
        if (c == ',') { field++; continue; }
        if (field == 2) { status = c; }
        else if (field == 3) { if (idx_lat < sizeof(lat)-1) lat[idx_lat++] = c; }
        else if (field == 4) { if (c=='N'||c=='S') ns = c; else return false; }
        else if (field == 5) { if (idx_lon < sizeof(lon)-1) lon[idx_lon++] = c; }
        else if (field == 6) { if (c=='E'||c=='W') ew = c; else return false; }
    }

    *out_status = status;
    *has_coords = false;

    /* Ensure null-terminated strings */
    lat[idx_lat] = '\0';
    lon[idx_lon] = '\0';

    if (status == 'A') {
        double dlat=0.0, dlon=0.0;
        if (nmea_to_deg(lat, ns, &dlat) && nmea_to_deg(lon, ew, &dlon)) {
            *out_lat = dlat;
            *out_lon = dlon;
            *has_coords = true;
        }
    }
    return true;
}

void gps_fix_interactive(void)
{
    if (!device_is_ready(i2c_dev)) {
        k_sleep(K_MSEC(200));
        if (!device_is_ready(i2c_dev)) {
            app_printk("[GPS] I2C1 not ready\r\n");
            return;
        }
    }

    /* 400 kHz for faster draining of the DDC stream */
    (void)i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST));

    app_printk("[GPS] Watching for fix. Press 'q' to cancel.\r\n");

    uint8_t buf[BURST_MAX];
    char line[256];
    size_t llen = 0;
    int64_t last_tick = 0;

    while (1) {
        if (quit_requested()) {
            app_printk("[GPS] exit requested → back to menu\r\n");
            return;
        }

        uint16_t avail = 0;
        if (ublox_len(&avail) != 0) {
            k_sleep(K_MSEC(50));
            continue;
        }
        while (avail > 0) {
            size_t chunk = (avail > BURST_MAX) ? BURST_MAX : avail;
            if (ublox_read(buf, chunk) != 0) {
                break;
            }
            for (size_t i = 0; i < chunk; i++) {
                char c = (char)buf[i];
                if (c == '\n' || c == '\r') {
                    if (llen > 0) {
                        line[llen] = '\0';
                        char status;
                        double lat=0, lon=0;
                        bool has_coords=false;
                        if (parse_rmc(line, &status, &lat, &lon, &has_coords)) {
                            if (status == 'A' && has_coords &&
                                lat >= -90.0 && lat <= 90.0 &&
                                lon >= -180.0 && lon <= 180.0) {
                                app_printk("A %.6f %.6f\r\n", lat, lon);
                                return;
                            }
                        }
                        llen = 0;
                    }
                } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
                    if (llen < sizeof(line)-1) {
                        line[llen++] = c;
                    } else {
                        llen = 0; /* too long, resync */
                    }
                }
            }
            avail -= chunk;

            int64_t now = k_uptime_get();
            if (now - last_tick >= 1000) {
                app_printk("V");
                last_tick = now;
            }
        }
        k_sleep(K_MSEC(5));
    }
}

