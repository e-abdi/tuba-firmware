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

#endif /* HW_GPS_H */
