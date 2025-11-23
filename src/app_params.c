#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/crc.h>
#include <zephyr/irq.h>
#include <string.h>

/* RP2040 SDK flash functions for direct flash access */
#include <hardware/flash.h>
#include <hardware/sync.h>

#include "app_params.h"
#include "app_print.h"

#define APP_PARAMS_SETTINGS_KEY "params/blob"

static struct app_params g_params;

/* Raw flash persistence (64KB storage partition at 0x1E0000) */
#define PARAMS_FLASH_OFFSET 0x001E0000u
#define PARAMS_FLASH_SECTOR_SIZE 4096u
#define PARAMS_MAGIC 0x5450524Du /* 'TPRM' */

struct params_flash_blob {
    uint32_t magic;
    uint32_t crc32;
    struct app_params params;
};

static void app_params_set_defaults_internal(void)
{
    g_params.dive_depth_m        = 5.0f;
    g_params.dive_timeout_min    = 5;
    g_params.dive_pump_s         = 3;
    g_params.deploy_wait_s       = 10;
    g_params.start_pump_s        = 0;
    g_params.climb_pump_s        = 0;

    g_params.start_pitch_s       = 0;
    g_params.surface_pitch_s     = 5;
    g_params.dive_pitch_s        = 7;
    g_params.climb_pitch_s       = 0;

    g_params.start_roll_s        = 0;
    g_params.max_roll_s          = 1;
    g_params.roll_time_s         = 5;

    g_params.desired_heading_deg = 180;
}

/* settings handler: load blob from NVS into g_params */
static int app_params_settings_set(const char *key, size_t len,
                                   settings_read_cb read_cb, void *cb_arg)
{
    const char *next;

    app_printk("[PARAM] settings_set called with key='%s', len=%zu\r\n", key, len);

    if (settings_name_steq(key, "blob", &next) && !next) {
        app_printk("[PARAM] blob key matched, expected size=%zu, got=%zu\r\n", 
                   sizeof(g_params), len);
        if (len != sizeof(g_params)) {
            app_printk("[PARAM] size mismatch!\r\n");
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, &g_params, sizeof(g_params));
        if (rc < 0) {
            app_printk("[PARAM] read_cb failed: %d\r\n", rc);
            return rc;
        }
        app_printk("[PARAM] loaded from NVM\r\n");
        return 0;
    }

    app_printk("[PARAM] key did not match 'blob'\r\n");
    return -ENOENT;
}

/* Export handler: called by settings_save_one to write data */
static int app_params_export(int (*cb)(const char *name,
                                       const void *value, size_t val_len))
{
    app_printk("[PARAM] export handler called\r\n");
    int rc = cb("blob", &g_params, sizeof(g_params));
    if (rc < 0) {
        app_printk("[PARAM] export cb failed: %d\r\n", rc);
    } else {
        app_printk("[PARAM] export cb OK\r\n");
    }
    return rc;
}

SETTINGS_STATIC_HANDLER_DEFINE(app_params, "params",
                               NULL, app_params_settings_set,
                               NULL, app_params_export);

int app_params_init(void)
{
    /* Start from compiled-in defaults; will overwrite if flash has blob */
    app_params_set_defaults_internal();
    app_printk("[PARAM] defaults applied\r\n");

    /* Read directly from flash memory-mapped region */
    const struct params_flash_blob *flash_ptr = 
        (const struct params_flash_blob *)(XIP_BASE + PARAMS_FLASH_OFFSET);
    
    if (flash_ptr->magic == PARAMS_MAGIC) {
        uint32_t crc = crc32_ieee_update(0, (const uint8_t *)&flash_ptr->params, sizeof(flash_ptr->params));
        if (crc == flash_ptr->crc32) {
            g_params = flash_ptr->params;
            app_printk("[PARAM] loaded from raw flash (magic OK, CRC OK)\r\n");
        } else {
            app_printk("[PARAM] flash blob CRC mismatch (stored=0x%08x calc=0x%08x)\r\n", 
                       flash_ptr->crc32, crc);
        }
    } else {
        app_printk("[PARAM] no valid params blob (magic=0x%08x)\r\n", flash_ptr->magic);
    }
    return 0;
}
int app_params_save(void)
{
    struct params_flash_blob blob = {
        .magic = PARAMS_MAGIC,
        .params = g_params,
    };
    blob.crc32 = crc32_ieee_update(0, (const uint8_t *)&blob.params, sizeof(blob.params));

    app_printk("[PARAM] saving blob (crc=0x%08x) to 0x%08x\r\n", blob.crc32, PARAMS_FLASH_OFFSET);

    /* Use RP2040 SDK flash functions directly - they handle XIP mode correctly */
    /* Note: offset is relative to flash base (0x10000000), so subtract XIP base */
    uint32_t flash_offset = PARAMS_FLASH_OFFSET;
    
    /* Disable interrupts during flash operations (SDK requirement) */
    uint32_t ints = save_and_disable_interrupts();
    
    /* Erase sector (4096 bytes) */
    flash_range_erase(flash_offset, PARAMS_FLASH_SECTOR_SIZE);
    
    /* Program the blob */
    flash_range_program(flash_offset, (const uint8_t *)&blob, sizeof(blob));
    
    restore_interrupts(ints);
    
    /* Small delay to ensure write completion */
    k_msleep(10);
    
    /* Verify write by reading directly from flash address */
    const struct params_flash_blob *flash_ptr = 
        (const struct params_flash_blob *)(XIP_BASE + flash_offset);
    
    if (flash_ptr->magic != PARAMS_MAGIC || flash_ptr->crc32 != blob.crc32) {
        app_printk("[PARAM] verify FAILED after write (magic=0x%08x, crc=0x%08x)\r\n", 
                   flash_ptr->magic, flash_ptr->crc32);
        return -EIO;
    }
    app_printk("[PARAM] persisted & verified (%zu bytes)\r\n", sizeof(blob));
    return 0;
}

void app_params_reset_defaults(void)
{
    app_params_set_defaults_internal();
    app_printk("[PARAM] reset to defaults (not yet saved)\r\n");
}

struct app_params *app_params_get(void)
{
    return &g_params;
}
