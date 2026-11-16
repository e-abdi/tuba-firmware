/* ota_stub.c — OTA is meaningless on native_sim; keep the menu linking. */
#include <zephyr/kernel.h>
#include "ota_simple.h"
#include "app_print.h"

int ota_simple_init(void) { return 0; }

int ota_simple_download(const char *url)
{
    app_printk("[OTA] not available in the simulator (url %s)\r\n", url);
    return -ENOTSUP;
}

int ota_simple_verify(void) { return -ENOTSUP; }
int ota_simple_reboot(void) { return -ENOTSUP; }
uint32_t ota_simple_get_progress(void) { return 0; }
