#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/mutex.h>
#include <string.h>

#define NET_CON_MAX 4
#define NET_CON_MSG_SIZE 128
#define NET_CON_MSG_COUNT 64

static int clients[NET_CON_MAX];
static K_MUTEX_DEFINE(mtx);

/* Message queue for console data */
struct net_con_msg {
    size_t len;
    char data[NET_CON_MSG_SIZE];
};
K_MSGQ_DEFINE(net_con_q, sizeof(struct net_con_msg), NET_CON_MSG_COUNT, 4);

static void net_console_sender(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    struct net_con_msg msg;
    while (1) {
        if (k_msgq_get(&net_con_q, &msg, K_FOREVER) == 0) {
            /* Copy client fds under mutex, then send without holding it */
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

/* Slightly larger stack for network send loop */
/* Generous stack: socket sends + mutex + msgq handling */
K_THREAD_DEFINE(net_con_tx, 3072, net_console_sender, NULL, NULL, NULL, 7, 0, 0);

static bool initialized = false;

/* Input accumulation and line queue */
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
    /* Chunk into NET_CON_MSG_SIZE blocks to fit the msgq */
    size_t off = 0;
    while (off < len) {
        struct net_con_msg msg;
        size_t chunk = len - off;
        if (chunk > NET_CON_MSG_SIZE) chunk = NET_CON_MSG_SIZE;
        memcpy(msg.data, buf + off, chunk);
        msg.len = chunk;
        /* Prefer delivery: wait briefly if queue is full to avoid losing lines */
        (void)k_msgq_put(&net_con_q, &msg, K_MSEC(50));
        off += chunk;
    }
}

/* Called from tcp_echo server loop when receiving bytes */
void net_console_ingest_bytes(const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
            if (c == '\r' || c == '\n') {
                /* terminate and enqueue if we have content (including empty line) */
                struct line_msg { char buf[128]; } msg;
                if (accum_len > 0) {
                    size_t n = accum_len < sizeof(msg.buf)-1 ? accum_len : sizeof(msg.buf)-1;
                    memcpy(msg.buf, accum, n);
                    msg.buf[n] = '\0';
                    (void)k_msgq_put(&net_con_line_q, msg.buf, K_NO_WAIT);
                    accum_len = 0;
                    memset(accum, 0, sizeof(accum));
                } else {
                    /* Enqueue an empty line: submit a zeroed element of size NET_CON_LINE_MAX */
                    char empty[NET_CON_LINE_MAX];
                    memset(empty, 0, sizeof(empty));
                    (void)k_msgq_put(&net_con_line_q, empty, K_NO_WAIT);
                }
        } else if (accum_len < NET_CON_LINE_MAX-1) {
            accum[accum_len++] = c;
        }
    }
}

bool net_console_poll_line(char *out, size_t max_len, k_timeout_t timeout)
{
    if (!out || max_len < NET_CON_LINE_MAX) {
        /* Expect buffers of at least NET_CON_LINE_MAX */
        return false;
    }
    return k_msgq_get(&net_con_line_q, out, timeout) == 0;
}
