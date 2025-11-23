#ifndef APP_PARAMS_H
#define APP_PARAMS_H

#include <stdint.h>

/* Application parameters stored in non-volatile memory. */
struct app_params {
    float   dive_depth_m;          /* meters */
    uint16_t dive_timeout_min;     /* minutes */
    uint16_t dive_pump_s;          /* seconds */
    uint16_t deploy_wait_s;        /* seconds to wait on surface before diving */
    uint16_t start_pump_s;         /* seconds */
    uint16_t climb_pump_s;         /* seconds */

    uint16_t start_pitch_s;        /* seconds */
    uint16_t surface_pitch_s;      /* seconds */
    uint16_t dive_pitch_s;         /* seconds */
    uint16_t climb_pitch_s;        /* seconds */

    uint16_t start_roll_s;         /* seconds */
    uint16_t max_roll_s;           /* seconds */
    uint16_t roll_time_s;          /* seconds */

    int16_t  desired_heading_deg;  /* degrees 0-359 */
};

int app_params_init(void);
int app_params_save(void);
void app_params_reset_defaults(void);

/* Access current parameters; pointer is valid for the lifetime of the app. */
struct app_params *app_params_get(void);

#endif /* APP_PARAMS_H */
