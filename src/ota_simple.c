#include "ota_simple.h"
#include "app_print.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define OTA_BUFFER_SIZE 2048
#define OTA_MAX_SIZE (1024 * 1024)
#define OTA_HTTP_HEADER_MAX 4096

static uint32_t ota_bytes_received = 0;
static uint8_t ota_buffer[OTA_BUFFER_SIZE];
static char ota_hostname[128];
static char ota_path[256];
static char ota_request[512];
static const uint8_t ota_slot1_area_id = FIXED_PARTITION_ID(slot1_partition);
static uint8_t ota_http_header[OTA_HTTP_HEADER_MAX];
static size_t ota_http_header_len;
static const struct flash_area *ota_fap = NULL;

static int ota_find_header_end(const uint8_t *buf, size_t len)
{
    if (len < 4) {
        return -1;
    }

    for (size_t index = 0; index <= len - 4; index++) {
        if (buf[index] == '\r' && buf[index + 1] == '\n' &&
            buf[index + 2] == '\r' && buf[index + 3] == '\n') {
            return (int)(index + 4);
        }
    }

    return -1;
}

int ota_simple_init(void)
{
    ota_bytes_received = 0;
    ota_http_header_len = 0;
    app_printk("[OTA] Initialized\r\n");
    return 0;
}

static int ota_write_chunk(const uint8_t *data, size_t len)
{
    if (!ota_fap) {
        app_printk("[OTA] Flash area not open\r\n");
        return -EINVAL;
    }
    
    int ret = flash_area_write(ota_fap, ota_bytes_received, data, len);
    if (ret < 0) {
        app_printk("[OTA] flash_area_write failed at offset %u: %d\r\n", ota_bytes_received, ret);
        return ret;
    }
    return 0;
}

int ota_simple_download(const char *url)
{
    int sock;
    struct sockaddr_in addr;
    int port = 80;
    int bytes_read;
    int ret;
    const char *path_start;

    app_printk("[OTA] Starting download from: %s\r\n", url);

    ota_bytes_received = 0;
    ota_http_header_len = 0;

    // Open and erase the flash area
    ret = flash_area_open(ota_slot1_area_id, &ota_fap);
    if (ret < 0) {
        app_printk("[OTA] flash_area_open failed for slot1 area %u: %d\r\n",
                   ota_slot1_area_id, ret);
        return ret;
    }

    ret = flash_area_erase(ota_fap, 0, ota_fap->fa_size);
    if (ret < 0) {
        app_printk("[OTA] flash_area_erase failed: %d\r\n", ret);
        flash_area_close(ota_fap);
        ota_fap = NULL;
        return ret;
    }

    app_printk("[OTA] Erased slot1 partition (%zu bytes)\r\n", ota_fap->fa_size);

    if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        app_printk("[OTA] HTTPS not supported\r\n");
        return -ENOTSUP;
    }

    memset(ota_hostname, 0, sizeof(ota_hostname));
    memset(ota_path, 0, sizeof(ota_path));
    strcpy(ota_path, "/");

    path_start = strchr(url, '/');
    if (path_start) {
        int host_len = path_start - url;
        if (host_len > sizeof(ota_hostname) - 1) {
            host_len = sizeof(ota_hostname) - 1;
        }
        strncpy(ota_hostname, url, host_len);
        ota_hostname[host_len] = '\0';
        strncpy(ota_path, path_start, sizeof(ota_path) - 1);
        ota_path[sizeof(ota_path) - 1] = '\0';
    } else {
        strncpy(ota_hostname, url, sizeof(ota_hostname) - 1);
        ota_hostname[sizeof(ota_hostname) - 1] = '\0';
    }

    char *port_sep = strchr(ota_hostname, ':');
    if (port_sep) {
        port = atoi(port_sep + 1);
        *port_sep = '\0';
    }

    app_printk("[OTA] Host: %s, Port: %d, Path: %s\r\n", ota_hostname, port, ota_path);

    sock = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        app_printk("[OTA] Socket creation failed: %d\r\n", sock);
        return sock;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ret = zsock_inet_pton(AF_INET, ota_hostname, &addr.sin_addr);
    if (ret < 0) {
        app_printk("[OTA] Invalid IP: %s\r\n", ota_hostname);
        zsock_close(sock);
        return ret;
    }

    ret = zsock_connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        app_printk("[OTA] Connect failed: %d\r\n", ret);
        zsock_close(sock);
        return ret;
    }

    app_printk("[OTA] Connected to host\r\n");

    snprintf(ota_request, sizeof(ota_request),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             ota_path, ota_hostname);

    ret = zsock_send(sock, ota_request, strlen(ota_request), 0);
    if (ret < 0) {
        app_printk("[OTA] Send failed: %d\r\n", ret);
        zsock_close(sock);
        return ret;
    }

    app_printk("[OTA] Downloading firmware...\r\n");

    int in_body = 0;

    while (1) {
        bytes_read = zsock_recv(sock, ota_buffer, sizeof(ota_buffer), 0);
        if (bytes_read <= 0) {
            break;
        }

        if (!in_body) {
            size_t space = OTA_HTTP_HEADER_MAX - ota_http_header_len;
            if ((size_t)bytes_read > space) {
                app_printk("[OTA] HTTP header too large\r\n");
                zsock_close(sock);
                return -EOVERFLOW;
            }

            memcpy(&ota_http_header[ota_http_header_len], ota_buffer, bytes_read);
            ota_http_header_len += (size_t)bytes_read;

            int header_end = ota_find_header_end(ota_http_header, ota_http_header_len);
            if (header_end >= 0) {
                in_body = 1;

                if (ota_http_header_len < 12 ||
                    memcmp(ota_http_header, "HTTP/1.", 7) != 0 ||
                    ota_http_header[9] != '2' || ota_http_header[10] != '0' || ota_http_header[11] != '0') {
                    app_printk("[OTA] HTTP response is not 200 OK\r\n");
                    zsock_close(sock);
                    return -EBADMSG;
                }

                size_t body_len = ota_http_header_len - (size_t)header_end;
                if (body_len > 0) {
                    ret = ota_write_chunk(&ota_http_header[header_end], body_len);
                    if (ret < 0) {
                        zsock_close(sock);
                        return ret;
                    }
                    ota_bytes_received += (uint32_t)body_len;
                    app_printk(".");
                }
            }
        } else {
            ret = ota_write_chunk(ota_buffer, bytes_read);
            if (ret < 0) {
                zsock_close(sock);
                return ret;
            }
            ota_bytes_received += bytes_read;
            app_printk(".");
        }

        if (ota_bytes_received > OTA_MAX_SIZE) {
            app_printk("\r\n[OTA] Firmware too large\r\n");
            zsock_close(sock);
            return -EFBIG;
        }
    }

    zsock_close(sock);

    if (ota_fap) {
        flash_area_close(ota_fap);
        ota_fap = NULL;
    }

    app_printk("\r\n[OTA] Downloaded %u bytes\r\n", ota_bytes_received);
    return 0;
}

int ota_simple_verify(void)
{
    if (ota_bytes_received == 0) {
        app_printk("[OTA] No firmware received\r\n");
        return -EINVAL;
    }

    uint8_t upload_slot = flash_img_get_upload_slot();
    app_printk("[OTA] upload_slot=%u, expected_slot1=%u\r\n", upload_slot, ota_slot1_area_id);

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
    // Debug: read first 16 bytes from slot to check for corruption
    const struct flash_area *fap;
    int ret = flash_area_open(ota_slot1_area_id, &fap);
    if (ret == 0) {
        uint8_t hdr_bytes[16];
        ret = flash_area_read(fap, 0, hdr_bytes, sizeof(hdr_bytes));
        if (ret == 0) {
            app_printk("[OTA] First 16 bytes at slot %u: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\r\n",
                       ota_slot1_area_id,
                       hdr_bytes[0], hdr_bytes[1], hdr_bytes[2], hdr_bytes[3],
                       hdr_bytes[4], hdr_bytes[5], hdr_bytes[6], hdr_bytes[7],
                       hdr_bytes[8], hdr_bytes[9], hdr_bytes[10], hdr_bytes[11],
                       hdr_bytes[12], hdr_bytes[13], hdr_bytes[14], hdr_bytes[15]);
        }
        flash_area_close(fap);
    }

    struct mcuboot_img_header hdr;
    ret = boot_read_bank_header(ota_slot1_area_id, &hdr, sizeof(hdr));
    if (ret < 0) {
        app_printk("[OTA] MCUboot header check failed in slot %u: %d\r\n", ota_slot1_area_id, ret);
        app_printk("[OTA] Use signed image: zephyr.signed.bin (not zephyr.bin)\r\n");
        return ret;
    }

    app_printk("[OTA] Firmware verified: %u bytes in slot %u (image_size=%u)\r\n",
               ota_bytes_received, ota_slot1_area_id, hdr.h.v1.image_size);
#else
    app_printk("[OTA] Firmware staged: %u bytes in slot %u\r\n",
               ota_bytes_received, upload_slot);
#endif

    return 0;
}

int ota_simple_reboot(void)
{
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
    int ret = boot_request_upgrade(1);
    if (ret < 0) {
        app_printk("[OTA] Failed to request MCUboot upgrade: %d\r\n", ret);
        return ret;
    }
    app_printk("[OTA] Upgrade requested. Rebooting...\r\n");
#else
    app_printk("[OTA] MCUboot not enabled; cannot apply staged image\r\n");
    return -ENOTSUP;
#endif

    k_msleep(500);
    while (1) {
        k_msleep(1000);
    }
    return 0;
}

uint32_t ota_simple_get_progress(void)
{
    return ota_bytes_received;
}
