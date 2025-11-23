#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "app_params.h"
#include "app_print.h"
#include "hw_ms5837.h"
#include "hw_bmp180.h"
#include "hw_hmc6343.h"
#include "hw_motors.h"
#include "hw_pump.h"

/* Physical constants */
#define SEA_WATER_DENSITY_KG_M3 1025.0
#define GRAVITY_M_S2 9.80665

void deploy_start(void)
{
    struct app_params *p = app_params_get();

    app_printk("[DEPLOY] starting sequence\r\n");

    /* 1) Take reading from external pressure sensor to use as surface reference */
    double temp_c = 0.0, press_kpa = 0.0;
    double surface_pa = 0.0;
    bool have_surface = false;
    if (ms5837_read(&temp_c, &press_kpa) == 0) {
        surface_pa = press_kpa * 1000.0; /* kPa -> Pa */
        have_surface = true;
        app_printk("[DEPLOY] surface external pressure: %.3f kPa (T=%.2f C)\r\n", press_kpa, temp_c);
    } else {
        app_printk("[DEPLOY] WARNING: could not read external pressure; will use timeout fallback for dive depth\r\n");
    }

    /* Record starting positions of motors and pump */
    float start_pitch_pos_s = motor_get_position_sec(MOTOR_PITCH);
    float start_roll_pos_s = motor_get_position_sec(MOTOR_ROLL);
    float start_pump_pos_s = pump_get_position_sec();
    app_printk("[DEPLOY] starting positions: pitch=%.1fs, roll=%.1fs, pump=%.1fs\r\n",
               start_pitch_pos_s, start_roll_pos_s, start_pump_pos_s);

    /* 2) Move to surface position (start_pitch and start_pump) */
    /* Compute relative movement needed to reach surface setpoints */
    float pitch_delta = (float)p->start_pitch_s - start_pitch_pos_s;
    float pump_delta = (float)p->start_pump_s - start_pump_pos_s;
    
    app_printk("[DEPLOY] moving to surface position: pitch target=%us (delta=%.1fs), pump target=%us (delta=%.1fs)\r\n",
               p->start_pitch_s, pitch_delta, p->start_pump_s, pump_delta);
    
    if (pitch_delta > 0.5f) {
        motor_run(MOTOR_PITCH, +1, (uint32_t)(pitch_delta + 0.5f));
    } else if (pitch_delta < -0.5f) {
        motor_run(MOTOR_PITCH, -1, (uint32_t)(-pitch_delta + 0.5f));
    }
    
    if (pump_delta > 0.5f) {
        pump_run(+1, (uint32_t)(pump_delta + 0.5f));
    } else if (pump_delta < -0.5f) {
        pump_run(-1, (uint32_t)(-pump_delta + 0.5f));
    }

    /* 3) Wait for deploy_wait_s seconds (default 10) */
    uint32_t wait_s = (uint32_t)p->deploy_wait_s;
    app_printk("[DEPLOY] waiting %us on surface before diving\r\n", wait_s);
    for (uint32_t i = 0; i < wait_s; ++i) {
        k_sleep(K_SECONDS(1));
    }

    /* 4) Start dive sequence: move pitch and pump to the absolute dive targets */
    /* Compute movement needed to reach absolute dive targets (seconds positions) */
    {
        float cur_pitch = motor_get_position_sec(MOTOR_PITCH);
        float cur_pump = pump_get_position_sec();
        float pitch_to_dive = (float)p->dive_pitch_s - cur_pitch;
        float pump_to_dive = (float)p->dive_pump_s - cur_pump;
        app_printk("[DEPLOY] moving to dive targets: pitch=%us (delta=%.1fs), pump=%us (delta=%.1fs)\r\n",
                   p->dive_pitch_s, pitch_to_dive, p->dive_pump_s, pump_to_dive);
        if (pitch_to_dive > 0.5f) {
            motor_run(MOTOR_PITCH, +1, (uint32_t)(pitch_to_dive + 0.5f));
        } else if (pitch_to_dive < -0.5f) {
            motor_run(MOTOR_PITCH, -1, (uint32_t)(-pitch_to_dive + 0.5f));
        }
        if (pump_to_dive > 0.5f) {
            pump_run(+1, (uint32_t)(pump_to_dive + 0.5f));
        } else if (pump_to_dive < -0.5f) {
            pump_run(-1, (uint32_t)(-pump_to_dive + 0.5f));
        }
    }

    /* 5) Monitor sensors every second until we reach target depth */
    app_printk("[DEPLOY] monitoring sensors while diving to %.1fm\r\n", p->dive_depth_m);
    uint64_t deadline_ms = 0;
    if (!have_surface) {
        /* Fallback: limit dive by dive_timeout_min (minutes) if no surface reference */
        uint32_t mins = p->dive_timeout_min ? p->dive_timeout_min : 1;
        deadline_ms = k_uptime_get() + (uint64_t)mins * 60ULL * 1000ULL;
        app_printk("[DEPLOY] no surface ref - will climb after %u minutes if depth not reached\r\n", mins);
    }

    while (1) {
        /* Read internal pressure (Pa) */
        int32_t internal_pa = 0;
        if (bmp180_read_pa(&internal_pa) != 0) {
            app_printk("[DEPLOY] Internal pressure read failed\r\n");
        }

        /* Read external pressure */
        if (ms5837_read(&temp_c, &press_kpa) != 0) {
            app_printk("[DEPLOY] External pressure read failed\r\n");
        }
        double external_pa = press_kpa * 1000.0;

        /* Convert external pressure to depth relative to surface reference */
        double depth_m = 0.0;
        if (surface_pa > 0.0) {
            depth_m = (external_pa - surface_pa) / (SEA_WATER_DENSITY_KG_M3 * GRAVITY_M_S2);
            if (depth_m < 0.0) depth_m = 0.0;
        }

        /* Read compass (heading/pitch/roll) */
        float head=0.0f, pitch=0.0f, roll=0.0f;
        if (hmc6343_read(&head, &pitch, &roll) != 0) {
            app_printk("[DEPLOY] Compass read failed\r\n");
        }

        /* Print summary */
        app_printk("[SENS] IntP=%d Pa, ExtDepth=%.2fm, H=%.1f,R=%.1f,P=%.1f\r\n",
                   internal_pa, depth_m, head, roll, pitch);

        /* Check if we've reached the requested dive depth */
        if (have_surface) {
            if (depth_m >= (double)p->dive_depth_m) {
                app_printk("[DEPLOY] target depth reached (%.2fm) -> start climb\r\n", depth_m);
                break;
            }
        } else {
            if ((int64_t)k_uptime_get() >= (int64_t)deadline_ms) {
                app_printk("[DEPLOY] deadline reached -> start climb\r\n");
                break;
            }
        }

        k_sleep(K_SECONDS(1));
    }

    /* 5) Start climb sequence: move pitch and pump to the absolute climb targets */
    {
        float cur_pitch = motor_get_position_sec(MOTOR_PITCH);
        float cur_pump = pump_get_position_sec();
        float pitch_to_climb = (float)p->climb_pitch_s - cur_pitch;
        float pump_to_climb = (float)p->climb_pump_s - cur_pump;
        app_printk("[DEPLOY] moving to climb targets: pitch=%us (delta=%.1fs), pump=%us (delta=%.1fs)\r\n",
                   p->climb_pitch_s, pitch_to_climb, p->climb_pump_s, pump_to_climb);
        if (pitch_to_climb > 0.5f) {
            motor_run(MOTOR_PITCH, +1, (uint32_t)(pitch_to_climb + 0.5f));
        } else if (pitch_to_climb < -0.5f) {
            motor_run(MOTOR_PITCH, -1, (uint32_t)(-pitch_to_climb + 0.5f));
        }
        if (pump_to_climb > 0.5f) {
            pump_run(+1, (uint32_t)(pump_to_climb + 0.5f));
        } else if (pump_to_climb < -0.5f) {
            pump_run(-1, (uint32_t)(-pump_to_climb + 0.5f));
        }
    }

    /* 6) Continue reading & displaying sensors and monitor depth; when depth < 1m, move to surface position */
    bool surface_reached = false;
    for (int i=0;i<60;i++) {  /* Allow up to 60 seconds for climb monitoring */
        int32_t internal_pa = 0; bmp180_read_pa(&internal_pa);
        ms5837_read(&temp_c,&press_kpa);
        double external_pa = press_kpa * 1000.0;
        double depth_m = (surface_pa>0.0)?((external_pa - surface_pa) / (SEA_WATER_DENSITY_KG_M3 * GRAVITY_M_S2)):0.0;
        if (depth_m < 0.0) depth_m = 0.0;
        
        float head=0.0f, pitch=0.0f, roll=0.0f; hmc6343_read(&head,&pitch,&roll);
        app_printk("[SENS] IntP=%d Pa, ExtDepth=%.2fm, H=%.1f,R=%.1f,P=%.1f\r\n",
                   internal_pa, depth_m, head, roll, pitch);
        
        /* When depth drops below 1m, move motors to surface position */
        if (!surface_reached && depth_m < 1.0) {
            surface_reached = true;
            app_printk("[DEPLOY] depth < 1m reached; moving to surface position\r\n");
            
            /* Move pitch to start_pitch position */
            float current_pitch = motor_get_position_sec(MOTOR_PITCH);
            float pitch_to_surface = (float)p->start_pitch_s - current_pitch;
            if (pitch_to_surface > 0.5f) {
                motor_run(MOTOR_PITCH, +1, (uint32_t)(pitch_to_surface + 0.5f));
            } else if (pitch_to_surface < -0.5f) {
                motor_run(MOTOR_PITCH, -1, (uint32_t)(-pitch_to_surface + 0.5f));
            }
            
            /* Move pump to start_pump position */
            float current_pump = pump_get_position_sec();
            float pump_to_surface = (float)p->start_pump_s - current_pump;
            if (pump_to_surface > 0.5f) {
                pump_run(+1, (uint32_t)(pump_to_surface + 0.5f));
            } else if (pump_to_surface < -0.5f) {
                pump_run(-1, (uint32_t)(-pump_to_surface + 0.5f));
            }
            
            /* Continue monitoring for a bit after reaching surface */
            for (int j=0; j<5; j++) {
                k_sleep(K_SECONDS(1));
                bmp180_read_pa(&internal_pa);
                ms5837_read(&temp_c,&press_kpa);
                external_pa = press_kpa * 1000.0;
                depth_m = (surface_pa>0.0)?((external_pa - surface_pa) / (SEA_WATER_DENSITY_KG_M3 * GRAVITY_M_S2)):0.0;
                hmc6343_read(&head,&pitch,&roll);
                app_printk("[SENS] IntP=%d Pa, ExtDepth=%.2fm, H=%.1f,R=%.1f,P=%.1f\r\n",
                           internal_pa, depth_m, head, roll, pitch);
            }
            break;
        }
        
        k_sleep(K_SECONDS(1));
    }

    app_printk("[DEPLOY] sequence complete\r\n");
}
