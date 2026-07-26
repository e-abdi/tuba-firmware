/*
 * app_events.h — State and event identifiers for the UI state machine
 *
 * The firmware is organized around one explicit state machine (see
 * tuba_state_machine.puml for the diagram and README.md for an overview).
 *
 * Two kinds of stimulus drive it:
 *   - complete input lines from either console (dispatched by
 *     ui_handle_line() in ui_menu.c), and
 *   - asynchronous events (timer ticks, timeouts, bare ENTER presses)
 *     delivered through a message queue and dispatched per state.
 *
 * The per-state entry/exit/event handlers live in ui_menu.c and are wired
 * into a dispatch table in main.c — adding a state means adding one enum
 * value here, its handlers in ui_menu.c, and one table row in main.c.
 */
#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include <zephyr/kernel.h>

/* State IDs */
typedef enum {
    ST_POWERUP_WAIT = 0,  /* boot countdown: ENTER -> menu, timeout -> deploy */
    ST_MENU,              /* top-level menu */
    ST_HWTEST_MENU,       /* hardware test menu */
    ST_SENSORS_MENU,      /* sensor tests (temperature, I2C scan, ...) */
    ST_TEMP_MENU,         /* temperature (TSYS01) self-test/stream menu */
    ST_PARAMS_MENU,       /* mission parameter list */
    ST_PARAM_INPUT,       /* waiting for a new value for one parameter */
    ST_PR_MENU,           /* pitch & roll motor test menu */
    ST_PR_INPUT,          /* waiting for a motor run duration */
    ST_PUMP_INPUT,        /* waiting for a pump run duration */
    ST_LIMIT_TEST,        /* (legacy) limit-switch display state */
    ST_RECOVERY,          /* error hold state; "resume" re-deploys */
    ST_DEPLOYED,          /* mission running (async worker thread) */
    ST_SIMULATE,          /* simulated mission running (async worker) */
    ST_COMPASS_MENU,      /* compass calibrate/stream menu */
    ST_OTA_MENU,          /* waiting for an OTA firmware URL */
    ST__COUNT
} state_id_t;

/* Event IDs (asynchronous stimuli; input lines are delivered separately) */
typedef enum {
    EVT_NONE = 0,
    EVT_TICK,     /* 50 ms UI timer tick (countdown display, polling) */
    EVT_TIMEOUT,  /* startup countdown expired -> auto-deploy */
    EVT_ENTER,    /* user pressed ENTER on an empty line */
} event_id_t;

typedef struct {
    event_id_t id;
} event_t;

#endif /* APP_EVENTS_H */
