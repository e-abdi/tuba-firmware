#pragma once
#include <stdarg.h>

/* Initialize dual print (called automatically via SYS_INIT) */
int dual_print_init_hook(void);

/* Duplicate printk to both UART0 and UART1 */
#ifdef __GNUC__
#define ATTR_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define ATTR_PRINTF(fmt_idx, arg_idx)
#endif

void dup_vprintk(const char *fmt, va_list ap);
void dup_printk(const char *fmt, ...) ATTR_PRINTF(1,2);

/* Redirect only printk calls from this application */
#define printk(...) dup_printk(__VA_ARGS__)

/* Optional helpers if you want explicit dual-printf in your code */
int my_printf(const char *fmt, ...);
int my_puts(const char *s);
int my_putchar(int c);
