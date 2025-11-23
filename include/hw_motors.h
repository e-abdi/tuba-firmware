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
void motors_reset_all_positions(void);

#endif /* HW_MOTORS_H */