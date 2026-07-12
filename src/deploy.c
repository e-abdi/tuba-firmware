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
 * Every monitoring second a [SENS] line is printed with a UTC timestamp,
 * internal (hull) pressure, depth, water temperature (TSYS01) and
 * compass attitude. Heading is held by banking the roll mass — see
 * update_roll_for_heading().
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
#include "hw_ms5837.h"
#include "hw_bmp180.h"
#include "hw_hmc6343.h"
#include "hw_tsys01.h"
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
    app_printk("[MOVE] %s %s: %.1fs -> %.1fs (%us)\r\n",
               (id == MOTOR_ROLL) ? "roll" : "pitch", why, cur, target_s, duration);
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
static bool update_roll_for_heading(bool dive_phase, float current_heading,
                                    float desired_heading, struct app_params *p)
{
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
 * Sensor sampling + [SENS] telemetry line
 * ===================================================================== */

/* One snapshot of everything reported during a mission. */
struct sens_sample {
    int32_t internal_pa;     /* hull pressure [Pa] (0 if read failed)    */
    double  depth_m;         /* depth below surface reference [m]        */
    double  water_temp_c;    /* TSYS01 water temperature [C]             */
    bool    water_temp_ok;   /* false when the TSYS01 is absent/failed   */
    float   heading, pitch, roll;   /* compass attitude [deg]            */
};

/* Read all mission sensors. depth is taken from the caller (it depends on
 * real-vs-simulated mode); everything else is sampled here. */
static void sens_read(struct sens_sample *s, double depth_m)
{
    s->internal_pa = 0;
    (void)bmp180_read_pa(&s->internal_pa);

    s->depth_m = depth_m < 0.0 ? 0.0 : depth_m;

    s->water_temp_ok = (tsys01_read(&s->water_temp_c) == 0);

    s->heading = s->pitch = s->roll = 0.0f;
    (void)hmc6343_read(&s->heading, &s->pitch, &s->roll);
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

/* =====================================================================
 * Depth sources
 * ===================================================================== */

/* Real depth from the external pressure sensor relative to the surface
 * reference. Returns <0 on sensor failure. */
static double real_depth_m(double surface_pa, const char *tag)
{
    double temp_c = 0.0, press_kpa = 0.0;
    if (ms5837_read(&temp_c, &press_kpa) != 0) {
        app_printk("%s External pressure read failed\r\n", tag);
        return -1.0;
    }
    double external_pa = press_kpa * 1000.0;
    double depth = (surface_pa > 0.0)
                 ? (external_pa - surface_pa) / (SEA_WATER_DENSITY_KG_M3 * GRAVITY_M_S2)
                 : 0.0;
    return depth < 0.0 ? 0.0 : depth;
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
 * One dive/climb cycle (shared by deploy and simulate)
 * ===================================================================== */
static void dive_cycle(struct app_params *p, double surface_pa, bool simulated)
{
    const char *tag = simulated ? "[SIMULATE]" : "[DEPLOY]";
    int heading_check_counter = 0;

    /* --- 1. Surface trim: pitch and pump to their surface positions --- */
    app_printk("%s moving to surface position (pitch=%us, pump=%us)\r\n",
               tag, p->start_pitch_s, p->start_pump_s);
    motor_move_to(MOTOR_PITCH, (float)p->start_pitch_s, "to surface position");
    pump_move_to((float)p->start_pump_s, "to surface position");

    /* --- 2. Dive trim: nose down, ballast in --- */
    app_printk("%s moving to dive targets (pitch=%us, pump=%us)\r\n",
               tag, p->dive_pitch_s, p->dive_pump_s);
    motor_move_to(MOTOR_PITCH, (float)p->dive_pitch_s, "to dive target");
    pump_move_to((float)p->dive_pump_s, "to dive target");

    /* --- 3. Sink: monitor once per second until target depth/timeout --- */
    app_printk("%s diving to %.1fm%s\r\n", tag, (double)p->dive_depth_m,
               simulated ? " (simulated at 0.5 m/s)" : "");
    uint64_t dive_start_ms = k_uptime_get();
    uint64_t deadline_ms = dive_start_ms + (uint64_t)p->dive_timeout_min * 60ULL * 1000ULL;

    while (1) {
        double depth;
        if (simulated) {
            double elapsed_s = (double)(k_uptime_get() - dive_start_ms) / 1000.0;
            depth = SIM_SINK_M_S * elapsed_s;
        } else {
            depth = real_depth_m(surface_pa, tag);
            if (depth < 0.0) depth = 0.0;   /* read failure: report 0, keep trying */
        }

        struct sens_sample s;
        sens_read(&s, depth);
        sens_print(&s, simulated);

        /* Heading hold: correct every HEADING_CHECK_INTERVAL_SEC seconds. */
        if (++heading_check_counter >= HEADING_CHECK_INTERVAL_SEC) {
            heading_check_counter = 0;
            update_roll_for_heading(true, s.heading, (float)p->desired_heading_deg, p);
        }

        if (depth >= (double)p->dive_depth_m) {
            app_printk("%s target depth reached (%.2fm) -> start climb\r\n", tag, depth);
            break;
        }
        if (!simulated && (int64_t)k_uptime_get() >= (int64_t)deadline_ms) {
            app_printk("%s dive timeout -> start climb\r\n", tag);
            break;
        }

        k_sleep(K_SECONDS(1));
    }

    /* --- 4. Climb trim: nose up, ballast out --- */
    app_printk("%s moving to climb targets (pitch=%us, pump=%us)\r\n",
               tag, p->climb_pitch_s, p->climb_pump_s);
    motor_move_to(MOTOR_PITCH, (float)p->climb_pitch_s, "to climb target");
    pump_move_to((float)p->climb_pump_s, "to climb target");

    /* --- 5. Rise: monitor until near-surface (max 60 s of monitoring) --- */
    uint64_t climb_start_ms = k_uptime_get();
    double climb_start_depth = (double)p->dive_depth_m;
    heading_check_counter = 0;

    for (int i = 0; i < 60; i++) {
        double depth;
        if (simulated) {
            double elapsed_s = (double)(k_uptime_get() - climb_start_ms) / 1000.0;
            depth = climb_start_depth - SIM_RISE_M_S * elapsed_s;
            if (depth < 0.0) depth = 0.0;
        } else {
            depth = real_depth_m(surface_pa, tag);
            if (depth < 0.0) depth = 0.0;
        }

        struct sens_sample s;
        sens_read(&s, depth);
        sens_print(&s, simulated);

        if (++heading_check_counter >= HEADING_CHECK_INTERVAL_SEC) {
            heading_check_counter = 0;
            update_roll_for_heading(false, s.heading, (float)p->desired_heading_deg, p);
        }

        if (depth < SURFACE_DEPTH_M) {
            /* --- 6. Surfaced: return all actuators to surface trim --- */
            app_printk("%s depth < %.0fm; moving to surface position\r\n",
                       tag, SURFACE_DEPTH_M);
            motor_move_to(MOTOR_PITCH, (float)p->start_pitch_s, "surfacing");
            pump_move_to((float)p->start_pump_s, "surfacing");
            motor_move_to(MOTOR_ROLL, (float)p->start_roll_s, "to neutral (surfaced)");

            /* A few extra readings while settled at the surface. */
            for (int j = 0; j < 5; j++) {
                k_sleep(K_SECONDS(1));
                sens_read(&s, 0.0);
                sens_print(&s, simulated);
            }
            break;
        }

        k_sleep(K_SECONDS(1));
    }
}

/* =====================================================================
 * Mission runners
 * ===================================================================== */

/* Shared mission sequence; simulated selects the depth source and skips
 * real GPS fixes. */
static void mission_run(bool simulated)
{
    struct app_params *p = app_params_get();
    const char *tag = simulated ? "[SIMULATE]" : "[DEPLOY]";

    /* Forget any stop request left over from a previous mission. */
    atomic_clear(&stop_requested);

    app_printk("%s starting sequence%s\r\n", tag,
               simulated ? " (pressure simulated)" : "");

    /* Surface pressure reference for depth calculation. */
    double surface_pa;
    if (simulated) {
        surface_pa = 101325.0;   /* standard sea level */
        app_printk("%s simulated surface pressure: %.3f kPa\r\n", tag, surface_pa / 1000.0);
    } else {
        double temp_c = 0.0, press_kpa = 0.0;
        if (ms5837_read(&temp_c, &press_kpa) != 0) {
            app_printk("%s ERROR: cannot read external pressure sensor (MS5837)\r\n", tag);
            app_printk("%s Try 'simulate' instead to test with simulated pressure\r\n", tag);
            return;
        }
        surface_pa = press_kpa * 1000.0;
        app_printk("%s surface external pressure: %.3f kPa (T=%.2f C)\r\n",
                   tag, press_kpa, temp_c);
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

    /* GPS fix before the first dive (also syncs the system clock). */
    if (simulated) {
        app_printk("%s acquiring simulated GPS fix\r\n", tag);
        k_sleep(K_SECONDS(2));
        app_printk("[GPS] acquired (simulated)\r\n");
    } else {
        app_printk("%s acquiring GPS fix before dive\r\n", tag);
        gps_fix_wait(30);
    }

    /* Main dive/climb loop: repeat until the operator presses ENTER in
     * the 10-second surface window after each cycle. */
    while (1) {
        dive_cycle(p, surface_pa, simulated);

        if (simulated) {
            app_printk("%s surfaced, getting simulated GPS fix\r\n", tag);
            k_sleep(K_SECONDS(2));
            app_printk("[GPS] acquired (simulated)\r\n");
        } else {
            app_printk("%s surfaced, getting GPS fix\r\n", tag);
            gps_fix_wait(30);
        }

        app_printk("%s press ENTER to stop (next dive starts in 10 seconds)...\r\n", tag);
        if (enter_pressed_within(10)) {
            app_printk("%s user requested stop\r\n", tag);
            break;
        }
        app_printk("%s no user input, starting another dive cycle\r\n", tag);
    }

    app_printk("%s mission complete, returning to menu\r\n", tag);
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
