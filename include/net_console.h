/*
 * net_console.h — WiFi (telnet) console: output mirroring + line input
 *
 * The glider has two consoles that work at the same time:
 *   - UART0 (USB/serial bridge, 9600 baud)
 *   - WiFi telnet (AP "Tuba-Glider", 192.168.4.1:23)
 *
 * Output written through app_printk() is mirrored to every connected telnet
 * client. Input arrives as complete lines (terminated by ENTER) through
 * net_console_poll_line().
 *
 * Which input sources are honored is a runtime setting (persisted in the
 * parameters menu) — see console_input_mode_t below. Output is always sent
 * to both, so you can watch a mission from either console regardless of the
 * input mode.
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Input-mode selection (user-switchable, saved in parameters) ---- */
typedef enum {
    CONSOLE_INPUT_BOTH = 0,   /* accept commands from UART and WiFi (default) */
    CONSOLE_INPUT_UART = 1,   /* UART/USB only — WiFi clients are view-only  */
    CONSOLE_INPUT_WIFI = 2,   /* WiFi only — UART is view-only               */
} console_input_mode_t;

/* Change/read the active input mode. Takes effect immediately. */
void console_input_mode_set(console_input_mode_t mode);
console_input_mode_t console_input_mode_get(void);
const char *console_input_mode_str(console_input_mode_t mode);

/* True when UART input should be honored under the current mode. */
bool console_uart_input_enabled(void);

/* ---- Client / output management (used by the telnet server) --------- */
void net_console_init(void);
void net_console_add(int fd);
void net_console_remove(int fd);
void net_console_write(const char *buf, size_t len);

/* Feed raw received bytes into the line aggregator (telnet server only). */
void net_console_ingest_bytes(const char *buf, size_t len);

/* ---- Input API ------------------------------------------------------- */
/* Get one complete input line (ENTER-terminated) from the WiFi console.
 * Returns false immediately if WiFi input is disabled by the current mode.
 * An empty line (just ENTER) yields an empty string. */
bool net_console_poll_line(char *out, size_t max_len, k_timeout_t timeout);

/* Shared "press q to quit" poll used by all interactive streaming views:
 * checks the WiFi console for a "q" line and the UART for a 'q' keypress,
 * honoring the current input mode. Non-blocking. */
bool console_quit_requested(void);

#ifdef __cplusplus
}
#endif
