#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include <zephyr/kernel.h>

/* State IDs */
typedef enum {
    ST_POWERUP_WAIT = 0,
    ST_MENU,
    ST_HWTEST_MENU,
    ST_PARAMS_MENU,
    ST_PARAM_INPUT,
    ST_PR_MENU,
    ST_PR_INPUT,
    ST_PUMP_INPUT,
    ST_RECOVERY,
    ST_DEPLOYED,
    ST_SIMULATE,
    ST_COMPASS_MENU,
    ST__COUNT
} state_id_t;

/* Event IDs */
typedef enum {
    EVT_NONE = 0,
    EVT_TICK,
    EVT_TIMEOUT,
    EVT_ENTER,
    EVT_MENU_SELECT_1,
    EVT_MENU_SELECT_2,
    EVT_MENU_SELECT_3,
    EVT_CMD_RESUME,
} event_id_t;

typedef struct {
    event_id_t id;
} event_t;

#endif /* APP_EVENTS_H */