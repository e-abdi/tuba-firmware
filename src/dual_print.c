#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart0), okay)
static const struct device *const uart0_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(uart0));
#else
static const struct device *const uart0_dev = NULL;
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart1), okay)
static const struct device *const uart1_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(uart1));
#else
static const struct device *const uart1_dev = NULL;
#endif

static bool uart0_ready = false;
static bool uart1_ready = false;

static inline void out_ch(char c) {
    if (uart0_ready) {
        if (c == '\n') { uart_poll_out(uart0_dev, '\r'); }
        uart_poll_out(uart0_dev, c);
    }
    if (uart1_ready) {
        if (c == '\n') { uart_poll_out(uart1_dev, '\r'); }
        uart_poll_out(uart1_dev, c);
    }
}

static void out_buf(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        out_ch(buf[i]);
    }
}

static int vprint_to_uarts(const char *fmt, va_list ap) {
    char tmp[256];
    int n = vsnprintk(tmp, sizeof(tmp), fmt, ap);
    if (n < 0) {
        return n;
    }
    size_t to_send = (n < (int)sizeof(tmp)) ? (size_t)n : sizeof(tmp) - 1;
    out_buf(tmp, to_send);
    return n;
}

void dup_vprintk(const char *fmt, va_list ap) {
    (void)vprint_to_uarts(fmt, ap);
}

void dup_printk(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vprint_to_uarts(fmt, ap);
    va_end(ap);
}

/* Optional libc-style helpers (not macro-mapped) */
int my_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprint_to_uarts(fmt, ap);
    va_end(ap);
    return n;
}

int my_puts(const char *s) {
    size_t len = strlen(s);
    out_buf(s, len);
    out_ch('\n');
    return (int)len;
}

int my_putchar(int c) {
    out_ch((char)c);
    return c;
}

static int dual_print_init(const struct device *unused) {
    ARG_UNUSED(unused);
    /* Temporarily disable dual UART printing to isolate crash */
    uart0_ready = false;
    uart1_ready = false;
    return 0;
}

/* Initialize at APPLICATION time to avoid early device readiness issues */
SYS_INIT(dual_print_init, APPLICATION, 99);

int dual_print_init_hook(void) { return dual_print_init(NULL); }
