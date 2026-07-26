/*
 * ui_menu.c — Menu system implementation
 *
 * This file contains all per-state behavior of the UI state machine:
 *   - on_entry_*()  print the menu/prompt when a state is entered
 *   - on_event_*()  react to async events (ticks, timeouts, bare ENTER)
 *   - ui_handle_line() routes every typed input line based on the current
 *     state and returns the next state
 *
 * The dispatch table that binds these to state IDs lives in main.c.
 *
 * Menu tree (see also README.md):
 *   MENU
 *   ├── 1 PARAMETERS ── edit/save/reset mission parameters
 *   ├── 2 HARDWARE TEST
 *   │   ├── 1 pitch & roll ── motor jog + limit-switch test
 *   │   ├── 2 pump
 *   │   ├── 3 show positions
 *   │   ├── 4 internal pressure (BMP180)
 *   │   ├── 5 external pressure (MS5837)
 *   │   ├── 6 GPS
 *   │   ├── 7 compass (HMC6343)
 *   │   ├── 8 leak sensor
 *   │   ├── 9 SENSORS
 *   │   │   └── 1 TEMPERATURE (TSYS01)
 *   │   │       ├── 1 self-test
 *   │   │       └── 2 stream
 *   │   └── a I2C sensor scan report
 *   ├── 3 SIMULATE ── mission with simulated depth
 *   ├── 4 DEPLOY   ── real mission
 *   └── 5 OTA firmware update
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "app_events.h"
#include "app_limits.h"
#include "app_params.h"
#include "app_time.h"
#include "ui_menu.h"
#include "hw_motors.h"
#include "hw_pump.h"
#include "hw_bmp180.h"
#include "hw_ms5837.h"
#include "hw_gps.h"
#include "hw_hmc6343.h"
#include "hw_limit_switches.h"
#include "hw_leak.h"
#include "hw_tsys01.h"
#include "sensors.h"
#include "deploy.h"
#include "ota_simple.h"
#include "net_console.h"
#include "version.h"

/* Motor currently being jogged in the PR_INPUT state (set by PR_MENU). */
extern enum motor_id current_motor;

/* Parameter selected for editing in the PARAM_INPUT state. */
static int current_param_index = 0;

/* POWERUP countdown bookkeeping. */
static int32_t remaining_sec = STARTUP_TIMEOUT_SEC;
static int32_t tick_counter = 0;   /* EVT_TICK fires at 20 Hz; count to 1 s */

/* =====================================================================
 * State entry hooks — print the menu/prompt for each state
 * ===================================================================== */

void on_entry_POWERUP_WAIT(void){
    remaining_sec = STARTUP_TIMEOUT_SEC;
    app_printk("\r\n-- POWERUP --\r\n");
    app_printk("press ENTER within %d seconds or the glider will go to DEPLOYED\r\n", remaining_sec);
}

void on_exit_POWERUP_WAIT(void){ app_printk("\r\n"); }

void on_entry_MENU(void){
    char ts[APP_TIME_STR_LEN];
    app_printk("\r\n=== MENU ===  (build %d | %s UTC [%s] | input: %s)\r\n",
               BUILD_NUMBER,
               app_time_str(ts, sizeof(ts)),
               app_time_source_str(),
               console_input_mode_str(console_input_mode_get()));
    app_printk("1) parameters\r\n");
    app_printk("2) hardware test\r\n");
    app_printk("3) simulate\r\n");
    app_printk("4) deploy\r\n");
    app_printk("5) OTA firmware update\r\n");
    app_printk("Select [1-5]: ");
}

void on_entry_PARAMS_MENU(void){
    struct app_params *p = app_params_get();
    app_printk("\r\n-- PARAMETERS --\r\n");
    app_printk("1) Dive depth [m]: %d\r\n", (int)p->dive_depth_m);
    app_printk("2) Wait before dive [s]: %u\r\n", p->deploy_wait_s);
    app_printk("3) Dive timeout [min]: %u\r\n", p->dive_timeout_min);
    app_printk("4) Dive pump [s]: %u\r\n", p->dive_pump_s);
    app_printk("5) Start pump [s]: %u\r\n", p->start_pump_s);
    app_printk("6) Climb pump [s]: %u\r\n", p->climb_pump_s);
    app_printk("7) Start pitch [s]: %u\r\n", p->start_pitch_s);
    app_printk("8) Surface pitch [s]: %u\r\n", p->surface_pitch_s);
    app_printk("9) Dive pitch [s]: %u\r\n", p->dive_pitch_s);
    app_printk("a) Climb pitch [s]: %u\r\n", p->climb_pitch_s);
    app_printk("b) Start roll [s]: %u\r\n", p->start_roll_s);
    app_printk("c) Max roll [s]: %u\r\n", p->max_roll_s);
    app_printk("d) Roll time [s]: %u\r\n", p->roll_time_s);
    app_printk("e) Desired heading [deg]: %d\r\n", p->desired_heading_deg);
    app_printk("f) Leak threshold [mV, 0=off]: %u\r\n", p->leak_threshold_mv);
    app_printk("g) Console input [0=UART+WiFi 1=UART 2=WiFi]: %u (%s)\r\n",
               p->console_input_mode,
               console_input_mode_str((console_input_mode_t)p->console_input_mode));
    app_printk("s) Save parameters\r\n");
    app_printk("r) Reset defaults\r\n");
    app_printk("x) Back\r\n");
    app_printk("Select [1-9,a-g,s,r,x]: ");
}

void on_entry_HWTEST_MENU(void){
    app_printk("\r\n-- HARDWARE TEST --\r\n");
    app_printk("1) pitch and roll\r\n");
    app_printk("2) pump\r\n");
    app_printk("3) show positions\r\n");
    app_printk("4) Internal Pressure\r\n");
    app_printk("5) External Pressure\r\n");
    app_printk("6) GPS\r\n");
    app_printk("7) Compass\r\n");
    app_printk("8) Leak sensor\r\n");
    app_printk("9) Sensors\r\n");
    app_printk("a) I2C sensor scan report\r\n");
    app_printk("x) back\r\n");
    app_printk("Select [1-9,a,x]: ");
}

void on_entry_SENSORS_MENU(void){
    app_printk("\r\n-- SENSORS --\r\n");
    app_printk("1) Temperature (TSYS01)\r\n");
    app_printk("x) back\r\n");
    app_printk("Select [1,x]: ");
}

void on_entry_TEMP_MENU(void){
    app_printk("\r\n-- TEMPERATURE (TSYS01) --\r\n");
    app_printk("1) self-test\r\n");
    app_printk("2) stream\r\n");
    app_printk("x) back\r\n");
    app_printk("Select [1-2,x]: ");
}

void on_entry_COMPASS_MENU(void){
    app_printk("\r\n-- Compass (HMC6343) --\r\n");
    app_printk("1) Calibrate (enter/exit)\r\n");
    app_printk("2) Continuous heading (q to quit)\r\n");
    app_printk("x) back\r\n");
    app_printk("Select [1,2,x]: ");
}

void on_entry_OTA_MENU(void){
    app_printk("\r\n-- OTA Firmware Update --\r\n");
    app_printk("Enter firmware URL (e.g., http://192.168.4.100:8080/firmware.bin)\r\n");
    app_printk("or press 'x' to cancel: ");
}

void on_entry_PR_MENU(void){
    app_printk("\r\n-- Pitch & Roll --\r\n");
    app_printk("1) roll\r\n");
    app_printk("2) pitch\r\n");
    app_printk("3) pitch limit switches (test)\r\n");
    app_printk("x) back\r\n");
    app_printk("Select [1-3,x]: ");
}

void on_entry_RECOVERY(void){
    app_printk("\r\n-- RECOVERY state --\r\n");
    app_printk("Type 'resume' to go to DEPLOYED.\r\n");
}

void on_entry_DEPLOYED(void){
    app_printk("\r\n== DEPLOYED state ==\r\n");
    /* Run the mission asynchronously so the UI and networking stay live. */
    deploy_start_async();
}

void on_entry_SIMULATE(void){
    app_printk("\r\n== SIMULATE state (lab testing with simulated pressure) ==\r\n");
    /* Run the simulation asynchronously so the UI and networking stay live. */
    simulate_start_async();
}

/* =====================================================================
 * Async event handlers
 *
 * Most states only react to typed lines; their event handlers simply stay
 * put. POWERUP_WAIT is the exception: it owns the boot countdown.
 * DEPLOYED/SIMULATE use events as a poll point to notice that their worker
 * thread has finished and it is time to return to the menu.
 * ===================================================================== */

state_id_t on_event_POWERUP_WAIT(const event_t *e){
    switch (e->id) {
    case EVT_TICK:
        /* EVT_TICK fires every 50 ms; update the countdown once per second. */
        tick_counter++;
        if (tick_counter >= 20) {
            tick_counter = 0;
            if (remaining_sec > 0) remaining_sec--;
            app_printk("\rpress ENTER within %d seconds or the glider will go to DEPLOYED   ", remaining_sec);
        }
        return ST_POWERUP_WAIT;
    case EVT_ENTER:
        app_printk("\r\nENTER received -> MENU\r\n");
        return ST_MENU;
    case EVT_TIMEOUT:
        app_printk("\r\nTimeout -> DEPLOYED\r\n");
        return ST_DEPLOYED;
    default:
        return ST_POWERUP_WAIT;
    }
}

state_id_t on_event_MENU(const event_t *e){ ARG_UNUSED(e); return ST_MENU; }
state_id_t on_event_HWTEST_MENU(const event_t *e){ ARG_UNUSED(e); return ST_HWTEST_MENU; }
state_id_t on_event_SENSORS_MENU(const event_t *e){ ARG_UNUSED(e); return ST_SENSORS_MENU; }
state_id_t on_event_TEMP_MENU(const event_t *e){ ARG_UNUSED(e); return ST_TEMP_MENU; }
state_id_t on_event_PARAMS_MENU(const event_t *e){ ARG_UNUSED(e); return ST_PARAMS_MENU; }
state_id_t on_event_PARAM_INPUT(const event_t *e){ ARG_UNUSED(e); return ST_PARAM_INPUT; }
state_id_t on_event_PR_MENU(const event_t *e){ ARG_UNUSED(e); return ST_PR_MENU; }
state_id_t on_event_PR_INPUT(const event_t *e){ ARG_UNUSED(e); return ST_PR_INPUT; }
state_id_t on_event_PUMP_INPUT(const event_t *e){ ARG_UNUSED(e); return ST_PUMP_INPUT; }
state_id_t on_event_RECOVERY(const event_t *e){ ARG_UNUSED(e); return ST_RECOVERY; }
state_id_t on_event_COMPASS_MENU(const event_t *e){ ARG_UNUSED(e); return ST_COMPASS_MENU; }
state_id_t on_event_OTA_MENU(const event_t *e){ ARG_UNUSED(e); return ST_OTA_MENU; }

state_id_t on_event_DEPLOYED(const event_t *e){
    /* The main loop owns all console input; forward ENTER to the mission
     * worker so it can stop at the next surface window. (The worker must
     * not poll the consoles itself — it would race the main loop.) */
    if (e->id == EVT_ENTER) {
        deploy_notify_enter();
    }
    /* The mission runs in a worker thread; when it exits, go back to MENU. */
    if (!deploy_is_running()) {
        return ST_MENU;
    }
    return ST_DEPLOYED;
}

state_id_t on_event_SIMULATE(const event_t *e){
    if (e->id == EVT_ENTER) {
        deploy_notify_enter();
    }
    if (!simulate_is_running()) {
        return ST_MENU;
    }
    return ST_SIMULATE;
}

/* =====================================================================
 * Typed-line dispatcher
 * ===================================================================== */

/* Parse a bounded integer; returns true and stores into *out on success. */
static bool parse_int(const char *line, long min, long max, long *out)
{
    char *endp = NULL;
    long val = strtol(line, &endp, 10);
    if (endp == line || *endp != '\0' || val < min || val > max) {
        return false;
    }
    *out = val;
    return true;
}

state_id_t ui_handle_line(state_id_t state, const char *line){
    /* Empty lines are handled as EVT_ENTER by the main loop, not here. */
    if (!line || line[0] == '\0') {
        return ST__COUNT;  /* no state change */
    }

    /* If a mission/simulation worker finished while we were typing, fall
     * back to the menu before interpreting the input. */
    if (state == ST_DEPLOYED && !deploy_is_running()) {
        return ST_MENU;
    }
    if (state == ST_SIMULATE && !simulate_is_running()) {
        return ST_MENU;
    }

    /* ---- Top-level menu ---- */
    if (state==ST_MENU){
        if(line[0]=='1') return ST_PARAMS_MENU;
        if(line[0]=='2') return ST_HWTEST_MENU;
        if(line[0]=='3') return ST_SIMULATE;
        if(line[0]=='4') {
            /* Refuse to deploy without the external pressure sensor: the
             * dive loop cannot measure depth without it. */
            if (!deploy_check_sensor_available()) {
                app_printk("[DEPLOY] ERROR: external pressure sensor not available\r\n");
                app_printk("[DEPLOY] Try option 3 (simulate) instead\r\n");
                on_entry_MENU();
                return ST_MENU;
            }
            return ST_DEPLOYED;
        }
        if(line[0]=='5') return ST_OTA_MENU;
        app_printk("Invalid.\r\n");
        return ST_MENU;
    }

    /* ---- Hardware test menu ---- */
    if (state==ST_HWTEST_MENU){
        if(line[0]=='1') return ST_PR_MENU;
        if(line[0]=='2') { app_printk("[PUMP] Enter seconds [-10,10], q to quit\r\n> "); return ST_PUMP_INPUT; }
        if(line[0]=='3') {
            /* Positions are "seconds of travel from zero" dead-reckoning. */
            long roll  = (long)motor_get_position_sec(MOTOR_ROLL);
            long pitch = (long)motor_get_position_sec(MOTOR_PITCH);
            long pump  = (long)pump_get_position_sec();
            app_printk("\r\n[POSITION] roll=%lds, pitch=%lds, pump=%lds\r\n",
                       roll, pitch, pump);
            on_entry_HWTEST_MENU();
            return ST_HWTEST_MENU;
        }
        if(line[0]=='4') {
            /* BMP180 stream (blocking; returns when user hits 'q') */
            bmp180_stream_interactive();
            on_entry_HWTEST_MENU();
            return ST_HWTEST_MENU;
        }
        if(line[0]=='5') {
            /* MS5837 stream (blocking; returns when user hits 'q') */
            ms5837_stream_interactive();
            on_entry_HWTEST_MENU();
            return ST_HWTEST_MENU;
        }
        if(line[0]=='6') { gps_fix_interactive(); on_entry_HWTEST_MENU(); return ST_HWTEST_MENU; }
        if(line[0]=='7') { return ST_COMPASS_MENU; }
        if(line[0]=='8') {
            leak_detector_stream_interactive(app_params_get()->leak_threshold_mv);
            on_entry_HWTEST_MENU();
            return ST_HWTEST_MENU;
        }
        if(line[0]=='9') { return ST_SENSORS_MENU; }
        if(line[0]=='a' || line[0]=='A') {
            sensors_report();
            on_entry_HWTEST_MENU();
            return ST_HWTEST_MENU;
        }
        if(line[0]=='x' || line[0]=='X') { return ST_MENU; }
        app_printk("Invalid.\r\n");
        return ST_HWTEST_MENU;
    }

    /* ---- Sensors submenu ---- */
    if (state==ST_SENSORS_MENU){
        if(line[0]=='1') { return ST_TEMP_MENU; }
        if(line[0]=='x' || line[0]=='X') { return ST_HWTEST_MENU; }
        app_printk("Invalid.\r\n");
        return ST_SENSORS_MENU;
    }

    /* ---- Temperature (TSYS01) submenu ---- */
    if (state==ST_TEMP_MENU){
        if(line[0]=='1') {
            (void)tsys01_selftest();
            on_entry_TEMP_MENU();
            return ST_TEMP_MENU;
        }
        if(line[0]=='2') {
            tsys01_stream_interactive();
            on_entry_TEMP_MENU();
            return ST_TEMP_MENU;
        }
        if(line[0]=='x' || line[0]=='X') { return ST_SENSORS_MENU; }
        app_printk("Invalid.\r\n");
        return ST_TEMP_MENU;
    }

    /* ---- Pitch & roll test menu ---- */
    if (state==ST_PR_MENU){
        if(line[0]=='1'){ current_motor=MOTOR_ROLL;  app_printk("[ROLL] Enter seconds [-10,10], q to quit\r\n> ");  return ST_PR_INPUT; }
        if(line[0]=='2'){ current_motor=MOTOR_PITCH; app_printk("[PITCH] Enter seconds [-10,10], q to quit\r\n> "); return ST_PR_INPUT; }
        if(line[0]=='3'){ limit_switches_test_interactive(); on_entry_PR_MENU(); return ST_PR_MENU; }
        if(line[0]=='x' || line[0]=='X'){ return ST_HWTEST_MENU; }
        app_printk("Invalid.\r\n");
        return ST_PR_MENU;
    }

    /* ---- Motor jog input: signed seconds, +fwd / -rev ---- */
    if (state==ST_PR_INPUT){
        if((line[0]=='q'||line[0]=='Q') && line[1]=='\0'){ return ST_PR_MENU; }
        long val;
        if(!parse_int(line, TEST_MIN_SEC, TEST_MAX_SEC, &val)){
            app_printk("Enter an integer in [%d..%d], or q\r\n> ", TEST_MIN_SEC, TEST_MAX_SEC);
            return ST_PR_INPUT;
        }
        int dir=(val>=0)?+1:-1; uint32_t dur=(val>=0)?(uint32_t)val:(uint32_t)(-val);
        motor_run(current_motor,dir,dur); app_printk("> "); return ST_PR_INPUT;
    }

    /* ---- Pump jog input: signed seconds, +out / -in ---- */
    if (state==ST_PUMP_INPUT){
        if((line[0]=='q'||line[0]=='Q') && line[1]=='\0'){ return ST_HWTEST_MENU; }
        long val;
        if(!parse_int(line, TEST_MIN_SEC, TEST_MAX_SEC, &val)){
            app_printk("Enter an integer in [%d..%d], or q\r\n> ", TEST_MIN_SEC, TEST_MAX_SEC);
            return ST_PUMP_INPUT;
        }
        int dir=(val>=0)?+1:-1; uint32_t dur=(val>=0)?(uint32_t)val:(uint32_t)(-val);
        pump_run(dir,dur); app_printk("> "); return ST_PUMP_INPUT;
    }

    /* ---- Compass submenu ---- */
    if (state==ST_COMPASS_MENU){
        if(line[0]=='1') { hmc6343_user_calibrate_interactive(); on_entry_COMPASS_MENU(); return ST_COMPASS_MENU; }
        if(line[0]=='2') { hmc6343_stream_heading_interactive(); on_entry_COMPASS_MENU(); return ST_COMPASS_MENU; }
        if(line[0]=='x' || line[0]=='X') { return ST_HWTEST_MENU; }
        app_printk("Invalid.\r\n");
        return ST_COMPASS_MENU;
    }

    /* ---- OTA: the typed line is the firmware URL ---- */
    if (state==ST_OTA_MENU){
        if(line[0]=='x' || line[0]=='X') { return ST_MENU; }

        app_printk("\r\n[OTA] Starting firmware download from: %s\r\n", line);
        int ret = ota_simple_download(line);
        if(ret == 0){
            app_printk("[OTA] Download complete. Verifying...\r\n");
            if(ota_simple_verify() == 0){
                app_printk("[OTA] Firmware verified! Rebooting to apply update...\r\n");
                k_msleep(1000);
                ota_simple_reboot();   /* does not return */
            }
            app_printk("[OTA] Verification failed. Returning to menu.\r\n");
        } else {
            app_printk("[OTA] Download failed with error code: %d\r\n", ret);
        }
        on_entry_MENU();
        return ST_MENU;
    }

    /* ---- Parameter list: pick an entry, save, reset, or leave ---- */
    if (state==ST_PARAMS_MENU){
        if(line[0]=='x' || line[0]=='X'){ return ST_MENU; }
        if(line[0]=='s' || line[0]=='S'){ app_params_save(); on_entry_PARAMS_MENU(); return ST_PARAMS_MENU; }
        if(line[0]=='r' || line[0]=='R'){ app_params_reset_defaults(); on_entry_PARAMS_MENU(); return ST_PARAMS_MENU; }
        /* select parameter to edit */
        if(line[0]=='1'){ current_param_index = 1; app_printk("Enter Dive depth [m]: "); return ST_PARAM_INPUT; }
        if(line[0]=='2'){ current_param_index = 2; app_printk("Enter Wait before dive [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='3'){ current_param_index = 3; app_printk("Enter Dive timeout [min]: "); return ST_PARAM_INPUT; }
        if(line[0]=='4'){ current_param_index = 4; app_printk("Enter Dive pump [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='5'){ current_param_index = 5; app_printk("Enter Start pump [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='6'){ current_param_index = 6; app_printk("Enter Climb pump [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='7'){ current_param_index = 7; app_printk("Enter Start pitch [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='8'){ current_param_index = 8; app_printk("Enter Surface pitch [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='9'){ current_param_index = 9; app_printk("Enter Dive pitch [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='a' || line[0]=='A'){ current_param_index = 10; app_printk("Enter Climb pitch [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='b' || line[0]=='B'){ current_param_index = 11; app_printk("Enter Start roll [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='c' || line[0]=='C'){ current_param_index = 12; app_printk("Enter Max roll [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='d' || line[0]=='D'){ current_param_index = 13; app_printk("Enter Roll time [s]: "); return ST_PARAM_INPUT; }
        if(line[0]=='e' || line[0]=='E'){ current_param_index = 14; app_printk("Enter Desired heading [deg]: "); return ST_PARAM_INPUT; }
        if(line[0]=='f' || line[0]=='F'){ current_param_index = 15; app_printk("Enter Leak threshold [mV, 0=off]: "); return ST_PARAM_INPUT; }
        if(line[0]=='g' || line[0]=='G'){
            current_param_index = 16;
            app_printk("Enter Console input mode [0=UART+WiFi 1=UART only 2=WiFi only]: ");
            return ST_PARAM_INPUT;
        }
        app_printk("Invalid.\r\n");
        return ST_PARAMS_MENU;
    }

    /* ---- Parameter value entry ---- */
    if (state==ST_PARAM_INPUT){
        struct app_params *p = app_params_get();
        long val;
        if(!parse_int(line, LONG_MIN, LONG_MAX, &val)){
            app_printk("Not a valid integer: '%s'\r\n", line);
            return ST_PARAMS_MENU;
        }
        switch(current_param_index){
        case 1:  p->dive_depth_m = (float)val; break;
        case 2:  p->deploy_wait_s = (uint16_t)val; break;
        case 3:  p->dive_timeout_min = (uint16_t)val; break;
        case 4:  p->dive_pump_s = (uint16_t)val; break;
        case 5:  p->start_pump_s = (uint16_t)val; break;
        case 6:  p->climb_pump_s = (uint16_t)val; break;
        case 7:  p->start_pitch_s = (uint16_t)val; break;
        case 8:  p->surface_pitch_s = (uint16_t)val; break;
        case 9:  p->dive_pitch_s = (uint16_t)val; break;
        case 10: p->climb_pitch_s = (uint16_t)val; break;
        case 11: p->start_roll_s = (uint16_t)val; break;
        case 12: p->max_roll_s = (uint16_t)val; break;
        case 13: p->roll_time_s = (uint16_t)val; break;
        case 14: p->desired_heading_deg = (int16_t)val; break;
        case 15: p->leak_threshold_mv   = (uint16_t)val; break;
        case 16:
            /* Console input mode: clamp and apply immediately so the user
             * gets instant feedback (and can undo from the other console
             * if they cut off the one they are typing on). */
            if (val < 0 || val > 2) {
                app_printk("Mode must be 0, 1 or 2.\r\n");
                on_entry_PARAMS_MENU();
                return ST_PARAMS_MENU;
            }
            p->console_input_mode = (uint8_t)val;
            console_input_mode_set((console_input_mode_t)val);
            app_printk("Console input now: %s\r\n",
                       console_input_mode_str((console_input_mode_t)val));
            break;
        default: break;
        }
        app_printk("Value updated (not yet saved).\r\n");
        on_entry_PARAMS_MENU();
        return ST_PARAMS_MENU;
    }

    /* ---- Recovery hold state ---- */
    if (state==ST_RECOVERY){
        if(strcmp(line,"resume")==0){ return ST_DEPLOYED; }
        app_printk("Unknown.\r\n"); return ST_RECOVERY;
    }

    return state;
}
