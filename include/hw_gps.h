#ifndef HW_GPS_H
#define HW_GPS_H

/* Blocks while reading the u-blox GPS (I2C/DDC). 
 * Prints 'V' once/sec until a valid fix, then prints:
 *   A <lat_deg> <lon_deg>
 * or exits early if the user presses 'q'.
 */
void gps_fix_interactive(void);

#endif /* HW_GPS_H */
