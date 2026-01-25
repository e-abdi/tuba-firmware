// Mirror printk/console output to UART1 (RP2040 Pico default: TX=GP8, RX=GP9)
// Use legacy Zephyr printk hook API: __printk_hook_install()
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

static const struct device *uart1_dev;

static int mirror_putchar(int c)
{
    if (uart1_dev && device_is_ready(uart1_dev)) {
        uart_poll_out(uart1_dev, (unsigned char)c);
    }
    return c; // keep default console working on uart0
}

static int console_mirror_init(void)
{
    /* Console mirroring not enabled on ESP32 (UART2 GPIO17/16 not configured).
       On RP2040 with UART1, this would mirror printk to uart1.
       For now, keep UART0 as the sole console.
    */
    return 0;
}

/* Initialize at application time to avoid pre-kernel device readiness issues */
SYS_INIT(console_mirror_init, APPLICATION, 0);
