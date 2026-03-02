#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/net/http_client.h>

#include "ota_update.h"
#include "app_print.h"

/* Flash image context for OTA updates */
static struct flash_img_context ota_flash_ctx;

int ota_update_init(void)
{
    int ret;
    
    /* Initialize flash image context for secondary image slot */
    ret = flash_img_init(&ota_flash_ctx);
    if (ret) {
        app_printk("[OTA] Failed to initialize flash image context: %d\r\n", ret);
        return ret;
    }
    
    app_printk("[OTA] OTA update subsystem initialized\r\n");
    return 0;
}

/**
 * HTTP client callback to receive firmware data chunks
 */
static int ota_http_response_cb(struct http_response *rsp,
                                enum http_final_call final_call,
                                void *user_data)
{
    struct flash_img_context *ctx = (struct flash_img_context *)user_data;
    int ret = 0;
    
    app_printk("[OTA] Received %d bytes\r\n", rsp->body_len);
    
    if (rsp->body_len > 0) {
        /* Write received data to flash */
        ret = flash_img_buffered_write(ctx, (const uint8_t *)rsp->body, rsp->body_len, final_call);
        if (ret) {
            app_printk("[OTA] Flash write failed: %d\r\n", ret);
            return ret;
        }
    }
    
    if (final_call == HTTP_DATA_FINAL) {
        app_printk("[OTA] Download complete\r\n");
        
        /* Verify the image */
        ret = flash_img_verify(&ota_flash_ctx, boot_is_img_confirmed());
        if (ret) {
            app_printk("[OTA] Image verification failed: %d\r\n", ret);
            return ret;
        }
        
        app_printk("[OTA] Image verified successfully\r\n");
    }
    
    return ret;
}

int ota_update_download_and_install(const char *url)
{
    struct http_request req = {0};
    int ret;
    
    if (!url) {
        app_printk("[OTA] Invalid URL\r\n");
        return -EINVAL;
    }
    
    app_printk("[OTA] Starting firmware download from: %s\r\n", url);
    
    /* Reset flash image context for new download */
    flash_img_init(&ota_flash_ctx);
    
    /* Configure HTTP request */
    req.method = HTTP_GET;
    req.url = url;
    req.host = NULL;  /* URL contains full address */
    req.protocol = "HTTP/1.1";
    req.response = ota_http_response_cb;
    req.user_data = &ota_flash_ctx;
    
    /* Start download */
    ret = http_client_req(NULL, &req, NULL);
    if (ret) {
        app_printk("[OTA] HTTP download failed: %d\r\n", ret);
        return ret;
    }
    
    app_printk("[OTA] Firmware update staged successfully. Reboot to apply.\r\n");
    return 0;
}

int ota_update_check_available(const char *server_url, const char *current_version)
{
    /* This would typically make an HTTP request to check version on server */
    /* For now, just a placeholder */
    app_printk("[OTA] Version check not yet implemented\r\n");
    return 0;
}

int ota_update_get_running_slot(void)
{
    return boot_is_img_confirmed() ? 0 : 1;
}

void ota_update_reboot(void)
{
    app_printk("[OTA] Rebooting to apply firmware update...\r\n");
    k_sleep(K_MSEC(500));
    sys_reboot(SYS_REBOOT_COLD);
}
