/*
 * net_console.c — WiFi (telnet) console backend
 *
 * Responsibilities:
 *   - Track connected telnet client sockets (added by the TCP server in
 *     main.c) and fan console output out to all of them. Output is queued
 *     into a message queue and sent from a dedicated thread so app_printk()
 *     never blocks on a slow WiFi client.
 *   - Aggregate received bytes into complete lines and queue them for
 *     net_console_poll_line() consumers (the menu state machine and the
 *     interactive sensor views).
 *   - Own the runtime console-input-mode switch (UART / WiFi / both) that
 *     replaces the old compile-time DISABLE_WIFI_INPUT #define. The mode is
 *     persisted via app_params and applied with console_input_mode_set().
 */
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/mutex.h>
#include <zephyr/drivers/uart.h>
#include <string.h>

#include "net_console.h"

#define NET_CON_MAX NET_CONSOLE_MAX_CLIENTS   /* max simultaneous telnet clients */
#define NET_CON_MSG_SIZE 128     /* output chunk size */
#define NET_CON_MSG_COUNT 64     /* output queue depth */

static int clients[NET_CON_MAX];
static K_MUTEX_DEFINE(mtx);

/* ---- Input-mode switch ------------------------------------------------ */

/* Which input sources are honored. Output is always mirrored to both.
 * Volatile-free: written from the UI thread, read from workers; a stale
 * read for one poll cycle is harmless. */
static console_input_mode_t g_input_mode = CONSOLE_INPUT_BOTH;

void console_input_mode_set(console_input_mode_t mode)
{
    if (mode > CONSOLE_INPUT_WIFI) {
        mode = CONSOLE_INPUT_BOTH;
    }
    g_input_mode = mode;
}

console_input_mode_t console_input_mode_get(void)
{
    return g_input_mode;
}

const char *console_input_mode_str(console_input_mode_t mode)
{
    switch (mode) {
    case CONSOLE_INPUT_UART: return "USB/serial only";
    case CONSOLE_INPUT_WIFI: return "WiFi only";
    default:                 return "USB/serial + WiFi";
    }
}

bool console_uart_input_enabled(void)
{
    return g_input_mode != CONSOLE_INPUT_WIFI;
}

static bool wifi_input_enabled(void)
{
    return g_input_mode != CONSOLE_INPUT_UART;
}

/* ---- Output path: msgq + sender thread -------------------------------- */

struct net_con_msg {
    size_t len;
    char data[NET_CON_MSG_SIZE];
};
K_MSGQ_DEFINE(net_con_q, sizeof(struct net_con_msg), NET_CON_MSG_COUNT, 4);

/* Sender thread: drains the output queue and writes to every client.
 * Client fds are copied under the mutex, then sent without holding it so a
 * blocking send cannot stall add/remove. */
static void net_console_sender(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    struct net_con_msg msg;
    while (1) {
        if (k_msgq_get(&net_con_q, &msg, K_FOREVER) == 0) {
            int local[NET_CON_MAX]; int cnt = 0;
            k_mutex_lock(&mtx, K_FOREVER);
            for (int i = 0; i < NET_CON_MAX; i++) {
                if (clients[i] >= 0) local[cnt++] = clients[i];
            }
            k_mutex_unlock(&mtx);
            for (int i = 0; i < cnt; i++) {
                (void)zsock_send(local[i], msg.data, msg.len, 0);
            }
        }
    }
}

/* Generous stack: socket sends + mutex + msgq handling on Xtensa. */
K_THREAD_DEFINE(net_con_tx, 3072, net_console_sender, NULL, NULL, NULL, 7, 0, 0);

static bool initialized = false;

/* ---- Input path: byte aggregator -> line queue ------------------------ */

#define NET_CON_LINE_MAX 128
K_MSGQ_DEFINE(net_con_line_q, NET_CON_LINE_MAX, 8, 4);
static char accum[NET_CON_LINE_MAX];
static size_t accum_len = 0;

void net_console_init(void)
{
    if (initialized) return;
    for (int i = 0; i < NET_CON_MAX; i++) clients[i] = -1;
    initialized = true;
}

void net_console_add(int fd)
{
    net_console_init();
    k_mutex_lock(&mtx, K_FOREVER);
    for (int i = 0; i < NET_CON_MAX; i++) {
        if (clients[i] == -1) { clients[i] = fd; break; }
    }
    k_mutex_unlock(&mtx);
}

void net_console_remove(int fd)
{
    k_mutex_lock(&mtx, K_FOREVER);
    for (int i = 0; i < NET_CON_MAX; i++) {
        if (clients[i] == fd) { clients[i] = -1; break; }
    }
    k_mutex_unlock(&mtx);
}

void net_console_write(const char *buf, size_t len)
{
    if (!initialized || !buf || len == 0) return;
    /* Chunk into NET_CON_MSG_SIZE blocks to fit the msgq element size. */
    size_t off = 0;
    while (off < len) {
        struct net_con_msg msg;
        size_t chunk = len - off;
        if (chunk > NET_CON_MSG_SIZE) chunk = NET_CON_MSG_SIZE;
        memcpy(msg.data, buf + off, chunk);
        msg.len = chunk;
        /* Prefer delivery: wait briefly if the queue is full rather than
         * silently dropping mission log lines. */
        (void)k_msgq_put(&net_con_q, &msg, K_MSEC(50));
        off += chunk;
    }
}

/* Called from the telnet server loop with raw received bytes. CR or LF
 * terminates a line; an ENTER on its own queues an empty line (the menu
 * uses empty lines as the "ENTER pressed" signal). */
void net_console_ingest_bytes(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\r' || c == '\n') {
            /* Queue elements are fixed-size; zero-fill so short lines are
             * NUL-terminated for the reader. */
            char msg[NET_CON_LINE_MAX];
            memset(msg, 0, sizeof(msg));
            if (accum_len > 0) {
                size_t n = accum_len < sizeof(msg) - 1 ? accum_len : sizeof(msg) - 1;
                memcpy(msg, accum, n);
                accum_len = 0;
            }
            (void)k_msgq_put(&net_con_line_q, msg, K_NO_WAIT);
        } else if (accum_len < NET_CON_LINE_MAX - 1) {
            accum[accum_len++] = c;
        }
    }
}

bool net_console_poll_line(char *out, size_t max_len, k_timeout_t timeout)
{
    /* Runtime mode switch: in UART-only mode WiFi clients are view-only. */
    if (!wifi_input_enabled()) {
        return false;
    }

    if (!out || max_len < NET_CON_LINE_MAX) {
        /* Callers must supply at least one full queue element. */
        return false;
    }
    return k_msgq_get(&net_con_line_q, out, timeout) == 0;
}

/* ---- Shared quit-poll helper ------------------------------------------ */

/* Console UART for single-key 'q' detection in streaming views. */
static const struct device *const quit_uart =
    DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

bool console_quit_requested(void)
{
    /* WiFi console: a line consisting of just "q". */
    char line[NET_CON_LINE_MAX];
    if (net_console_poll_line(line, sizeof(line), K_NO_WAIT)) {
        if ((line[0] == 'q' || line[0] == 'Q') && line[1] == '\0') {
            return true;
        }
    }

    /* UART console: a bare 'q' keypress (no ENTER needed). */
    if (!console_uart_input_enabled()) {
        return false;
    }
    if (quit_uart == NULL || !device_is_ready(quit_uart)) {
        return false;
    }
    unsigned char c;
    if (uart_poll_in(quit_uart, &c) == 0 && (c == 'q' || c == 'Q')) {
        return true;
    }
    return false;
}
