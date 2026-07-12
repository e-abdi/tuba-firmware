/*
 * ui_menu.h — Menu system: per-state entry/event handlers + line dispatch
 *
 * Each UI state has:
 *   on_entry_<STATE>() — prints the state's menu/prompt (optional)
 *   on_event_<STATE>() — reacts to async events (ticks, timeouts, ENTER)
 *
 * and all typed input lines are routed through ui_handle_line(), which
 * returns the next state (or ST__COUNT for "no change"). main.c owns the
 * dispatch table that binds these handlers to state IDs.
 */
#ifndef UI_MENU_H
#define UI_MENU_H

#include "app_events.h"

/* --- State entry hooks (print menus / start workers) --- */
void on_entry_POWERUP_WAIT(void);
void on_exit_POWERUP_WAIT(void);
void on_entry_MENU(void);
void on_entry_HWTEST_MENU(void);
void on_entry_SENSORS_MENU(void);
void on_entry_PARAMS_MENU(void);
void on_entry_PR_MENU(void);
void on_entry_RECOVERY(void);
void on_entry_DEPLOYED(void);
void on_entry_SIMULATE(void);
void on_entry_COMPASS_MENU(void);
void on_entry_OTA_MENU(void);

/* --- Async event handlers (return the next state) --- */
state_id_t on_event_POWERUP_WAIT(const event_t *e);
state_id_t on_event_MENU(const event_t *e);
state_id_t on_event_HWTEST_MENU(const event_t *e);
state_id_t on_event_SENSORS_MENU(const event_t *e);
state_id_t on_event_PARAMS_MENU(const event_t *e);
state_id_t on_event_PARAM_INPUT(const event_t *e);
state_id_t on_event_PR_MENU(const event_t *e);
state_id_t on_event_PR_INPUT(const event_t *e);
state_id_t on_event_PUMP_INPUT(const event_t *e);
state_id_t on_event_RECOVERY(const event_t *e);
state_id_t on_event_DEPLOYED(const event_t *e);
state_id_t on_event_SIMULATE(const event_t *e);
state_id_t on_event_COMPASS_MENU(const event_t *e);
state_id_t on_event_OTA_MENU(const event_t *e);

/* Typed-line dispatcher (called from the main loop for every input line).
 * Returns the next state, or ST__COUNT for "stay in the current state". */
state_id_t ui_handle_line(state_id_t state, const char *line);

#endif /* UI_MENU_H */
