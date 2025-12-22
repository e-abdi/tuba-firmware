#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/crc.h>
#include <string.h>

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
    int rc;
    
    /* Start from compiled-in defaults; will overwrite if settings available */
    app_params_set_defaults_internal();
    app_printk("[PARAM] defaults applied\r\n");

    /* Initialize Zephyr settings subsystem to load from NVS */
    rc = settings_subsys_init();
    if (rc != 0) {
        app_printk("[PARAM] settings_subsys_init failed: %d\r\n", rc);
        app_printk("[PARAM] continuing with defaults only\r\n");
        return 0;
    }
    
    /* Load parameters from NVS if they exist */
    rc = settings_load_subtree("params");
    if (rc == 0) {
        app_printk("[PARAM] loaded from NVS\r\n");
    } else if (rc == -ENOENT) {
        app_printk("[PARAM] no NVS data yet, using defaults\r\n");
        return 0;
    } else {
        app_printk("[PARAM] settings_load_subtree failed: %d\r\n", rc);
        app_printk("[PARAM] continuing with defaults\r\n");
        return 0;
    }
    
    return 0;
}
int app_params_save(void)
{
    int rc;
    
    /* Save parameters to NVS via settings subsystem */
    rc = settings_save_one(APP_PARAMS_SETTINGS_KEY, &g_params, sizeof(g_params));
    if (rc == 0) {
        app_printk("[PARAM] saved to NVS successfully\r\n");
    } else {
        app_printk("[PARAM] save to NVS failed: %d\r\n", rc);
    }
    return rc;
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
