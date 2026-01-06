#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>
#include <stdbool.h>

#include "app_params.h"
#include "app_print.h"
#include "hw_ms5837.h"
#include "hw_bmp180.h"
#include "hw_hmc6343.h"
#include "hw_motors.h"
#include "hw_pump.h"
#include "hw_gps.h"
#include "net_console.h"

/* Physical constants */
#define SEA_WATER_DENSITY_KG_M3 1025.0
#define GRAVITY_M_S2 9.80665

/* Flag to signal that deploy/simulate failed and should return to menu */
static atomic_t return_to_menu_flag = ATOMIC_INIT(0);

/* Check if external pressure sensor is available */
bool deploy_check_sensor_available(void)
{
    double temp_c = 0.0, press_kpa = 0.0;
    return (ms5837_read(&temp_c, &press_kpa) == 0);
}

/* Single dive/climb cycle */
static void deploy_dive_cycle(struct app_params *p, double surface_pa)
{
    /* Move to surface position (start_pitch and start_pump) */
    float start_pitch_pos_s = motor_get_position_sec(MOTOR_PITCH);
    float start_pump_pos_s = pump_get_position_sec();
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

    /* Dive sequence: move pitch and pump to the absolute dive targets */
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

    /* Monitor sensors while diving to target depth */
    app_printk("[DEPLOY] monitoring sensors while diving to %.1fm\r\n", p->dive_depth_m);
    double temp_c = 0.0, press_kpa = 0.0;
    uint64_t deadline_ms = k_uptime_get() + (uint64_t)p->dive_timeout_min * 60ULL * 1000ULL;

    while (1) {
        int32_t internal_pa = 0;
        if (bmp180_read_pa(&internal_pa) != 0) {
            app_printk("[DEPLOY] Internal pressure read failed\r\n");
        }

        if (ms5837_read(&temp_c, &press_kpa) != 0) {
            app_printk("[DEPLOY] External pressure read failed\r\n");
        }
        double external_pa = press_kpa * 1000.0;
        double depth_m = (surface_pa > 0.0) ? ((external_pa - surface_pa) / (SEA_WATER_DENSITY_KG_M3 * GRAVITY_M_S2)) : 0.0;
        if (depth_m < 0.0) depth_m = 0.0;

        float head=0.0f, pitch=0.0f, roll=0.0f;
        if (hmc6343_read(&head, &pitch, &roll) != 0) {
            app_printk("[DEPLOY] Compass read failed\r\n");
        }

        app_printk("[SENS] IntP=%d Pa, ExtDepth=%.2fm, H=%.1f,R=%.1f,P=%.1f\r\n",
                   internal_pa, depth_m, head, roll, pitch);

        if (depth_m >= (double)p->dive_depth_m) {
            app_printk("[DEPLOY] target depth reached (%.2fm) -> start climb\r\n", depth_m);
            break;
        }
        
        if ((int64_t)k_uptime_get() >= (int64_t)deadline_ms) {
            app_printk("[DEPLOY] dive timeout -> start climb\r\n");
            break;
        }

        k_sleep(K_SECONDS(1));
    }

    /* Start climb sequence */
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

    /* Monitor climb until surface */
    bool surface_reached = false;
    for (int i=0;i<60;i++) {  /* Allow up to 60 seconds for climb monitoring */
        int32_t internal_pa = 0;
        bmp180_read_pa(&internal_pa);
        ms5837_read(&temp_c,&press_kpa);
        double external_pa = press_kpa * 1000.0;
        double depth_m = (surface_pa>0.0)?((external_pa - surface_pa) / (SEA_WATER_DENSITY_KG_M3 * GRAVITY_M_S2)):0.0;
        if (depth_m < 0.0) depth_m = 0.0;
        
        float head=0.0f, pitch=0.0f, roll=0.0f;
        hmc6343_read(&head,&pitch,&roll);
        app_printk("[SENS] IntP=%d Pa, ExtDepth=%.2fm, H=%.1f,R=%.1f,P=%.1f\r\n",
                   internal_pa, depth_m, head, roll, pitch);
        
        if (!surface_reached && depth_m < 1.0) {
            surface_reached = true;
            app_printk("[DEPLOY] depth < 1m reached; moving to surface position\r\n");
            
            float current_pitch = motor_get_position_sec(MOTOR_PITCH);
            float pitch_to_surface = (float)p->start_pitch_s - current_pitch;
            if (pitch_to_surface > 0.5f) {
                motor_run(MOTOR_PITCH, +1, (uint32_t)(pitch_to_surface + 0.5f));
            } else if (pitch_to_surface < -0.5f) {
                motor_run(MOTOR_PITCH, -1, (uint32_t)(-pitch_to_surface + 0.5f));
            }
            
            float current_pump = pump_get_position_sec();
            float pump_to_surface = (float)p->start_pump_s - current_pump;
            if (pump_to_surface > 0.5f) {
                pump_run(+1, (uint32_t)(pump_to_surface + 0.5f));
            } else if (pump_to_surface < -0.5f) {
                pump_run(-1, (uint32_t)(-pump_to_surface + 0.5f));
            }
            
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
}

void deploy_start(void)
{
    struct app_params *p = app_params_get();

    app_printk("[DEPLOY] starting sequence\r\n");

    /* 1) Take reading from external pressure sensor to use as surface reference */
    double temp_c = 0.0, press_kpa = 0.0;
    double surface_pa = 0.0;
    if (ms5837_read(&temp_c, &press_kpa) != 0) {
        app_printk("[DEPLOY] ERROR: cannot read external pressure sensor (MS5837)\r\n");
        app_printk("[DEPLOY] Try 'simulate' instead to test with simulated pressure\r\n");
        atomic_set(&return_to_menu_flag, 1);
        return;
    }
    surface_pa = press_kpa * 1000.0; /* kPa -> Pa */
    app_printk("[DEPLOY] surface external pressure: %.3f kPa (T=%.2f C)\r\n", press_kpa, temp_c);

    /* Record starting positions */
    float start_pitch_pos_s = motor_get_position_sec(MOTOR_PITCH);
    float start_roll_pos_s = motor_get_position_sec(MOTOR_ROLL);
    float start_pump_pos_s = pump_get_position_sec();
    app_printk("[DEPLOY] starting positions: pitch=%.1fs, roll=%.1fs, pump=%.1fs\r\n",
               start_pitch_pos_s, start_roll_pos_s, start_pump_pos_s);

    /* 2) Wait before first dive */
    uint32_t wait_s = (uint32_t)p->deploy_wait_s;
    app_printk("[DEPLOY] waiting %us before first dive\r\n", wait_s);
    for (uint32_t i = 0; i < wait_s; ++i) {
        k_sleep(K_SECONDS(1));
    }

    /* 3) Acquire GPS fix before dive */
    app_printk("[DEPLOY] acquiring GPS fix before dive\r\n");
    gps_fix_wait(30);  /* 30 second timeout */

    /* 4) Main dive/climb loop */
    while (1) {
        /* Perform dive and climb cycle */
        deploy_dive_cycle(p, surface_pa);

        /* 5) After climb, acquire another GPS fix */
        app_printk("[DEPLOY] acquired surface position, getting GPS fix\r\n");
        gps_fix_wait(30);  /* 30 second timeout */

        /* 6) Wait 10 seconds for user to press ENTER to stop, else auto-restart dive */
        app_printk("[DEPLOY] press ENTER within 10 seconds to stop, or will start another dive...\r\n");
        int64_t wait_start = k_uptime_get();
        bool enter_pressed = false;
        
        while (k_uptime_get() - wait_start < 10000) {  /* 10 second timeout */
            char line[128];
            if (net_console_poll_line(line, sizeof(line), K_MSEC(500))) {
                /* User pressed ENTER on net console */
                if (line[0] == '\0' || line[0] == '\r' || line[0] == '\n') {
                    enter_pressed = true;
                    break;
                }
            }
            k_sleep(K_MSEC(100));
        }

        if (enter_pressed) {
            app_printk("[DEPLOY] user requested stop\r\n");
            break;
        }

        app_printk("[DEPLOY] no user input, starting another dive cycle\r\n");
    }

    app_printk("[DEPLOY] deployment complete, returning to menu\r\n");
}

/* --- Async deploy worker --- */
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

/* --- Simulate deployment (with simulated external pressure) --- */
/* Single simulate dive/climb cycle with simulated depth */
static void simulate_dive_cycle(struct app_params *p, double surface_pa)
{
    /* Move to surface position */
    float start_pitch_pos_s = motor_get_position_sec(MOTOR_PITCH);
    float start_pump_pos_s = pump_get_position_sec();
    float pitch_delta = (float)p->start_pitch_s - start_pitch_pos_s;
    float pump_delta = (float)p->start_pump_s - start_pump_pos_s;
    
    app_printk("[SIMULATE] moving to surface position: pitch target=%us, pump target=%us\r\n",
               p->start_pitch_s, p->start_pump_s);
    
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

    /* Dive sequence */
    {
        float cur_pitch = motor_get_position_sec(MOTOR_PITCH);
        float cur_pump = pump_get_position_sec();
        float pitch_to_dive = (float)p->dive_pitch_s - cur_pitch;
        float pump_to_dive = (float)p->dive_pump_s - cur_pump;
        app_printk("[SIMULATE] moving to dive targets: pitch=%us, pump=%us\r\n",
                   p->dive_pitch_s, p->dive_pump_s);
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

    /* Simulate dive to target depth at 50 cm/s */
    app_printk("[SIMULATE] diving to %.1fm (simulated pressure at 50cm/s)\r\n", p->dive_depth_m);
    uint64_t dive_start_ms = k_uptime_get();
    
    while (1) {
        uint64_t elapsed_ms = k_uptime_get() - dive_start_ms;
        double elapsed_s = (double)elapsed_ms / 1000.0;
        double simulated_depth_m = 0.5 * elapsed_s;  /* 50 cm/s = 0.5 m/s */
        
        int32_t internal_pa = 0;
        bmp180_read_pa(&internal_pa);
        
        float head=0.0f, pitch=0.0f, roll=0.0f;
        hmc6343_read(&head, &pitch, &roll);

        app_printk("[SENS] IntP=%d Pa, SimDepth=%.2fm, H=%.1f,R=%.1f,P=%.1f\r\n",
                   internal_pa, simulated_depth_m, head, roll, pitch);

        if (simulated_depth_m >= (double)p->dive_depth_m) {
            app_printk("[SIMULATE] target depth reached (%.2fm) -> start climb\r\n", simulated_depth_m);
            break;
        }

        k_sleep(K_SECONDS(1));
    }

    /* Climb sequence */
    {
        float cur_pitch = motor_get_position_sec(MOTOR_PITCH);
        float cur_pump = pump_get_position_sec();
        float pitch_to_climb = (float)p->climb_pitch_s - cur_pitch;
        float pump_to_climb = (float)p->climb_pump_s - cur_pump;
        app_printk("[SIMULATE] moving to climb targets: pitch=%us, pump=%us\r\n",
                   p->climb_pitch_s, p->climb_pump_s);
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

    /* Simulate climb back to surface */
    bool surface_reached = false;
    for (int i=0;i<60;i++) {
        uint64_t elapsed_ms = k_uptime_get() - dive_start_ms;
        double elapsed_s = (double)elapsed_ms / 1000.0;
        double simulated_depth_m = 0.5 * elapsed_s;
        
        if (simulated_depth_m > (double)p->dive_depth_m) {
            simulated_depth_m = (double)p->dive_depth_m - 0.5 * (simulated_depth_m - (double)p->dive_depth_m);
            if (simulated_depth_m < 0.0) simulated_depth_m = 0.0;
        }

        int32_t internal_pa = 0;
        bmp180_read_pa(&internal_pa);
        
        float head=0.0f, pitch=0.0f, roll=0.0f;
        hmc6343_read(&head,&pitch,&roll);
        app_printk("[SENS] IntP=%d Pa, SimDepth=%.2fm, H=%.1f,R=%.1f,P=%.1f\r\n",
                   internal_pa, simulated_depth_m, head, roll, pitch);
        
        if (!surface_reached && simulated_depth_m < 1.0) {
            surface_reached = true;
            app_printk("[SIMULATE] depth < 1m reached; moving to surface position\r\n");
            
            float current_pitch = motor_get_position_sec(MOTOR_PITCH);
            float pitch_to_surface = (float)p->start_pitch_s - current_pitch;
            if (pitch_to_surface > 0.5f) {
                motor_run(MOTOR_PITCH, +1, (uint32_t)(pitch_to_surface + 0.5f));
            } else if (pitch_to_surface < -0.5f) {
                motor_run(MOTOR_PITCH, -1, (uint32_t)(-pitch_to_surface + 0.5f));
            }
            
            float current_pump = pump_get_position_sec();
            float pump_to_surface = (float)p->start_pump_s - current_pump;
            if (pump_to_surface > 0.5f) {
                pump_run(+1, (uint32_t)(pump_to_surface + 0.5f));
            } else if (pump_to_surface < -0.5f) {
                pump_run(-1, (uint32_t)(-pump_to_surface + 0.5f));
            }
            
            for (int j=0; j<5; j++) {
                k_sleep(K_SECONDS(1));
                bmp180_read_pa(&internal_pa);
                hmc6343_read(&head,&pitch,&roll);
                app_printk("[SENS] IntP=%d Pa, SimDepth=0.00m, H=%.1f,R=%.1f,P=%.1f\r\n",
                           internal_pa, head, roll, pitch);
            }
            break;
        }
        
        k_sleep(K_SECONDS(1));
    }
}

void simulate_start(void)
{
    struct app_params *p = app_params_get();

    app_printk("[SIMULATE] starting simulation sequence (pressure sensor simulated)\r\n");

    double surface_pa = 101325.0;  /* Sea level reference */
    app_printk("[SIMULATE] simulated surface pressure: %.3f kPa\r\n", surface_pa / 1000.0);

    float start_pitch_pos_s = motor_get_position_sec(MOTOR_PITCH);
    float start_roll_pos_s = motor_get_position_sec(MOTOR_ROLL);
    float start_pump_pos_s = pump_get_position_sec();
    app_printk("[SIMULATE] starting positions: pitch=%.1fs, roll=%.1fs, pump=%.1fs\r\n",
               start_pitch_pos_s, start_roll_pos_s, start_pump_pos_s);

    /* Initial wait */
    uint32_t wait_s = (uint32_t)p->deploy_wait_s;
    app_printk("[SIMULATE] waiting %us before first dive\r\n", wait_s);
    for (uint32_t i = 0; i < wait_s; ++i) {
        k_sleep(K_SECONDS(1));
    }

    /* Acquire simulated GPS fix before dive */
    app_printk("[SIMULATE] acquiring simulated GPS fix before dive\r\n");
    k_sleep(K_SECONDS(2));
    app_printk("[GPS] acquired (simulated)\r\n");

    /* Main dive/climb loop */
    while (1) {
        simulate_dive_cycle(p, surface_pa);

        /* After climb, get simulated GPS fix */
        app_printk("[SIMULATE] acquired surface position, getting simulated GPS fix\r\n");
        k_sleep(K_SECONDS(2));
        app_printk("[GPS] acquired (simulated)\r\n");

        /* Wait 10 seconds for user to press ENTER to stop, else auto-restart dive */
        app_printk("[SIMULATE] press ENTER within 10 seconds to stop, or will start another dive...\r\n");
        int64_t wait_start = k_uptime_get();
        bool enter_pressed = false;
        
        while (k_uptime_get() - wait_start < 10000) {  /* 10 second timeout */
            char line[128];
            if (net_console_poll_line(line, sizeof(line), K_MSEC(500))) {
                if (line[0] == '\0' || line[0] == '\r' || line[0] == '\n') {
                    enter_pressed = true;
                    break;
                }
            }
            k_sleep(K_MSEC(100));
        }

        if (enter_pressed) {
            app_printk("[SIMULATE] user requested stop\r\n");
            break;
        }

        app_printk("[SIMULATE] no user input, starting another dive cycle\r\n");
    }

    app_printk("[SIMULATE] simulation complete, returning to menu\r\n");
}

/* --- Async simulate worker --- */
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

/* Check if deploy is currently running */
bool deploy_is_running(void)
{
    return atomic_get(&deploy_running) != 0;
}

/* Check if simulate is currently running */
bool simulate_is_running(void)
{
    return atomic_get(&simulate_running) != 0;
}
