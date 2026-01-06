#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdlib.h>

#include "app_events.h"
#include "app_limits.h"
#include "app_params.h"
#include "ui_menu.h"
#include "hw_motors.h"
#include "hw_pump.h"
#include "hw_bmp180.h"
#include "hw_gps.h"
#include "hw_hmc6343.h"
#include "hw_gps.h"
#include "hw_hmc6343.h"
#include "deploy.h"

/* MS5837 external pressure */
void ms5837_stream_interactive(void);

/* Extern from main.c */
extern enum motor_id current_motor;
static int current_param_index = 0;

/* POWERUP countdown */
static int32_t remaining_sec = STARTUP_TIMEOUT_SEC;
static int32_t tick_counter = 0;  /* Counter to track seconds for EVT_TICK */

/* --- Entry functions --- */
void on_entry_POWERUP_WAIT(void){
    remaining_sec = STARTUP_TIMEOUT_SEC;
    app_printk("\r\n-- POWERUP --\r\n");
    app_printk("press ENTER within %d seconds or the glider will go to DEPLOYED\r\n", remaining_sec);
}
void on_exit_POWERUP_WAIT(void){ app_printk("\r\n"); }

void on_entry_MENU(void){
    app_printk("\r\n=== MENU ===\r\n");
    app_printk("1) parameters\r\n");
    app_printk("2) hardware test\r\n");
    app_printk("3) simulate\r\n");
    app_printk("4) deploy\r\n");
    app_printk("Select [1-4]: ");
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
    app_printk("s) Save parameters\r\n");
    app_printk("r) Reset defaults\r\n");
    app_printk("x) Back\r\n");
    app_printk("Select [1-9,a-e,s,r,x]: ");
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
    app_printk("x) back\r\n");
    app_printk("Select [1-7,x]: ");
}


void on_entry_COMPASS_MENU(void){
    app_printk("\r\n-- Compass (HMC6343) --\r\n");
    app_printk("1) Calibrate (enter/exit)\r\n");
    app_printk("2) Continuous heading (q to quit)\r\n");
    app_printk("x) back\r\n");
    app_printk("Select [1,2,x]: ");
}

void on_entry_PR_MENU(void){
    app_printk("\r\n-- Pitch & Roll --\r\n");
    app_printk("1) roll\r\n");
    app_printk("2) pitch\r\n");
    app_printk("x) back\r\n");
    app_printk("Select [1,2,x]: ");
}

void on_entry_RECOVERY(void){
    app_printk("\r\n-- RECOVERY state --\r\n");
    app_printk("Type 'resume' to go to DEPLOYED.\r\n");
}
void on_entry_DEPLOYED(void){
    app_printk("\r\n== DEPLOYED state ==\r\n");
    /* Run deploy asynchronously to keep UI and networking responsive */
    deploy_start_async();
}

void on_entry_SIMULATE(void){
    app_printk("\r\n== SIMULATE state (lab testing with simulated pressure) ==\r\n");
    /* Run simulate asynchronously to keep UI and networking responsive */
    simulate_start_async();
}

/* --- Event handlers (unchanged skeleton) --- */
state_id_t on_event_POWERUP_WAIT(const event_t *e){
    switch (e->id) {
    case EVT_TICK:
        /* Tick fires every 50ms (20 times/sec); only decrement every 20 ticks (1 second) */
        tick_counter++;
        if (tick_counter >= 20) {
            tick_counter = 0;
            if (remaining_sec > 0) remaining_sec--;
            /* Update display once per second */
            app_printk("\rpress ENTER within %d seconds or the glider will go to RECOVERY   ", remaining_sec);
        }
        return ST_POWERUP_WAIT;
    case EVT_ENTER:
        app_printk("\r\nENTER received → MENU\r\n");
        return ST_MENU;
    case EVT_TIMEOUT:
        app_printk("\r\nTimeout → DEPLOYED\r\n");
        return ST_DEPLOYED;
    default:
        return ST_POWERUP_WAIT;
    }
}
state_id_t on_event_MENU(const event_t *e){ return ST_MENU; }
state_id_t on_event_HWTEST_MENU(const event_t *e){ return ST_HWTEST_MENU; }
state_id_t on_event_PARAMS_MENU(const event_t *e){ return ST_PARAMS_MENU; }
state_id_t on_event_PARAM_INPUT(const event_t *e){ return ST_PARAM_INPUT; }
state_id_t on_event_PR_MENU(const event_t *e){ return ST_PR_MENU; }
state_id_t on_event_PR_INPUT(const event_t *e){ return ST_PR_INPUT; }
state_id_t on_event_PUMP_INPUT(const event_t *e){ return ST_PUMP_INPUT; }
state_id_t on_event_RECOVERY(const event_t *e){ return ST_RECOVERY; }
state_id_t on_event_DEPLOYED(const event_t *e){
    /* Check if deploy has completed (thread no longer running) */
    if (!deploy_is_running()) {
        return ST_MENU;
    }
    return ST_DEPLOYED;
}
state_id_t on_event_SIMULATE(const event_t *e){
    /* Check if simulate has completed (thread no longer running) */
    if (!simulate_is_running()) {
        return ST_MENU;
    }
    return ST_SIMULATE;
}

/* --- Line handler --- */
state_id_t ui_handle_line(state_id_t state, const char *line){
    /* Ignore empty lines */
    if (!line || line[0] == '\0') {
        return ST__COUNT;  /* No state change */
    }
    
    /* Check if deploy/simulate has completed (worker thread no longer running) */
    if (state == ST_DEPLOYED && !deploy_is_running()) {
        return ST_MENU;
    }
    if (state == ST_SIMULATE && !simulate_is_running()) {
        return ST_MENU;
    }
    
    if (state==ST_MENU){
        if(line[0]=='1') return ST_PARAMS_MENU;
        if(line[0]=='2') return ST_HWTEST_MENU;
        if(line[0]=='3') return ST_SIMULATE;
        if(line[0]=='4') {
            /* Check if deploy sensor is available before transitioning to DEPLOYED */
            if (!deploy_check_sensor_available()) {
                app_printk("[DEPLOY] ERROR: external pressure sensor not available\r\n");
                app_printk("[DEPLOY] Try option 3 (simulate) instead\r\n");
                on_entry_MENU();
                return ST_MENU;
            }
            return ST_DEPLOYED;
        }
        /* Invalid input - stay in same state, print error, no state entry call */
        app_printk("Invalid.\r\n");
        return ST_MENU;

    }
    if (state==ST_HWTEST_MENU){
        if(line[0]=='1') return ST_PR_MENU;
        if(line[0]=='2') { app_printk("[PUMP] Enter seconds [-10,10], q to quit\r\n> "); return ST_PUMP_INPUT; }
        if(line[0]=='3') {
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
        if(line[0]=='x' || line[0]=='X') { return ST_MENU; }
        app_printk("Invalid.\r\n");
        return ST_HWTEST_MENU;
    }

    if (state==ST_PR_MENU){
        if(line[0]=='1'){ current_motor=MOTOR_ROLL;  app_printk("[ROLL] Enter seconds [-10,10], q to quit\r\n> ");  return ST_PR_INPUT; }
        if(line[0]=='2'){ current_motor=MOTOR_PITCH; app_printk("[PITCH] Enter seconds [-10,10], q to quit\r\n> "); return ST_PR_INPUT; }
        if(line[0]=='x' || line[0]=='X'){ return ST_HWTEST_MENU; }
        app_printk("Invalid.\r\n");
        return ST_PR_MENU;
    }

    if (state==ST_PR_INPUT){
        if((line[0]=='q'||line[0]=='Q') && line[1]=='\0'){ return ST_PR_MENU; }
        char *endp=NULL; long val=strtol(line,&endp,10);
        if(endp==line||*endp!='\0'){ app_printk("Not a valid integer: '%s'\r\n> ", line); return ST_PR_INPUT; }
        if(val<TEST_MIN_SEC||val>TEST_MAX_SEC){ app_printk("Range -10..10 only\r\n> ");     return ST_PR_INPUT; }
        int dir=(val>=0)?+1:-1; uint32_t dur=(val>=0)?(uint32_t)val:(uint32_t)(-val);
        motor_run(current_motor,dir,dur); app_printk("> "); return ST_PR_INPUT;
    }

    if (state==ST_PUMP_INPUT){
        if((line[0]=='q'||line[0]=='Q') && line[1]=='\0'){ return ST_HWTEST_MENU; }
        char *endp=NULL; long val=strtol(line,&endp,10);
        if(endp==line||*endp!='\0'){ app_printk("Not a valid integer: '%s'\r\n> ", line); return ST_PUMP_INPUT; }
        if(val<TEST_MIN_SEC||val>TEST_MAX_SEC){ app_printk("Range -10..10 only\r\n> ");     return ST_PUMP_INPUT; }
        int dir=(val>=0)?+1:-1; uint32_t dur=(val>=0)?(uint32_t)val:(uint32_t)(-val);
        pump_run(dir,dur); app_printk("> "); return ST_PUMP_INPUT;
    }

    if (state==ST_COMPASS_MENU){
        if(line[0]=='1') { hmc6343_user_calibrate_interactive(); on_entry_COMPASS_MENU(); return ST_COMPASS_MENU; }
        if(line[0]=='2') { hmc6343_stream_heading_interactive(); on_entry_COMPASS_MENU(); return ST_COMPASS_MENU; }
        if(line[0]=='x' || line[0]=='X') { return ST_HWTEST_MENU; }
        app_printk("Invalid.\r\n");
        return ST_COMPASS_MENU;
    }

    if (state==ST_PARAMS_MENU){
        /* navigation */
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
        app_printk("Invalid.\r\n");
        return ST_PARAMS_MENU;
    }

    if (state==ST_PARAM_INPUT){
        struct app_params *p = app_params_get();
        char *endp = NULL;
        long val = strtol(line,&endp,10);
        if(endp==line||*endp!='\0'){ app_printk("Not a valid integer: '%s'\r\n", line); return ST_PARAMS_MENU; }
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
    default: break;
    }
        app_printk("Value updated (not yet saved).\r\n");
        on_entry_PARAMS_MENU();
        return ST_PARAMS_MENU;
    }

    if (state==ST_RECOVERY){
        if(strcmp(line,"resume")==0){ on_entry_DEPLOYED(); return ST_DEPLOYED; }
        app_printk("Unknown.\r\n"); return ST_RECOVERY;
    }

    return state;
}