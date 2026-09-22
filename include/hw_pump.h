#ifndef HW_PUMP_H
#define HW_PUMP_H

#include <zephyr/kernel.h>
#include <stdint.h>

int pump_init(void);
void pump_run(int dir, uint32_t duration_s);


int32_t pump_get_position_sec(void);
void pump_reset_position(void);

/* Force the dead-reckoned position to a known value. Used after a blind
 * full-stroke move (e.g. the recovery maneuver), where the pump is driven
 * against its end stop and the true position is known regardless of what
 * dead reckoning believed beforehand. */
void pump_set_position_sec(int32_t pos_s);

#endif /* HW_PUMP_H */