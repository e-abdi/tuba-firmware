#pragma once
#include <stddef.h>
#include <zephyr/sys/mutex.h>

#ifdef __cplusplus
extern "C" {
#endif

void net_console_init(void);
void net_console_add(int fd);
void net_console_remove(int fd);
void net_console_write(const char *buf, size_t len);

/* Input API: get a complete line (terminated by CR or LF). Returns true if a line was read. */
bool net_console_poll_line(char *out, size_t max_len, k_timeout_t timeout);

#ifdef __cplusplus
}
#endif
