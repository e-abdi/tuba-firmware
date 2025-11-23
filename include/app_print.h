#pragma once
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the mirror output (called automatically) */
int app_print_init(void);

/* App-level printing that goes to console (printk) and mirrors to UART1 */
void app_vprintk(const char *fmt, va_list ap);
void app_printk(const char *fmt, ...);

/* Optional helpers */
int app_puts(const char *s);
int app_putchar(int c);

#ifdef __cplusplus
}
#endif
