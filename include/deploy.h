/*
 * deploy.h — Mission execution API (deploy + simulate)
 *
 * Missions run on a worker thread so the UI stays responsive. Input
 * ownership: the main UI loop reads ALL console input (both UART and
 * WiFi); the worker never polls the consoles itself. When the operator
 * presses ENTER during a mission, the UI forwards it with
 * deploy_notify_enter(), and the worker stops at the next surface window.
 */
#ifndef DEPLOY_H
#define DEPLOY_H
#include <zephyr/kernel.h>
#include <stdbool.h>

/* Run a real mission (blocking; used by the async runner). */
void deploy_start(void);
/* Async deploy runner: spawns a worker thread that runs deploy_start() */
void deploy_start_async(void);
/* Simulated mission: same sequence with synthetic depth (blocking). */
void simulate_start(void);
/* Async simulate runner: spawns a worker thread that runs simulate_start() */
void simulate_start_async(void);
/* Check if external pressure sensor is available */
bool deploy_check_sensor_available(void);
/* Check if deploy is currently running */
bool deploy_is_running(void);
/* Check if simulate is currently running */
bool simulate_is_running(void);

/* Called from the UI when the operator presses ENTER while a mission or
 * simulation is running: requests a stop at the next surface window.
 * Safe to call any time — pressing ENTER mid-dive schedules the stop. */
void deploy_notify_enter(void);

/* True when the last mission ended because a safety guard tripped rather
 * than because the operator stopped it. The glider is then parked in the
 * recovery position and the UI should land in the RECOVERY state. */
bool deploy_aborted(void);

/* Human-readable cause of that abort, or NULL if the mission ended
 * normally. Valid until the next mission starts. */
const char *deploy_abort_reason(void);

#endif
