#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

/* Mirror only to UART1; console remains whatever Zephyr is configured to use */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart1), okay)
static const struct device *const uart1_dev = DEVICE_DT_GET(DT_NODELABEL(uart1));
#else
static const struct device *const uart1_dev = NULL;
#endif

static bool uart1_ready = false;

static inline void uart1_out_ch(char c) {
    if (!uart1_ready) return;
    if (c == '\n') { uart_poll_out(uart1_dev, '\r'); }
    uart_poll_out(uart1_dev, c);
}

static void uart1_out_buf(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uart1_out_ch(buf[i]);
    }
}

static int vprint_mirror(const char *fmt, va_list ap) {
    char tmp[256];
    int n = vsnprintk(tmp, sizeof(tmp), fmt, ap);
    if (n < 0) return n;
    size_t to_send = (n < (int)sizeof(tmp)) ? (size_t)n : sizeof(tmp) - 1;
    uart1_out_buf(tmp, to_send);
    return n;
}

void app_vprintk(const char *fmt, va_list ap) {
    /* Print to normal Zephyr console */
    vprintk(fmt, ap);
    /* Mirror to UART1 */
    vprint_mirror(fmt, ap);
}

void app_printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    /* Print to console */
    vprintk(fmt, ap);
    /* Need a copy of va_list for second pass */
    va_end(ap);
    va_start(ap, fmt);
    (void)vprint_mirror(fmt, ap);
    va_end(ap);
}

int app_puts(const char *s) {
    printk("%s\n", s);
    size_t len = strlen(s);
    uart1_out_buf(s, len);
    uart1_out_ch('\n');
    return (int)len;
}

int app_putchar(int c) {
    printk("%c", (char)c);
    uart1_out_ch((char)c);
    return c;
}

static int _app_print_init(void) {
    uart1_ready = (uart1_dev && device_is_ready(uart1_dev));
    return 0;
}

/* Initialize late so console is up; application-level is fine */
SYS_INIT(_app_print_init, APPLICATION, 0);

int app_print_init(void) { return _app_print_init(); }
