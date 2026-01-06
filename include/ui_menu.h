#ifndef UI_MENU_H
#define UI_MENU_H

#include "app_events.h"

void on_entry_POWERUP_WAIT(void);
void on_exit_POWERUP_WAIT(void);
void on_entry_MENU(void);
void on_entry_HWTEST_MENU(void);
void on_entry_PARAMS_MENU(void);
void on_entry_PR_MENU(void);
void on_entry_RECOVERY(void);
void on_entry_DEPLOYED(void);
void on_entry_SIMULATE(void);
void on_entry_COMPASS_MENU(void);

state_id_t on_event_POWERUP_WAIT(const event_t *e);
state_id_t on_event_MENU(const event_t *e);
state_id_t on_event_HWTEST_MENU(const event_t *e);
state_id_t on_event_PARAMS_MENU(const event_t *e);
state_id_t on_event_PARAM_INPUT(const event_t *e);
state_id_t on_event_PR_MENU(const event_t *e);
state_id_t on_event_PR_INPUT(const event_t *e);
state_id_t on_event_PUMP_INPUT(const event_t *e);
state_id_t on_event_RECOVERY(const event_t *e);
state_id_t on_event_DEPLOYED(const event_t *e);
state_id_t on_event_SIMULATE(const event_t *e);
state_id_t on_event_COMPASS_MENU(const event_t *e);


/* line handler (needed by main.c) */
state_id_t ui_handle_line(state_id_t state, const char *line);

#endif /* UI_MENU_H */
