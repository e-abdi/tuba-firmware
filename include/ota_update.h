#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <zephyr/kernel.h>

/**
 * Initialize OTA update subsystem
 * @return 0 on success, negative on error
 */
int ota_update_init(void);

/**
 * Download and install firmware update from HTTP server
 * @param url Full HTTP URL to firmware image (e.g., "http://192.168.4.100:8080/firmware.bin")
 * @return 0 on success, negative on error
 */
int ota_update_download_and_install(const char *url);

/**
 * Check if an update is available on the server
 * @param server_url Base URL of update server (e.g., "http://192.168.4.100:8080")
 * @param current_version Current firmware version string
 * @return 1 if update available, 0 if up-to-date, negative on error
 */
int ota_update_check_available(const char *server_url, const char *current_version);

/**
 * Get the currently running image slot information
 * @return 0 if running from primary slot, 1 if running from secondary slot
 */
int ota_update_get_running_slot(void);

/**
 * Manually trigger a reboot after OTA update is staged
 */
void ota_update_reboot(void);

#endif /* OTA_UPDATE_H */
