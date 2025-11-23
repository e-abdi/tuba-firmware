
#include "hw_hmc6343.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <errno.h>

static const struct device *const i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const uart_console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#define HMC6343_ADDR 0x19

static inline int i2c_write_cmd(uint8_t cmd){ return i2c_write(i2c_dev, &cmd, 1, HMC6343_ADDR); }

static bool kbhit_quit(void){
    if (!device_is_ready(uart_console)) return false;
    unsigned char c;
    while (uart_fifo_read(uart_console, &c, 1) == 1) { if (c=='q'||c=='Q') return true; }
    return false;
}

static int eeprom_read(uint8_t addr, uint8_t *val){
    uint8_t cmd[2] = {0xE1, addr};
    int rc = i2c_write(i2c_dev, cmd, 2, HMC6343_ADDR);
    if (rc) return rc;
    k_msleep(10);
    return i2c_read(i2c_dev, val, 1, HMC6343_ADDR);
}

static int eeprom_write(uint8_t addr, uint8_t val){
    uint8_t buf[3] = {0xF1, addr, val};
    int rc = i2c_write(i2c_dev, buf, sizeof(buf), HMC6343_ADDR);
    if (rc) return rc;
    k_msleep(10);
    return 0;
}

static int hmc6343_ensure_perm_orientation_uf(void){
    uint8_t om1 = 0;
    int rc = eeprom_read(0x04, &om1);
    if (rc) { app_printk("[HMC6343] EEPROM read 0x04 failed: %d\r\n", rc); return rc; }
    uint8_t new_om1 = (om1 & ~0x07u) | 0x04u;
    if (new_om1 != om1) {
        app_printk("[HMC6343] Writing OM1 (0x04) from 0x%02X to 0x%02X for UF\r\n", om1, new_om1);
        rc = eeprom_write(0x04, new_om1);
        if (rc) { app_printk("[HMC6343] EEPROM write 0x04 failed: %d\r\n", rc); return rc; }
        (void)i2c_write_cmd(0x82); /* Reset */
        k_msleep(500);
    }
    return 0;
}

static int hmc6343_init(void){
    if (!device_is_ready(i2c_dev)) { app_printk("[HMC6343] I2C not ready\r\n"); return -ENODEV; }
    (void)i2c_write_cmd(0x75); /* Run */
    k_msleep(10);
    (void)hmc6343_ensure_perm_orientation_uf();
    (void)i2c_write_cmd(0x74); /* runtime UF */
    k_msleep(10);
    return 0;
}

void hmc6343_user_calibrate_interactive(void){
    if (hmc6343_init() != 0) return;
    app_printk("[HMC6343] Entering user calibration (0x71). Rotate device; press 'q' to exit.\r\n");
    if (i2c_write_cmd(0x71) != 0) { app_printk("[HMC6343] Failed to enter calibration\r\n"); return; }
    while (!kbhit_quit()) { k_sleep(K_MSEC(50)); }
    app_printk("[HMC6343] Exiting calibration (0x7E)...\r\n");
    (void)i2c_write_cmd(0x7E);
    k_msleep(60);
    app_printk("[HMC6343] Calibration exit done.\r\n");
}

void hmc6343_stream_heading_interactive(void){
    if (hmc6343_init() != 0) return;
    app_printk("[HMC6343] Streaming Heading/Pitch/Roll; press 'q' to quit\r\n");
    int64_t next = k_uptime_get();
    while (1) {
        if (i2c_write_cmd(0x50) != 0) { app_printk("[HMC6343] write 0x50 failed\r\n"); return; }
        k_msleep(2);
        uint8_t buf[6] = {0};
        int rc = i2c_read(i2c_dev, buf, sizeof(buf), HMC6343_ADDR);
        if (rc) { app_printk("[HMC6343] read failed: %d\r\n", rc); return; }
        int16_t head = (buf[0]<<8) | buf[1];
        int16_t pitch = (buf[2]<<8) | buf[3];
        int16_t roll  = (buf[4]<<8) | buf[5];
        app_printk("Heading=%.1f°, Pitch=%.1f°, Roll=%.1f°\r\n", head/10.0, (int16_t)pitch/10.0, (int16_t)roll/10.0);
        next += 1000;
        while (k_uptime_get() < next) { if (kbhit_quit()) { app_printk("[HMC6343] exit requested → back to menu\r\n"); return; } k_sleep(K_MSEC(20)); }
    }
}

/* Non-interactive single-sample read of heading/pitch/roll in degrees. */
int hmc6343_read(float *heading_deg, float *pitch_deg, float *roll_deg)
{
    if (hmc6343_init() != 0) return -ENODEV;
    if (i2c_write_cmd(0x50) != 0) { return -EIO; }
    k_msleep(2);
    uint8_t buf[6] = {0};
    int rc = i2c_read(i2c_dev, buf, sizeof(buf), HMC6343_ADDR);
    if (rc) return rc;
    int16_t head = (buf[0]<<8) | buf[1];
    int16_t pitch = (buf[2]<<8) | buf[3];
    int16_t roll  = (buf[4]<<8) | buf[5];
    if (heading_deg) *heading_deg = head / 10.0f;
    if (pitch_deg)   *pitch_deg   = pitch / 10.0f;
    if (roll_deg)    *roll_deg    = roll / 10.0f;
    return 0;
}
