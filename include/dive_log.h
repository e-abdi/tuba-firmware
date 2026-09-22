/*
 * dive_log.h — Seaglider-style structured dive records
 *
 * Everything the firmware prints goes to the console *and* to the OpenLog
 * SD recorder, so the OpenLog capture is the glider's raw dive record. The
 * trouble with the old capture is that it was prose: human-readable, but a
 * converter had to guess at it.
 *
 * This module interleaves machine-readable records into the same stream,
 * modelled directly on the Seaglider .log file (see the Seaglider File
 * Formats Manual, ch. 2.1.1). Every structured line starts with '$' in
 * column 0, so a basestation script can pull the dive record out of a
 * capture that also contains menus, banners and [SENS] prose:
 *
 *     grep '^\$' tuba_capture.txt
 *
 * The record set, in the order one dive emits them:
 *
 *   $VERSION,$GLIDER,$MISSION,$DIVE,$START   header, once per dive
 *   $<PARAM>,<value>                         parameter dump (Seaglider style)
 *   $GPS1,...                                fix before the dive
 *   $COLUMNS,...                             names for the $D rows below
 *   $STATE,<elaps>,begin dive                phase transitions
 *   $D,<elaps>,...                           one sampled row per second
 *   $GC,<elaps>,...                          one row per actuator move
 *   $STATE,<elaps>,end dive,<reason>
 *   ...
 *   $GPS2,...                                fix after surfacing
 *   $ERRORS,...                              per-sensor failure counts
 *   $FINISH,<elaps>,<depth_m>                end of dive
 *
 * IMPORTANT — units differ from a real Seaglider. The Seaglider reports
 * actuator positions in cm (pitch), degrees (roll) and cc (VBD), because it
 * has position feedback and a calibration. Tuba dead-reckons in *seconds of
 * motor travel*, so the control columns are named pitchCtl_s / rollCtl_s /
 * vbdCtl_s and carry seconds. Do not feed them to a Seaglider flight model
 * as-is.
 */
#ifndef DIVE_LOG_H
#define DIVE_LOG_H

#include <stdint.h>
#include <stdbool.h>

struct app_params;

/* GC_phase values used in the $D rows (Tuba's own encoding — the Seaglider
 * numbering covers motor sub-phases Tuba does not have). */
enum dive_phase {
    PHASE_SURFACE = 0,
    PHASE_DIVE    = 1,
    PHASE_APOGEE  = 2,   /* dive -> climb actuator swing */
    PHASE_CLIMB   = 3,
    PHASE_RECOVERY = 4,
};

/* Per-dive sensor failure tallies, reported in $ERRORS (Seaglider's
 * $ERRORS line is the same idea: a count per subsystem, so a pilot can see
 * a sensor degrading over a mission before it fails outright). */
struct dive_errors {
    uint16_t ms5837;    /* external pressure (depth) — critical */
    uint16_t bmp180;    /* internal/hull pressure */
    uint16_t tsys01;    /* water temperature */
    uint16_t hmc6343;   /* compass */
    uint16_t gps;       /* fixes not acquired within the timeout */
};

/* Start a new dive record. dive_num is 1-based within this mission.
 * Dumps the full parameter set, exactly like a Seaglider .log, so the
 * capture is self-describing and a converter needs no side-channel. */
void dive_log_header(uint32_t mission_num, uint32_t dive_num,
                     const struct app_params *p, bool simulated);

/* $GPS1 (which=1, before the dive) / $GPS2 (which=2, after surfacing).
 * Position is in decimal degrees; ok=false emits an empty-position record
 * so the converter still sees that a fix was attempted and missed. */
void dive_log_gps(int which, bool ok, double lat_deg, double lon_deg);

/* $STATE,<elaps>,<what>[,<reason>] — phase transitions.
 * `what` is e.g. "begin dive" / "end dive"; `reason` may be NULL. */
void dive_log_state(uint32_t elaps_s, const char *what, const char *reason);

/* $GC,<elaps>,<actuator>,<from_s>,<to_s>,<dir>,<dur_s> — one actuator move.
 * The Seaglider equivalent carries currents and retry counts; Tuba has no
 * current sensing, so the row stops at the commanded motion. */
void dive_log_gc(uint32_t elaps_s, const char *actuator,
                 float from_s, float to_s, int dir, uint32_t dur_s);

/* One sampled data row. temp_ok=false writes NaN for the temperature,
 * matching the Seaglider .asc convention that a missing sample is marked
 * rather than silently zero-filled. */
void dive_log_data(uint32_t elaps_s, double depth_m, float heading_deg,
                   float pitch_deg, float roll_deg,
                   float pitch_ctl_s, float roll_ctl_s, float vbd_ctl_s,
                   enum dive_phase phase, int32_t internal_pa,
                   double water_temp_c, bool temp_ok);

/* $ERRORS + $FINISH — closes the dive record. */
void dive_log_close(uint32_t elaps_s, double depth_m,
                    const struct dive_errors *errs);

#endif /* DIVE_LOG_H */
