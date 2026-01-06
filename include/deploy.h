/* deploy.h - deploy sequence API */
#ifndef DEPLOY_H
#define DEPLOY_H
#include <zephyr/kernel.h>
#include <stdbool.h>

void deploy_start(void);
/* Async deploy runner: spawns a worker thread that runs deploy_start() */
void deploy_start_async(void);
/* Simulate deployment: runs entire sequence with simulated external pressure */
void simulate_start(void);
/* Async simulate runner: spawns a worker thread that runs simulate_start() */
void simulate_start_async(void);
/* Check if external pressure sensor is available */
bool deploy_check_sensor_available(void);
/* Check if deploy is currently running */
bool deploy_is_running(void);
/* Check if simulate is currently running */
bool simulate_is_running(void);
#endif
