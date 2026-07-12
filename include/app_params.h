/*
 * app_params.h — Mission parameters, persisted in flash (NVS/settings)
 *
 * One flat struct holds every user-adjustable parameter. It is loaded from
 * NVS at boot, edited through the PARAMETERS menu, and written back only
 * when the user selects "save" (so experiments are cheap to abandon).
 *
 * NOTE ON LAYOUT: the struct is stored as a raw blob. Changing its size or
 * field order invalidates the stored copy — the loader detects the size
 * mismatch and falls back to compiled-in defaults (a re-save is then
 * required). Add new fields at the end.
 */
#ifndef APP_PARAMS_H
#define APP_PARAMS_H

#include <stdint.h>

struct app_params {
    /* --- Dive profile --- */
    float    dive_depth_m;         /* target depth per dive cycle [m] */
    uint16_t dive_timeout_min;     /* abort dive and climb after this [min] */
    uint16_t dive_pump_s;          /* pump position for diving [s from zero] */
    uint16_t deploy_wait_s;        /* surface wait before the first dive [s] */
    uint16_t start_pump_s;         /* pump position at the surface [s] */
    uint16_t climb_pump_s;         /* pump position for climbing [s] */

    /* --- Pitch mass positions (seconds of motor travel from zero) --- */
    uint16_t start_pitch_s;        /* surface/neutral pitch position */
    uint16_t surface_pitch_s;      /* pitch used while surfaced */
    uint16_t dive_pitch_s;         /* nose-down position for diving */
    uint16_t climb_pitch_s;        /* nose-up position for climbing */

    /* --- Roll mass positions (heading control) --- */
    uint16_t start_roll_s;         /* neutral roll position */
    uint16_t max_roll_s;           /* max bank used for heading correction */
    uint16_t roll_time_s;          /* (reserved) roll actuation time */

    /* --- Navigation --- */
    int16_t  desired_heading_deg;  /* course to hold, 0-359 degrees true */

    /* --- Safety --- */
    uint16_t leak_threshold_mv;    /* leak ADC below this = leak detected */

    /* --- Console --- */
    uint8_t  console_input_mode;   /* console_input_mode_t: 0=both 1=UART 2=WiFi */
};

/* Load parameters from NVS (or defaults) and apply side effects
 * (e.g. console input mode). Call once at boot. */
int app_params_init(void);

/* Persist the current parameters to NVS. */
int app_params_save(void);

/* Restore compiled-in defaults (RAM only — not saved until "save"). */
void app_params_reset_defaults(void);

/* Access current parameters; pointer is valid for the lifetime of the app. */
struct app_params *app_params_get(void);

#endif /* APP_PARAMS_H */
