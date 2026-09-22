#ifndef HW_GPS_H
#define HW_GPS_H
#include <stdbool.h>

/* Interactive GPS fix: blocks while reading the u-blox GPS (I2C/DDC). 
 * Prints 'V' once/sec until a valid fix, then prints:
 *   A <lat_deg> <lon_deg>
 * or exits early if the user presses 'q'.
 */
void gps_fix_interactive(void);

/* Non-interactive GPS fix for deploy/simulate: 
 * Blocks for up to timeout_sec seconds waiting for a valid fix.
 * Returns true if fix acquired, false on timeout.
 */
bool gps_fix_wait(int timeout_sec);

/* Same as gps_fix_wait(), but also returns the fix position in decimal
 * degrees so the caller can log it (the dive log's $GPS1/$GPS2 lines).
 * out_lat/out_lon may be NULL; they are left untouched on failure. */
bool gps_fix_wait_pos(int timeout_sec, double *out_lat, double *out_lon);

#endif /* HW_GPS_H */
