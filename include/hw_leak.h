#ifndef HW_LEAK_H
#define HW_LEAK_H

#include <stdint.h>

int  leak_detector_init(void);
int  leak_detector_read_mv(int32_t *voltage_mv);
void leak_detector_stream_interactive(uint32_t threshold_mv);

#endif /* HW_LEAK_H */
