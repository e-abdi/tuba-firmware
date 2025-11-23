
#ifndef HW_HMC6343_H
#define HW_HMC6343_H
#include <zephyr/kernel.h>
void hmc6343_user_calibrate_interactive(void);
void hmc6343_stream_heading_interactive(void);
/* Non-interactive single-sample read of heading/pitch/roll in degrees. */
int hmc6343_read(float *heading_deg, float *pitch_deg, float *roll_deg);
#endif
