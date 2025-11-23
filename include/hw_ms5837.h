#ifndef HW_MS5837_H
#define HW_MS5837_H
#include <zephyr/kernel.h>
int ms5837_init(void);
void ms5837_stream_interactive(void);
int ms5837_read(double *temp_c, double *press_kpa);
#endif
