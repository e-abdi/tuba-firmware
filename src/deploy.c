/*
 * deploy.c — Mission execution: dive/climb cycles (real and simulated)
 *
 * A mission is a loop of dive/climb cycles:
 *
 *   surface position -> dive targets -> sink until target depth (or
 *   timeout) -> climb targets -> rise until near surface -> surface
 *   position -> GPS fix -> wait for operator -> repeat
 *
 * DEPLOY and SIMULATE run the *same* cycle code; the only difference is
 * where depth comes from:
 *   - deploy:   MS5837 external pressure -> depth below the surface
 *               reference captured at mission start
 *   - simulate: a synthetic profile (sink at 0.5 m/s, rise at 0.5 m/s)
 *               so the full sequence can be tested on the bench
 *
 * ---------------------------------------------------------------------
 * SAFETY ENVELOPE (modelled on the Seaglider)
 *
 * A Seaglider never trusts a dive to end on its own. Every cycle runs
 * inside a set of bounds, and breaching any of them abandons the dive and
 * parks the vehicle on the surface to be recovered. Tuba does the same,
 * with these guards (all parameters, all editable from the menu):
 *
 *   $T_MISSION (t_mission_min)
 *       Hard limit on one surface-to-surface cycle. Two checkpoints:
 *         - at T_MISSION/2 the glider must already be climbing. If it is
 *           still descending, something is wrong (stuck pump, bad trim,
 *           snagged) and waiting the rest of the dive out only makes the
 *           recovery harder. Abort now.
 *         - at T_MISSION, abort unconditionally, in either phase.
 *       This is the Seaglider $T_MISSION escape path.
 *
 *   $D_ABORT (max_depth_m)
 *       Depth ceiling. Breaching it aborts. 0 disables the check.
 *
 *   Sensor retries (sensor_retries)
 *       Every sensor read is retried up to N times before it counts as a
 *       failure. A failed *critical* sensor — the MS5837, which is the
 *       only source of depth — aborts: a glider that cannot measure depth
 *       cannot fly a profile. Failed non-critical sensors (hull pressure,
 *       water temperature, compass) are logged and tallied in $ERRORS, and
 *       the dive continues with that column marked missing.
 *
 *   Leak (leak_threshold_mv)
 *       The leak monitor already alarms on the console; during a mission
 *       it also aborts the dive. 0 disables the check.
 *
 * Every abort ends in the RECOVERY POSITION: pump driven fully OUT (maximum
 * buoyancy) and the pitch mass driven fully FORWARD, so the glider floats
 * nose-down / antenna-up like a Seaglider after $QUIT. Both moves are blind
 * full strokes against the end stops rather than dead-reckoned moves —
 * an abort is exactly the situation where the believed position may be
 * wrong, so the recovery must not depend on it.
 *
 * After the maneuver the worker holds station on the surface, reporting and
 * taking a GPS fix every $T_RSLEEP minutes until the operator presses
 * ENTER; the UI then lands in the RECOVERY state (see ui_menu.c), where
 * "resume" starts a new mission.
 * ---------------------------------------------------------------------
 *
 * Every monitoring second a [SENS] line is printed with a UTC timestamp,
 * internal (hull) pressure, depth, water temperature (TSYS01) and compass
 * attitude, alongside a machine-readable $D row (see dive_log.h) that the
 * basestation converter turns into .log/.eng files. Heading is held by
 * banking the roll mass — see update_roll_for_heading().
 *
 * Both missions run on a dedicated worker thread (deploy_start_async /
 * simulate_start_async) so the console/UI stays responsive; the UI polls
 * deploy_is_running()/simulate_is_running() to know when to show the menu
 * again.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <stdbool.h>
#include <math.h>

#include "app_params.h"
#include "app_print.h"
#include "app_time.h"
#include "dive_log.h"
#include "hw_ms5837.h"
#include "hw_bmp180.h"
#include "hw_hmc6343.h"
#include "hw_tsys01.h"
#include "hw_leak.h"
#include "hw_motors.h"
#include "hw_pump.h"
#include "hw_gps.h"
#include "deploy.h"

/* Physical constants for pressure -> depth conversion. */
#define SEA_WATER_DENSITY_KG_M3 1025.0
#define GRAVITY_M_S2 9.80665

/* Heading control: check every N seconds, correct outside +/- tolerance. */
#define HEADING_CHECK_INTERVAL_SEC 10
#define HEADING_TOLERANCE_DEG 5.0

/* Simulated vertical speeds (bench testing). */
#define SIM_SINK_M_S  0.5
#define SIM_RISE_M_S  0.5

/* Surface detection threshold during climb. */
#define SURFACE_DEPTH_M 1.0

/* Gap between retries of a failed sensor read. Kept short: with the
 * default 10 retries a dead sensor costs at most half a second per
 * sample, which slows the 1 Hz loop without stalling it. */
#define SENSOR_RETRY_GAP_MS 50

/* GPS fix timeout used at the surface, in seconds. */
#define GPS_FIX_TIMEOUT_SEC 30

/* Backstop for t_mission_min == 0 ("no limit"). The dive and climb loops
 * are bounded by T_MISSION; with it disabled, a climb that never reaches
 * the surface would spin forever, which is not a state any glider should
 * be able to reach. This ceiling only ends the cycle — it does not enable
 * the half-time "should be climbing by now" checkpoint, which stays off as
 * the operator asked. (The old code bounded the climb at 60 iterations,
 * i.e. 60 seconds: short enough to cut a real profile short.) */
#define T_MISSION_FALLBACK_MIN 60

/* =====================================================================
 * Abort causes
 *
 * The names double as the reason field of the $STATE record, so they use
 * the Seaglider's SHOUTING_SNAKE_CASE convention and are parsed by the
 * basestation converter.
 * ===================================================================== */
enum dive_abort {
    ABORT_NONE = 0,
    ABORT_T_MISSION,          /* whole-cycle hard timeout */
    ABORT_NO_CLIMB_BY_HALF,   /* still descending at T_MISSION/2 */
    ABORT_DEPTH_SENSOR,       /* MS5837 unreadable — cannot fly blind */
    ABORT_DEPTH_EXCEEDED,     /* deeper than max_depth_m */
    ABORT_LEAK,               /* leak detector below threshold */
};

static const char *abort_reason_str(enum dive_abort a)
{
    switch (a) {
    case ABORT_T_MISSION:        return "T_MISSION_EXCEEDED";
    case ABORT_NO_CLIMB_BY_HALF: return "NOT_CLIMBING_AT_HALF_T_MISSION";
    case ABORT_DEPTH_SENSOR:     return "DEPTH_SENSOR_FAILED";
    case ABORT_DEPTH_EXCEEDED:   return "DEPTH_LIMIT_EXCEEDED";
    case ABORT_LEAK:             return "LEAK_DETECTED";
    default:                     return "NONE";
    }
}

/* Human-readable one-liner for the console and the RECOVERY menu. */
static const char *abort_text(enum dive_abort a)
{
    switch (a) {
    case ABORT_T_MISSION:
        return "dive exceeded T_MISSION";
    case ABORT_NO_CLIMB_BY_HALF:
        return "still descending at half of T_MISSION";
    case ABORT_DEPTH_SENSOR:
        return "external pressure sensor (MS5837) unreadable";
    case ABORT_DEPTH_EXCEEDED:
        return "depth limit exceeded";
    case ABORT_LEAK:
        return "leak detected";
    default:
        return "none";
    }
}

/* Reason the last mission ended, for the UI. NULL = ended normally. */
static const char *g_abort_text = NULL;

const char *deploy_abort_reason(void) { return g_abort_text; }
bool deploy_aborted(void)             { return g_abort_text != NULL; }

/* =====================================================================
 * Last GPS fix
 *
 * $GPS1 has to appear inside the dive record, after $VERSION, or a
 * converter reading the capture has no dive to attach it to. But the fix
 * itself is taken at the surface, before the record starts — so it is held
 * here and emitted by dive_cycle(). Seaglider does the same thing: a
 * dive's GPS1 is the fix taken when it surfaced from the previous dive.
 * ===================================================================== */
static bool   g_fix_ok;
static double g_fix_lat, g_fix_lon;

/* Take a surface fix (or fake one in simulation) and remember it.
 * Bumps errs->gps when a real fix times out. */
static void take_gps_fix(bool simulated, const char *tag, struct dive_errors *errs)
{
    if (simulated) {
        app_printk("%s acquiring simulated GPS fix\r\n", tag);
        k_sleep(K_SECONDS(2));
        app_printk("[GPS] acquired (simulated)\r\n");
        g_fix_ok = false;       /* no position to report in simulation */
        g_fix_lat = g_fix_lon = 0.0;
        return;
    }

    app_printk("%s acquiring GPS fix\r\n", tag);
    double lat = 0.0, lon = 0.0;
    g_fix_ok = gps_fix_wait_pos(GPS_FIX_TIMEOUT_SEC, &lat, &lon);
    if (g_fix_ok) {
        g_fix_lat = lat;
        g_fix_lon = lon;
    } else {
        errs->gps++;
    }
}

/* =====================================================================
 * Cycle clock
 *
 * Elapsed seconds since "begin dive" — the time base for every $STATE,
 * $GC and $D record, and for the T_MISSION checks.
 * ===================================================================== */
static int64_t g_cycle_start_ms;

static void cycle_clock_start(void) { g_cycle_start_ms = k_uptime_get(); }

static uint32_t cycle_elapsed_s(void)
{
    int64_t d = k_uptime_get() - g_cycle_start_ms;
    return (d < 0) ? 0u : (uint32_t)(d / 1000);
}

/* =====================================================================
 * Small helpers
 * ===================================================================== */

/* Shortest angular distance between two headings (degrees).
 * Positive = starboard (right) turn needed; range -180..+180. */
static float heading_delta(float current_deg, float desired_deg)
{
    float delta = desired_deg - current_deg;
    while (delta > 180.0f) delta -= 360.0f;
    while (delta < -180.0f) delta += 360.0f;
    return delta;
}

/* Move a motor to an absolute position (in seconds of travel).
 * The 0.5 s deadband avoids chattering the H-bridge on tiny errors.
 * Returns true if a move was commanded. */
static bool motor_move_to(enum motor_id id, float target_s, const char *why)
{
    float cur = motor_get_position_sec(id);
    float delta = target_s - cur;
    if (fabsf(delta) <= 0.5f) {
        return false;   /* close enough */
    }
    int dir = (delta > 0) ? +1 : -1;
    uint32_t duration = (uint32_t)(fabsf(delta) + 0.5f);
    const char *name = (id == MOTOR_ROLL) ? "roll" : "pitch";
    app_printk("[MOVE] %s %s: %.1fs -> %.1fs (%us)\r\n",
               name, why, cur, target_s, duration);
    dive_log_gc(cycle_elapsed_s(), name, cur, target_s, dir, duration);
    motor_run(id, dir, duration);
    return true;
}

/* Same as motor_move_to but for the buoyancy pump. */
static bool pump_move_to(float target_s, const char *why)
{
    float cur = pump_get_position_sec();
    float delta = target_s - cur;
    if (fabsf(delta) <= 0.5f) {
        return false;
    }
    int dir = (delta > 0) ? +1 : -1;
    uint32_t duration = (uint32_t)(fabsf(delta) + 0.5f);
    app_printk("[MOVE] pump %s: %.1fs -> %.1fs (%us)\r\n", why, cur, target_s, duration);
    dive_log_gc(cycle_elapsed_s(), "vbd", cur, target_s, dir, duration);
    pump_run(dir, duration);
    return true;
}

/* =====================================================================
 * Heading hold
 *
 * The glider steers by banking: during a dive, banking toward one side
 * turns it the other way (and vice versa during a climb), so the roll
 * direction must flip between phases.
 * ===================================================================== */

/* Roll direction (+1/-1/0) for a wanted heading change in this phase. */
static int roll_direction_for_phase(bool dive_phase, float hdg_delta)
{
    if (hdg_delta > HEADING_TOLERANCE_DEG) {
        /* Need to turn starboard (right). */
        return dive_phase ? -1 : +1;
    } else if (hdg_delta < -HEADING_TOLERANCE_DEG) {
        /* Need to turn port (left). */
        return dive_phase ? +1 : -1;
    }
    return 0;   /* within tolerance, no correction */
}

/* Adjust the roll mass to steer toward the desired heading.
 * Within tolerance: return roll to neutral. Outside: bank to max_roll_s
 * in the phase-appropriate direction. Returns true if roll was moved. */
static bool roll_no_compass_warned;

static bool update_roll_for_heading(bool dive_phase, float current_heading,
                                    bool heading_valid, float desired_heading,
                                    struct app_params *p)
{
    /* No compass, no steering. The heading field is zero when the read
     * failed, so banking on it would "correct" a fabricated error and fly
     * the glider in a circle. Centre the roll mass and hold course until
     * the compass comes back. */
    if (!heading_valid) {
        if (!roll_no_compass_warned) {
            app_printk("[ROLL] compass read failed - steering disabled, roll centred\r\n");
            roll_no_compass_warned = true;
        }
        return motor_move_to(MOTOR_ROLL, (float)p->start_roll_s,
                             "to neutral (no compass)");
    }
    roll_no_compass_warned = false;

    float hdg_delta = heading_delta(current_heading, desired_heading);

    if (hdg_delta <= HEADING_TOLERANCE_DEG && hdg_delta >= -HEADING_TOLERANCE_DEG) {
        /* On course — make sure we are not still banked over. */
        return motor_move_to(MOTOR_ROLL, (float)p->start_roll_s, "to neutral (on course)");
    }

    int roll_dir = roll_direction_for_phase(dive_phase, hdg_delta);
    float target_roll = (roll_dir > 0) ? (float)p->max_roll_s : -(float)p->max_roll_s;
    app_printk("[ROLL] heading=%.1f desired=%.1f (delta=%.1f) during %s\r\n",
               (double)current_heading, (double)desired_heading, (double)hdg_delta,
               dive_phase ? "dive" : "climb");
    return motor_move_to(MOTOR_ROLL, target_roll, "heading correction");
}

/* =====================================================================
 * Sensor reads with retry
 *
 * Uniform adapters so one retry loop serves every sensor. Each read is
 * attempted up to sensor_retries times; every failed attempt bumps the
 * per-dive tally that ends up in the $ERRORS record.
 * ===================================================================== */

typedef int (*sensor_read_fn)(void *out);

struct ms5837_out { double temp_c; double press_kpa; };
struct hmc_out    { float heading; float pitch; float roll; };

static int rd_ms5837(void *o)
{
    struct ms5837_out *v = o;
    return ms5837_read(&v->temp_c, &v->press_kpa);
}
static int rd_bmp180(void *o)  { return bmp180_read_pa((int32_t *)o); }
static int rd_tsys01(void *o)  { return tsys01_read((double *)o); }
static int rd_hmc6343(void *o)
{
    struct hmc_out *v = o;
    return hmc6343_read(&v->heading, &v->pitch, &v->roll);
}

/* Try a sensor read up to sensor_retries times. Returns true on success. */
static bool sensor_read_retry(const char *name, sensor_read_fn fn, void *out,
                              uint16_t *err_count)
{
    uint16_t tries = app_params_get()->sensor_retries;
    if (tries == 0) {
        tries = 1;      /* a zero retry budget would never read anything */
    }

    for (uint16_t i = 0; i < tries; i++) {
        if (fn(out) == 0) {
            if (i > 0) {
                app_printk("[SENS] %s recovered after %u attempt%s\r\n",
                           name, (unsigned)(i + 1), i == 0 ? "" : "s");
            }
            return true;
        }
        (*err_count)++;
        k_sleep(K_MSEC(SENSOR_RETRY_GAP_MS));
    }
    app_printk("[ERR] %s: no valid read in %u attempts\r\n", name, (unsigned)tries);
    return false;
}

/* =====================================================================
 * Sensor sampling + [SENS] telemetry line
 * ===================================================================== */

/* One snapshot of everything reported during a mission. */
struct sens_sample {
    int32_t internal_pa;     /* hull pressure [Pa] (0 if read failed)    */
    bool    internal_ok;
    double  depth_m;         /* depth below surface reference [m]        */
    double  water_temp_c;    /* TSYS01 water temperature [C]             */
    bool    water_temp_ok;   /* false when the TSYS01 is absent/failed   */
    float   heading, pitch, roll;   /* compass attitude [deg]            */
    bool    attitude_ok;
};

/* Read all non-critical mission sensors. Depth comes from the caller (it
 * depends on real-vs-simulated mode); everything else is sampled here.
 * None of these failing aborts the dive — they are tallied and reported. */
static void sens_read(struct sens_sample *s, double depth_m,
                      struct dive_errors *errs)
{
    s->depth_m = depth_m < 0.0 ? 0.0 : depth_m;

    s->internal_pa = 0;
    s->internal_ok = sensor_read_retry("BMP180 (hull pressure)", rd_bmp180,
                                       &s->internal_pa, &errs->bmp180);

    s->water_temp_c = 0.0;
    s->water_temp_ok = sensor_read_retry("TSYS01 (water temp)", rd_tsys01,
                                         &s->water_temp_c, &errs->tsys01);

    struct hmc_out att = {0};
    s->attitude_ok = sensor_read_retry("HMC6343 (compass)", rd_hmc6343,
                                       &att, &errs->hmc6343);
    s->heading = att.heading;
    s->pitch = att.pitch;
    s->roll = att.roll;
}

/* Print the telemetry line, e.g.:
 * [SENS] 2026-07-12 14:03:22Z IntP=99321 Pa, Depth=3.20m, WaterT=17.85C, H=181.0,R=0.5,P=-32.0 */
static void sens_print(const struct sens_sample *s, bool simulated)
{
    char ts[APP_TIME_STR_LEN];
    app_time_str(ts, sizeof(ts));

    if (s->water_temp_ok) {
        app_printk("[SENS] %sZ IntP=%d Pa, %s=%.2fm, WaterT=%.2fC, H=%.1f,R=%.1f,P=%.1f\r\n",
                   ts, s->internal_pa, simulated ? "SimDepth" : "Depth", s->depth_m,
                   s->water_temp_c, (double)s->heading, (double)s->roll, (double)s->pitch);
    } else {
        app_printk("[SENS] %sZ IntP=%d Pa, %s=%.2fm, WaterT=--, H=%.1f,R=%.1f,P=%.1f\r\n",
                   ts, s->internal_pa, simulated ? "SimDepth" : "Depth", s->depth_m,
                   (double)s->heading, (double)s->roll, (double)s->pitch);
    }
}

/* Emit both the human line and the machine-readable $D row. */
static void sens_report(const struct sens_sample *s, bool simulated,
                        enum dive_phase phase)
{
    sens_print(s, simulated);
    dive_log_data(cycle_elapsed_s(), s->depth_m, s->heading, s->pitch, s->roll,
                  (float)motor_get_position_sec(MOTOR_PITCH),
                  (float)motor_get_position_sec(MOTOR_ROLL),
                  (float)pump_get_position_sec(),
                  phase, s->internal_pa, s->water_temp_c, s->water_temp_ok);
}

/* =====================================================================
 * Depth source (the one critical sensor)
 * ===================================================================== */

/* Real depth from the external pressure sensor relative to the surface
 * reference. Returns false when the sensor could not be read at all after
 * the full retry budget — the caller must abort. */
static bool real_depth_m(double surface_pa, struct dive_errors *errs,
                         double *out_depth)
{
    struct ms5837_out v = {0};
    if (!sensor_read_retry("MS5837 (depth)", rd_ms5837, &v, &errs->ms5837)) {
        return false;
    }
    double external_pa = v.press_kpa * 1000.0;
    double depth = (surface_pa > 0.0)
                 ? (external_pa - surface_pa) / (SEA_WATER_DENSITY_KG_M3 * GRAVITY_M_S2)
                 : 0.0;
    *out_depth = depth < 0.0 ? 0.0 : depth;
    return true;
}

/* =====================================================================
 * Operator interaction between cycles
 * ===================================================================== */

/* Stop request from the UI.
 *
 * IMPORTANT: the worker must NOT poll the consoles itself — the main UI
 * loop reads them every ~10 ms and would win every race for the byte
 * (this is exactly the bug where ENTER "did not register" during the
 * 10-second window). Instead the main loop turns a bare ENTER into
 * EVT_ENTER, the DEPLOYED/SIMULATE event handlers call
 * deploy_notify_enter(), and the worker just watches this flag. */
static atomic_t stop_requested = ATOMIC_INIT(0);

void deploy_notify_enter(void)
{
    atomic_set(&stop_requested, 1);
}

/* Wait up to timeout_s for the operator's stop request (ENTER on either
 * console, forwarded by the UI). Returns true if a stop was requested —
 * including one made earlier, mid-cycle. */
static bool enter_pressed_within(int timeout_s)
{
    int64_t start = k_uptime_get();
    do {
        if (atomic_get(&stop_requested)) {
            return true;
        }
        k_sleep(K_MSEC(100));
    } while (k_uptime_get() - start < (int64_t)timeout_s * 1000);
    return atomic_get(&stop_requested) != 0;
}

/* Check if the external pressure sensor is available (menu pre-check). */
bool deploy_check_sensor_available(void)
{
    double temp_c = 0.0, press_kpa = 0.0;
    return (ms5837_read(&temp_c, &press_kpa) == 0);
}

/* =====================================================================
 * Safety checks shared by the dive and climb loops
 * ===================================================================== */

/* Leak check. Returns true when the hull is wet. 0 threshold = disabled.
 * A read failure is NOT treated as a leak: the ADC is far less trustworthy
 * than the consequence of a false abort. */
static bool leak_tripped(const struct app_params *p)
{
    if (p->leak_threshold_mv == 0) {
        return false;
    }
    int32_t mv;
    if (leak_detector_read_mv(&mv) != 0) {
        return false;
    }
    return mv < (int32_t)p->leak_threshold_mv;
}

/* The guards that apply in every profile phase. Returns the abort cause,
 * or ABORT_NONE to keep flying. `climbing` selects the half-T_MISSION
 * checkpoint: it only fires while the glider is still on its way down. */
static enum dive_abort check_envelope(const struct app_params *p, double depth_m,
                                      bool climbing)
{
    uint32_t elapsed = cycle_elapsed_s();

    if (p->t_mission_min > 0) {
        uint32_t t_mission_s = (uint32_t)p->t_mission_min * 60u;
        if (elapsed >= t_mission_s) {
            return ABORT_T_MISSION;
        }
        if (!climbing && elapsed >= t_mission_s / 2u) {
            return ABORT_NO_CLIMB_BY_HALF;
        }
    } else if (elapsed >= (uint32_t)T_MISSION_FALLBACK_MIN * 60u) {
        return ABORT_T_MISSION;
    }

    if (p->max_depth_m > 0 && depth_m > (double)p->max_depth_m) {
        return ABORT_DEPTH_EXCEEDED;
    }

    if (leak_tripped(p)) {
        return ABORT_LEAK;
    }

    return ABORT_NONE;
}

/* =====================================================================
 * Recovery
 *
 * "Recovery position" = pump fully OUT (maximum buoyancy) and the pitch
 * mass fully FORWARD. Both are driven as blind full strokes against their
 * end stops: in an abort the dead-reckoned position is precisely the thing
 * that may be wrong, so the maneuver must not depend on it.
 *
 * Buoyancy goes first. Getting the glider heading for the surface matters
 * more than its attitude once there, and the two H-bridges draw a lot of
 * current if run together.
 * ===================================================================== */
static void recovery_position(struct app_params *p, const char *tag)
{
    app_printk("%s >>> RECOVERY POSITION: pump fully OUT, pitch fully FORWARD\r\n", tag);
    dive_log_state(cycle_elapsed_s(), "begin recovery", NULL);

    /* 1. Pump fully out — maximum buoyancy. */
    uint32_t pump_s = p->pump_full_stroke_s;
    if (pump_s > 0) {
        float from = (float)pump_get_position_sec();
        app_printk("%s recovery: pump OUT for %us (full stroke)\r\n", tag, pump_s);
        dive_log_gc(cycle_elapsed_s(), "vbd", from, (float)pump_s, +1, pump_s);
        pump_run(+1, pump_s);
        /* +1 s so the driver's delayed stop has fired before we move on. */
        k_sleep(K_SECONDS(pump_s + 1));
        /* At the end stop the position is known, whatever we believed. */
        pump_set_position_sec((int32_t)pump_s);
    } else {
        app_printk("%s recovery: WARNING pump_full_stroke_s is 0 — pump not moved\r\n", tag);
    }

    /* 2. Pitch mass fully forward. The pitch limit switch stops the motor
     *    early if it reaches the stop before the stroke time elapses. */
    uint32_t pitch_s = p->pitch_full_stroke_s;
    if (pitch_s > 0) {
        float from = (float)motor_get_position_sec(MOTOR_PITCH);
        app_printk("%s recovery: pitch FORWARD for %us (full stroke)\r\n", tag, pitch_s);
        dive_log_gc(cycle_elapsed_s(), "pitch", from, (float)pitch_s, +1, pitch_s);
        motor_run(MOTOR_PITCH, +1, pitch_s);
        k_sleep(K_SECONDS(pitch_s + 1));
        motor_set_position_sec(MOTOR_PITCH, (int32_t)pitch_s);
    } else {
        app_printk("%s recovery: WARNING pitch_full_stroke_s is 0 — pitch not moved\r\n", tag);
    }

    /* 3. Roll back to neutral so the glider floats level. */
    motor_move_to(MOTOR_ROLL, (float)p->start_roll_s, "to neutral (recovery)");

    dive_log_state(cycle_elapsed_s(), "end recovery", "RECOVERY_POSITION_SET");
    app_printk("%s recovery position set: pitch=%lds, roll=%lds, pump=%lds\r\n", tag,
               (long)motor_get_position_sec(MOTOR_PITCH),
               (long)motor_get_position_sec(MOTOR_ROLL),
               (long)pump_get_position_sec());
}

/* Hold on the surface after a recovery: report position and state every
 * $T_RSLEEP minutes until the operator presses ENTER. This is the
 * Seaglider recovery loop (GPS, call home, sleep, repeat) minus the
 * satellite call Tuba does not have. */
static void recovery_hold(struct app_params *p, const char *tag, bool simulated,
                          struct dive_errors *errs)
{
    uint32_t sleep_s = (uint32_t)p->t_rsleep_min * 60u;
    if (sleep_s == 0) {
        sleep_s = 60;
    }

    app_printk("%s holding in recovery position; status every %u min.\r\n",
               tag, (unsigned)(sleep_s / 60));
    app_printk("%s press ENTER to end the mission and return to the menu.\r\n", tag);

    while (!atomic_get(&stop_requested)) {
        char ts[APP_TIME_STR_LEN];
        app_time_str(ts, sizeof(ts));
        app_printk("[RECOVERY] %sZ holding — pitch=%lds, roll=%lds, pump=%lds\r\n",
                   ts,
                   (long)motor_get_position_sec(MOTOR_PITCH),
                   (long)motor_get_position_sec(MOTOR_ROLL),
                   (long)pump_get_position_sec());

        if (!simulated) {
            take_gps_fix(false, tag, errs);
            dive_log_gps(2, g_fix_ok, g_fix_lat, g_fix_lon);
        }

        /* Sleep in 1 s slices so ENTER is noticed promptly. */
        for (uint32_t i = 0; i < sleep_s && !atomic_get(&stop_requested); i++) {
            k_sleep(K_SECONDS(1));
        }
    }
}

/* =====================================================================
 * One dive/climb cycle (shared by deploy and simulate)
 *
 * Returns ABORT_NONE for a cycle that completed normally, or the cause
 * that ended it early. On an abort the caller runs the recovery maneuver.
 * ===================================================================== */
static enum dive_abort dive_cycle(struct app_params *p, double surface_pa,
                                  bool simulated, uint32_t mission_num,
                                  uint32_t dive_num, struct dive_errors *errs,
                                  double *out_final_depth)
{
    const char *tag = simulated ? "[SIMULATE]" : "[DEPLOY]";
    int heading_check_counter = 0;
    enum dive_abort cause = ABORT_NONE;
    double depth = 0.0;

    /* --- 1. Surface trim: pitch and pump to their surface positions --- */
    cycle_clock_start();
    dive_log_header(mission_num, dive_num, p, simulated);
    /* The fix taken at the surface before this dive started. */
    dive_log_gps(1, g_fix_ok, g_fix_lat, g_fix_lon);

    app_printk("%s moving to surface position (pitch=%us, pump=%us)\r\n",
               tag, p->start_pitch_s, p->start_pump_s);
    dive_log_state(cycle_elapsed_s(), "begin surface", NULL);
    motor_move_to(MOTOR_PITCH, (float)p->start_pitch_s, "to surface position");
    pump_move_to((float)p->start_pump_s, "to surface position");
    dive_log_state(cycle_elapsed_s(), "end surface", "CONTROL_FINISHED_OK");

    /* --- 2. Dive trim: nose down, ballast in --- */
    app_printk("%s moving to dive targets (pitch=%us, pump=%us)\r\n",
               tag, p->dive_pitch_s, p->dive_pump_s);
    motor_move_to(MOTOR_PITCH, (float)p->dive_pitch_s, "to dive target");
    pump_move_to((float)p->dive_pump_s, "to dive target");

    /* --- 3. Sink: monitor once per second until target depth/timeout ---
     *
     * The cycle clock is restarted here so T_MISSION measures the profile
     * itself, not the trim moves that precede it. */
    app_printk("%s diving to %.1fm%s\r\n", tag, (double)p->dive_depth_m,
               simulated ? " (simulated at 0.5 m/s)" : "");
    cycle_clock_start();
    dive_log_state(0, "begin dive", NULL);

    uint64_t dive_start_ms = (uint64_t)k_uptime_get();
    uint64_t deadline_ms = dive_start_ms + (uint64_t)p->dive_timeout_min * 60ULL * 1000ULL;
    const char *dive_end_reason = NULL;

    while (1) {
        if (simulated) {
            double elapsed_s = (double)((uint64_t)k_uptime_get() - dive_start_ms) / 1000.0;
            depth = SIM_SINK_M_S * elapsed_s;
        } else if (!real_depth_m(surface_pa, errs, &depth)) {
            /* Critical sensor: a glider that cannot measure depth cannot
             * fly a profile, and must not keep descending on hope. */
            cause = ABORT_DEPTH_SENSOR;
            break;
        }

        struct sens_sample s;
        sens_read(&s, depth, errs);
        sens_report(&s, simulated, PHASE_DIVE);

        /* Heading hold: correct every HEADING_CHECK_INTERVAL_SEC seconds. */
        if (++heading_check_counter >= HEADING_CHECK_INTERVAL_SEC) {
            heading_check_counter = 0;
            update_roll_for_heading(true, s.heading, s.attitude_ok,
                                    (float)p->desired_heading_deg, p);
        }

        /* Safety envelope. Checked before the normal end conditions so a
         * breach always wins over "close enough to target depth". */
        cause = check_envelope(p, depth, false /* still descending */);
        if (cause != ABORT_NONE) {
            break;
        }

        if (depth >= (double)p->dive_depth_m) {
            app_printk("%s target depth reached (%.2fm) -> start climb\r\n", tag, depth);
            dive_end_reason = "TARGET_DEPTH_REACHED";
            break;
        }
        if (!simulated && (uint64_t)k_uptime_get() >= deadline_ms) {
            app_printk("%s dive timeout -> start climb\r\n", tag);
            dive_end_reason = "DIVE_TIMEOUT";
            break;
        }

        k_sleep(K_SECONDS(1));
    }

    *out_final_depth = depth;
    if (cause != ABORT_NONE) {
        dive_log_state(cycle_elapsed_s(), "end dive", abort_reason_str(cause));
        return cause;
    }
    dive_log_state(cycle_elapsed_s(), "end dive", dive_end_reason);

    /* --- 4. Apogee: swing the actuators from dive to climb trim --- */
    dive_log_state(cycle_elapsed_s(), "begin apogee", NULL);
    app_printk("%s moving to climb targets (pitch=%us, pump=%us)\r\n",
               tag, p->climb_pitch_s, p->climb_pump_s);
    motor_move_to(MOTOR_PITCH, (float)p->climb_pitch_s, "to climb target");
    pump_move_to((float)p->climb_pump_s, "to climb target");
    dive_log_state(cycle_elapsed_s(), "end apogee", "CONTROL_FINISHED_OK");

    /* --- 5. Rise: monitor until near-surface ---
     *
     * The climb has no iteration cap of its own: T_MISSION is the bound.
     * (It used to stop after 60 samples, which is 60 seconds — fine for a
     * 5 m bench dive, far too short for any real profile.) */
    dive_log_state(cycle_elapsed_s(), "begin climb", NULL);
    uint64_t climb_start_ms = (uint64_t)k_uptime_get();
    double climb_start_depth = depth;
    heading_check_counter = 0;

    while (1) {
        if (simulated) {
            double elapsed_s = (double)((uint64_t)k_uptime_get() - climb_start_ms) / 1000.0;
            depth = climb_start_depth - SIM_RISE_M_S * elapsed_s;
            if (depth < 0.0) depth = 0.0;
        } else if (!real_depth_m(surface_pa, errs, &depth)) {
            cause = ABORT_DEPTH_SENSOR;
            break;
        }

        struct sens_sample s;
        sens_read(&s, depth, errs);
        sens_report(&s, simulated, PHASE_CLIMB);

        if (++heading_check_counter >= HEADING_CHECK_INTERVAL_SEC) {
            heading_check_counter = 0;
            update_roll_for_heading(false, s.heading, s.attitude_ok,
                                    (float)p->desired_heading_deg, p);
        }

        cause = check_envelope(p, depth, true /* climbing */);
        if (cause != ABORT_NONE) {
            break;
        }

        if (depth < SURFACE_DEPTH_M) {
            break;
        }

        k_sleep(K_SECONDS(1));
    }

    *out_final_depth = depth;
    if (cause != ABORT_NONE) {
        dive_log_state(cycle_elapsed_s(), "end climb", abort_reason_str(cause));
        return cause;
    }
    dive_log_state(cycle_elapsed_s(), "end climb", "SURFACE_DEPTH_REACHED");

    /* --- 6. Surfaced: return all actuators to surface trim --- */
    app_printk("%s depth < %.0fm; moving to surface position\r\n", tag, SURFACE_DEPTH_M);
    dive_log_state(cycle_elapsed_s(), "begin surface", NULL);
    motor_move_to(MOTOR_PITCH, (float)p->start_pitch_s, "surfacing");
    pump_move_to((float)p->start_pump_s, "surfacing");
    motor_move_to(MOTOR_ROLL, (float)p->start_roll_s, "to neutral (surfaced)");

    /* A few extra readings while settled at the surface. */
    for (int j = 0; j < 5; j++) {
        k_sleep(K_SECONDS(1));
        struct sens_sample s;
        sens_read(&s, 0.0, errs);
        sens_report(&s, simulated, PHASE_SURFACE);
    }

    *out_final_depth = 0.0;
    return ABORT_NONE;
}

/* =====================================================================
 * Mission runners
 * ===================================================================== */

/* Fold one dive's sensor failure counts into the mission totals. */
static void tally_errors(struct dive_errors *total, const struct dive_errors *one)
{
    total->ms5837  += one->ms5837;
    total->bmp180  += one->bmp180;
    total->tsys01  += one->tsys01;
    total->hmc6343 += one->hmc6343;
    total->gps     += one->gps;
}

/* Warn about parameter combinations that make the safety envelope either
 * useless or actively harmful. Called once at mission start — better to
 * see this on the console before the glider is in the water. */
static void check_envelope_config(struct app_params *p, const char *tag)
{
    if (p->t_mission_min == 0) {
        app_printk("%s WARNING: T_MISSION is 0 — no dive time limit, and no "
                   "half-time climb check; the cycle still ends after %d min\r\n",
                   tag, T_MISSION_FALLBACK_MIN);
    } else if (p->t_mission_min < 2u * p->dive_timeout_min) {
        /* Half of T_MISSION would elapse before a healthy dive reaches its
         * own timeout, so every normal dive would abort. */
        app_printk("%s WARNING: T_MISSION (%u min) is less than 2x the dive timeout "
                   "(%u min) — normal dives will abort at half T_MISSION\r\n",
                   tag, p->t_mission_min, p->dive_timeout_min);
    }
    if (p->max_depth_m == 0) {
        app_printk("%s NOTE: depth limit is 0 (off) — set it above the target depth "
                   "to enable the D_ABORT check\r\n", tag);
    } else if ((float)p->max_depth_m <= p->dive_depth_m) {
        app_printk("%s WARNING: depth limit (%u m) is not above the target depth "
                   "(%d m) — every dive will abort\r\n",
                   tag, p->max_depth_m, (int)p->dive_depth_m);
    }
    if (p->pump_full_stroke_s == 0 || p->pitch_full_stroke_s == 0) {
        app_printk("%s WARNING: a full-stroke time is 0 — the recovery maneuver "
                   "cannot move that actuator\r\n", tag);
    }
}

/* Shared mission sequence; simulated selects the depth source and skips
 * real GPS fixes. */
static void mission_run(bool simulated)
{
    struct app_params *p = app_params_get();
    const char *tag = simulated ? "[SIMULATE]" : "[DEPLOY]";
    struct dive_errors errs = {0};
    static uint32_t mission_num;

    /* Forget any stop request left over from a previous mission. */
    atomic_clear(&stop_requested);
    g_abort_text = NULL;
    mission_num++;

    app_printk("%s starting sequence%s\r\n", tag,
               simulated ? " (pressure simulated)" : "");
    check_envelope_config(p, tag);

    /* Surface pressure reference for depth calculation. */
    double surface_pa;
    if (simulated) {
        surface_pa = 101325.0;   /* standard sea level */
        app_printk("%s simulated surface pressure: %.3f kPa\r\n", tag, surface_pa / 1000.0);
    } else {
        struct ms5837_out v = {0};
        if (!sensor_read_retry("MS5837 (depth)", rd_ms5837, &v, &errs.ms5837)) {
            app_printk("%s ERROR: cannot read external pressure sensor (MS5837)\r\n", tag);
            app_printk("%s Try 'simulate' instead to test with simulated pressure\r\n", tag);
            return;
        }
        surface_pa = v.press_kpa * 1000.0;
        app_printk("%s surface external pressure: %.3f kPa (T=%.2f C)\r\n",
                   tag, v.press_kpa, v.temp_c);
    }

    app_printk("%s starting positions: pitch=%.1fs, roll=%.1fs, pump=%.1fs\r\n", tag,
               (double)motor_get_position_sec(MOTOR_PITCH),
               (double)motor_get_position_sec(MOTOR_ROLL),
               (double)pump_get_position_sec());

    /* Surface wait before the first dive (operator safety margin). */
    app_printk("%s waiting %us before first dive\r\n", tag, p->deploy_wait_s);
    for (uint32_t i = 0; i < p->deploy_wait_s; ++i) {
        k_sleep(K_SECONDS(1));
    }

    /* GPS fix before the first dive (also syncs the system clock). It is
     * recorded as that dive's $GPS1 by dive_cycle(), not here. */
    g_fix_ok = false;
    g_fix_lat = g_fix_lon = 0.0;
    take_gps_fix(simulated, tag, &errs);

    /* Main dive/climb loop: repeat until the operator presses ENTER in
     * the 10-second surface window after each cycle, or a safety guard
     * trips and sends the glider to the recovery position. */
    uint32_t dive_num = 0;
    while (1) {
        dive_num++;
        struct dive_errors dive_errs = {0};
        double final_depth = 0.0;
        enum dive_abort cause = dive_cycle(p, surface_pa, simulated,
                                           mission_num, dive_num, &dive_errs,
                                           &final_depth);

        if (cause != ABORT_NONE) {
            app_printk("\r\n%s *** DIVE ABORTED at %.2fm: %s ***\r\n",
                       tag, final_depth, abort_text(cause));
            g_abort_text = abort_text(cause);
            recovery_position(p, tag);
            dive_log_close(cycle_elapsed_s(), final_depth, &dive_errs);
            recovery_hold(p, tag, simulated, &dive_errs);
            tally_errors(&errs, &dive_errs);
            break;
        }

        app_printk("%s surfaced\r\n", tag);
        take_gps_fix(simulated, tag, &dive_errs);
        dive_log_gps(2, g_fix_ok, g_fix_lat, g_fix_lon);

        dive_log_close(cycle_elapsed_s(), final_depth, &dive_errs);
        tally_errors(&errs, &dive_errs);

        app_printk("%s press ENTER to stop (next dive starts in 10 seconds)...\r\n", tag);
        if (enter_pressed_within(10)) {
            app_printk("%s user requested stop\r\n", tag);
            break;
        }
        app_printk("%s no user input, starting another dive cycle\r\n", tag);
    }

    app_printk("%s mission complete (%u dive%s, sensor errors: "
               "depth=%u hull=%u temp=%u compass=%u gps=%u)\r\n",
               tag, dive_num, dive_num == 1 ? "" : "s",
               errs.ms5837, errs.bmp180, errs.tsys01, errs.hmc6343, errs.gps);
    app_printk("%s returning to menu\r\n", tag);
}

void deploy_start(void)   { mission_run(false); }
void simulate_start(void) { mission_run(true);  }

/* =====================================================================
 * Async workers
 *
 * Missions run on their own threads so the UI/network stay responsive.
 * The *_running atomics both guard double-starts and let the UI poll for
 * completion.
 * ===================================================================== */

static K_THREAD_STACK_DEFINE(deploy_stack, 4096);
static struct k_thread deploy_thread;
static atomic_t deploy_running = ATOMIC_INIT(0);

static void deploy_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    deploy_start();
    atomic_clear(&deploy_running);
}

void deploy_start_async(void)
{
    if (atomic_get(&deploy_running)) {
        app_printk("[DEPLOY] already running\r\n");
        return;
    }
    atomic_set(&deploy_running, 1);
    k_thread_create(&deploy_thread, deploy_stack, K_THREAD_STACK_SIZEOF(deploy_stack),
                    deploy_thread_fn, NULL, NULL, NULL,
                    8 /* lower priority than WiFi/telnet */, 0, K_NO_WAIT);
    k_thread_name_set(&deploy_thread, "deploy_worker");
    app_printk("[DEPLOY] worker started\r\n");
}

static K_THREAD_STACK_DEFINE(simulate_stack, 4096);
static struct k_thread simulate_thread;
static atomic_t simulate_running = ATOMIC_INIT(0);

static void simulate_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    simulate_start();
    atomic_clear(&simulate_running);
}

void simulate_start_async(void)
{
    if (atomic_get(&simulate_running)) {
        app_printk("[SIMULATE] already running\r\n");
        return;
    }
    atomic_set(&simulate_running, 1);
    k_thread_create(&simulate_thread, simulate_stack, K_THREAD_STACK_SIZEOF(simulate_stack),
                    simulate_thread_fn, NULL, NULL, NULL,
                    8 /* lower priority than WiFi/telnet */, 0, K_NO_WAIT);
    k_thread_name_set(&simulate_thread, "simulate_worker");
    app_printk("[SIMULATE] worker started\r\n");
}

bool deploy_is_running(void)   { return atomic_get(&deploy_running) != 0; }
bool simulate_is_running(void) { return atomic_get(&simulate_running) != 0; }
