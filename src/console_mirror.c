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
    uart1_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));

    /* Older Zephyr trees expose only this symbol */
    extern void __printk_hook_install(int (*fn)(int));
    __printk_hook_install(mirror_putchar);

    return 0;
}

SYS_INIT(console_mirror_init, PRE_KERNEL_1, 0);
