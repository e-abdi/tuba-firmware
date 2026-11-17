/*
 * sil_console.c — route printk to the UART (terminal) unbuffered
 *
 * native_sim hard-selects POSIX_ARCH_CONSOLE, whose printk hook buffers a
 * whole line and colourises it through the simulator's trace output. Menu
 * prompts without a trailing newline ("Select [1-5]: ") would then never
 * appear, and single-key streaming views would not behave like the serial
 * console. Install our own hook after both consoles have initialised: raw
 * bytes to the console UART, exactly what the ESP32 build does.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk-hooks.h>

static const struct device *const con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static int sil_console_out(int c)
{
    if (c == '\n') {
        uart_poll_out(con, '\r');
    }
    uart_poll_out(con, (unsigned char)c);
    return c;
}

static int sil_console_init(void)
{
    if (device_is_ready(con)) {
        __printk_hook_install(sil_console_out);   /* host libc stdout stays as is */
    }
    return 0;
}

SYS_INIT(sil_console_init, POST_KERNEL, 99);
