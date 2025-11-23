#ifndef HW_PUMP_H
#define HW_PUMP_H

#include <zephyr/kernel.h>
#include <stdint.h>

int pump_init(void);
void pump_run(int dir, uint32_t duration_s);


int32_t pump_get_position_sec(void);
void pump_reset_position(void);

#endif /* HW_PUMP_H */