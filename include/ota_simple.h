#ifndef OTA_SIMPLE_H
#define OTA_SIMPLE_H

#include <stdint.h>

/**
 * Simple OTA (Over-The-Air) firmware update module
 * Downloads firmware via HTTP and reboots to apply
 */

/**
 * Initialize OTA subsystem
 * @return 0 on success, negative on error
 */
int ota_simple_init(void);

/**
 * Download firmware from HTTP URL
 * @param url Full HTTP URL to firmware image
 * @return 0 on success, negative on error
 */
int ota_simple_download(const char *url);

/**
 * Verify downloaded firmware
 * @return 0 if valid, negative if invalid
 */
int ota_simple_verify(void);

/**
 * Reboot device to apply firmware update
 * @return Does not return (reboots immediately)
 */
int ota_simple_reboot(void);

/**
 * Get current download progress in bytes
 * @return Number of bytes received
 */
uint32_t ota_simple_get_progress(void);

#endif /* OTA_SIMPLE_H */
