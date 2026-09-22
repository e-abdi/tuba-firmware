#ifndef HW_MOTORS_H
#define HW_MOTORS_H

#include <zephyr/kernel.h>
#include <stdint.h>

enum motor_id {
    MOTOR_ROLL = 0,
    MOTOR_PITCH = 1
};

int motors_init(void);
void motor_run(enum motor_id id, int dir, uint32_t duration_s);


int32_t motor_get_position_sec(enum motor_id id);
void motor_reset_position(enum motor_id id);

/* Force the dead-reckoned position to a known value — see the equivalent
 * note in hw_pump.h. The pitch limit switches make this exact for pitch. */
void motor_set_position_sec(enum motor_id id, int32_t pos_s);
void motors_reset_all_positions(void);

#endif /* HW_MOTORS_H */