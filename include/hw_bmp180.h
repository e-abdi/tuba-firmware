#ifndef HW_BMP180_H
#define HW_BMP180_H

#include <zephyr/kernel.h>

int bmp180_init(void);
/* Blocks: prints 1 Hz readings until user types 'q' on the console */
void bmp180_stream_interactive(void);
/* Read a single compensated pressure sample (Pa) */
int bmp180_read_pa(int32_t *out_pa);

#endif /* HW_BMP180_H */
