/*
 * app_time.h — System wall-clock time (UTC)
 *
 * The glider has no battery-backed RTC, so wall-clock time is kept in
 * software on top of the Zephyr uptime counter (k_uptime_get()):
 *
 *   wall_time = time_base + (uptime - uptime_at_base)
 *
 * Time sources, in increasing order of trust:
 *   1. Firmware build date (version.h BUILD_DATE) — applied at boot so
 *      timestamps are at least "roughly right" from the first second.
 *   2. GPS — every valid RMC sentence carries UTC time and date; the GPS
 *      driver calls app_time_set_from_gps() on each fix, which re-anchors
 *      the clock to true UTC.
 *
 * All timestamps produced by this module are UTC.
 */
#ifndef APP_TIME_H
#define APP_TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Where the current wall-clock value came from. */
typedef enum {
    APP_TIME_SRC_NONE = 0,   /* not initialized (should not happen after boot) */
    APP_TIME_SRC_BUILD,      /* firmware build date — approximate */
    APP_TIME_SRC_GPS,        /* synced from a GPS fix — true UTC */
} app_time_source_t;

/* Initialize the clock from the firmware build date (version.h BUILD_DATE).
 * Called once at boot, before anything prints timestamps. */
void app_time_init(void);

/* Re-anchor the clock from a GPS RMC sentence (all fields UTC).
 * year is two-digit NMEA year (e.g. 26 -> 2026). Returns 0 on success,
 * -EINVAL if the fields do not form a valid date/time. */
int app_time_set_from_gps(int hour, int min, int sec,
                          int day, int month, int year2);

/* Current Unix time (seconds since 1970-01-01 UTC). */
int64_t app_time_now(void);

/* Format the current time as "YYYY-MM-DD hh:mm:ss" into buf.
 * buf must be at least APP_TIME_STR_LEN bytes. Returns buf. */
#define APP_TIME_STR_LEN 20
const char *app_time_str(char *buf, size_t buflen);

/* Which source last set the clock (for status displays). */
app_time_source_t app_time_source(void);

/* Short label for the current source: "none" / "build" / "GPS". */
const char *app_time_source_str(void);

#endif /* APP_TIME_H */
