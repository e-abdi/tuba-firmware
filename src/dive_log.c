/*
 * dive_log.c — Seaglider-style structured dive records
 *
 * See dive_log.h for the record set and the units caveat. Everything here
 * is a formatted app_printk(): the records ride the same stream as the
 * human-readable output, reach every console, and land in the OpenLog
 * capture, which is what the basestation converter parses.
 */
#include <zephyr/kernel.h>
#include <math.h>

#include "app_params.h"
#include "app_time.h"
#include "dive_log.h"
#include "version.h"

/* Seaglider writes positions as degrees+decimal minutes (4743.9064 means
 * 47 deg 43.9064'). Keep that convention so basestation code that already
 * parses $GPS lines needs no special case. */
static double deg_to_ddmm(double deg)
{
    double sign = (deg < 0.0) ? -1.0 : 1.0;
    double a = fabs(deg);
    double d = floor(a);
    return sign * (d * 100.0 + (a - d) * 60.0);
}

void dive_log_header(uint32_t mission_num, uint32_t dive_num,
                     const struct app_params *p, bool simulated)
{
    struct app_time_utc tm;
    app_time_get(&tm);

    app_printk("$VERSION,%d,%s\r\n", BUILD_NUMBER, BUILD_DATE);
    app_printk("$GLIDER,tuba\r\n");
    app_printk("$MISSION,%u\r\n", mission_num);
    app_printk("$DIVE,%u\r\n", dive_num);
    app_printk("$SIMULATE,%d\r\n", simulated ? 1 : 0);
    /* Seaglider's "start:" line is month day year hour min sec (UTC). */
    app_printk("$START,%u %u %u %u %u %u\r\n",
               (unsigned)tm.month, (unsigned)tm.day, (unsigned)tm.year,
               (unsigned)tm.hour, (unsigned)tm.minute, (unsigned)tm.second);

    /* Full parameter dump, so the capture is self-describing. Names track
     * the Seaglider parameter they correspond to where one exists. */
    app_printk("$D_TGT,%d\r\n", (int)p->dive_depth_m);
    app_printk("$D_ABORT,%u\r\n", p->max_depth_m);
    app_printk("$D_SURF,1\r\n");
    app_printk("$T_DIVE,%u\r\n", p->dive_timeout_min);
    app_printk("$T_MISSION,%u\r\n", p->t_mission_min);
    app_printk("$T_RSLEEP,%u\r\n", p->t_rsleep_min);
    app_printk("$HEADING,%d\r\n", p->desired_heading_deg);
    app_printk("$N_SENSOR_TRIES,%u\r\n", p->sensor_retries);
    app_printk("$PITCH_SURFACE_S,%u\r\n", p->start_pitch_s);
    app_printk("$PITCH_DIVE_S,%u\r\n", p->dive_pitch_s);
    app_printk("$PITCH_CLIMB_S,%u\r\n", p->climb_pitch_s);
    app_printk("$PITCH_STROKE_S,%u\r\n", p->pitch_full_stroke_s);
    app_printk("$VBD_SURFACE_S,%u\r\n", p->start_pump_s);
    app_printk("$VBD_DIVE_S,%u\r\n", p->dive_pump_s);
    app_printk("$VBD_CLIMB_S,%u\r\n", p->climb_pump_s);
    app_printk("$VBD_STROKE_S,%u\r\n", p->pump_full_stroke_s);
    app_printk("$ROLL_NEUTRAL_S,%u\r\n", p->start_roll_s);
    app_printk("$ROLL_MAX_S,%u\r\n", p->max_roll_s);
    app_printk("$LEAK_THRESHOLD_MV,%u\r\n", p->leak_threshold_mv);

    /* Column names for the $D rows. Control columns are SECONDS of motor
     * travel, not cm/cc — see the header comment in dive_log.h. */
    app_printk("$COLUMNS,elaps_t,depth,head,pitchAng,rollAng,"
               "pitchCtl_s,rollCtl_s,vbdCtl_s,GC_phase,intP,waterT\r\n");
}

void dive_log_gps(int which, bool ok, double lat_deg, double lon_deg)
{
    struct app_time_utc tm;
    app_time_get(&tm);

    if (ok) {
        app_printk("$GPS%d,%02u%02u%02u,%02u%02u%02u,%.4f,%.4f\r\n",
                   which,
                   (unsigned)tm.day, (unsigned)tm.month,
                   (unsigned)(tm.year % 100),
                   (unsigned)tm.hour, (unsigned)tm.minute, (unsigned)tm.second,
                   deg_to_ddmm(lat_deg), deg_to_ddmm(lon_deg));
    } else {
        /* No fix: still record the attempt, with empty position fields. */
        app_printk("$GPS%d,%02u%02u%02u,%02u%02u%02u,,\r\n",
                   which,
                   (unsigned)tm.day, (unsigned)tm.month,
                   (unsigned)(tm.year % 100),
                   (unsigned)tm.hour, (unsigned)tm.minute, (unsigned)tm.second);
    }
}

void dive_log_state(uint32_t elaps_s, const char *what, const char *reason)
{
    if (reason) {
        app_printk("$STATE,%u,%s,%s\r\n", elaps_s, what, reason);
    } else {
        app_printk("$STATE,%u,%s\r\n", elaps_s, what);
    }
}

void dive_log_gc(uint32_t elaps_s, const char *actuator,
                 float from_s, float to_s, int dir, uint32_t dur_s)
{
    app_printk("$GC,%u,%s,%.1f,%.1f,%d,%u\r\n",
               elaps_s, actuator, (double)from_s, (double)to_s, dir, dur_s);
}

void dive_log_data(uint32_t elaps_s, double depth_m, float heading_deg,
                   float pitch_deg, float roll_deg,
                   float pitch_ctl_s, float roll_ctl_s, float vbd_ctl_s,
                   enum dive_phase phase, int32_t internal_pa,
                   double water_temp_c, bool temp_ok)
{
    /* Depth in cm, as the Seaglider .eng does. */
    app_printk("$D,%u,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,",
               elaps_s, (int)(depth_m * 100.0),
               (double)heading_deg, (double)pitch_deg, (double)roll_deg,
               (double)pitch_ctl_s, (double)roll_ctl_s, (double)vbd_ctl_s,
               (int)phase, internal_pa);
    if (temp_ok) {
        app_printk("%.2f\r\n", water_temp_c);
    } else {
        app_printk("NaN\r\n");
    }
}

void dive_log_close(uint32_t elaps_s, double depth_m,
                    const struct dive_errors *errs)
{
    app_printk("$ERRORS,%u,%u,%u,%u,%u\r\n",
               errs->ms5837, errs->bmp180, errs->tsys01,
               errs->hmc6343, errs->gps);
    app_printk("$FINISH,%u,%.2f\r\n", elaps_s, depth_m);
}
