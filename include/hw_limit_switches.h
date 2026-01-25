#ifndef HW_LIMIT_SWITCHES_H
#define HW_LIMIT_SWITCHES_H

#include <zephyr/kernel.h>

/* Limit switch IDs */
#define LIMIT_PITCH_UP   0
#define LIMIT_PITCH_DOWN 1

/**
 * Initialize pitch limit switches on GPIO32 and GPIO33.
 * Switches are monitored via GPIO interrupts and trigger motor stop.
 * GPIO32/33 are input-only pins and safe from boot strapping conflicts.
 *
 * @return 0 on success, negative errno on failure
 */
int limit_switches_init(void);

/**
 * Check if a pitch limit switch is currently triggered.
 * Useful for manual polling or diagnostics.
 *
 * @param switch_id LIMIT_PITCH_UP or LIMIT_PITCH_DOWN
 * @return true if switch is pressed (active low), false otherwise
 */
bool limit_switch_is_pressed(int switch_id);

/**
 * Callback invoked when a limit switch is triggered.
 * This stops the pitch motor immediately.
 * (Internal use; set up automatically during init.)
 */
void limit_switch_callback(int switch_id);

/**
 * Check triggered switches and perform motor stop (SAFE FOR MAIN LOOP).
 * This function should be called periodically from the main event loop,
 * NOT from an ISR. It handles the actual motor stop action.
 */
void limit_switches_check_and_stop(void);

/**
 * Interactive test loop for limit switches.
 * Continuously displays GPIO32/33 status and exits when user presses 'q'.
 * Call from menu for real-time testing.
 */
void limit_switches_test_interactive(void);

#endif /* HW_LIMIT_SWITCHES_H */
