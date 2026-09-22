/*
 * main.c — Tuba underwater glider firmware entry point
 *
 * Responsibilities:
 *   1. Bring up subsystems in order: settings/params -> wall clock ->
 *      sensor scan -> actuators -> OTA -> leak detector.
 *   2. Host the UI state machine main loop. States and their handlers are
 *      declared in app_events.h / ui_menu.h; this file owns the dispatch
 *      table that binds them together and performs transitions.
 *   3. Run the WiFi access point + telnet console server and the
 *      background leak-monitor thread.
 *
 * Console architecture (see net_console.h):
 *   - UART0 (USB bridge, 9600 baud) and WiFi telnet (192.168.4.1:23) are
 *     both live. Output goes to both; which one(s) accept input is a
 *     runtime parameter (PARAMETERS menu, option 'g').
 *
 * State machine (see tuba_state_machine.puml):
 *   - Typed lines   -> ui_handle_line(state, line) -> next state
 *   - Async events  -> STATE_TABLE[state].on_event(evt) -> next state
 *   - Transitions   -> exit hook of old state, then entry hook of new one
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* WiFi AP console (ESP32 DevKitC) */
#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dhcpv4_server.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include "app_events.h"
#include "app_limits.h"
#include "app_params.h"
#include "app_time.h"
#include "ui_menu.h"
#include "hw_motors.h"
#include "hw_pump.h"
#include "hw_limit_switches.h"
#include "hw_leak.h"
#include "net_console.h"
#include "sensors.h"
#include "ota_simple.h"
#include "version.h"

/* ===================================================================
 * Console input plumbing
 * =================================================================== */

/* Console UART — _OR_NULL so a missing console cannot crash boot. */
static const struct device *const uart_console = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

static inline bool uart_ready(void) { return device_is_ready(uart_console); }

static bool uart_getch(uint8_t *out) {
    if (!uart_ready()) return false;
    return (uart_poll_in(uart_console, (unsigned char *)out) == 0);
}

/* Poll both consoles for one complete input line (non-blocking).
 *
 * Returns true when a full line is available in buf. Sets *enter_only when
 * the user pressed ENTER on an empty line (the menus use that as its own
 * signal, e.g. to leave POWERUP_WAIT).
 *
 * The WiFi console delivers whole lines through a queue; the UART is
 * accumulated here byte-by-byte with local echo. Which sources are honored
 * follows the runtime console-input mode. */
static char line_buf[APP_LINE_MAX];
static bool read_line_nonblock(char *buf, size_t buflen, bool *enter_only) {
    *enter_only = false;

    /* 1) WiFi/telnet console (already line-buffered) */
    char line_tmp[128];
    if (net_console_poll_line(line_tmp, sizeof(line_tmp), K_NO_WAIT)) {
        size_t n = strnlen(line_tmp, sizeof(line_tmp));
        if (n == 0) {
            *enter_only = true;
            printk("\r\n");
            return false;
        }
        if (n >= buflen) n = buflen-1;
        memcpy(buf, line_tmp, n);
        buf[n] = '\0';
        printk("%s\r\n", buf);   /* echo network input on the local console */
        return true;
    }

    /* 2) UART console (byte-wise accumulation with echo) */
    if (!console_uart_input_enabled()) {
        return false;            /* WiFi-only mode: UART is view-only */
    }
    static size_t idx = 0;
    uint8_t ch;
    while (uart_getch(&ch)) {
        if (ch=='\r'||ch=='\n') {
            printk("\r\n");
            if (idx > 0) {
                buf[idx]='\0';
                idx=0;
                return true;
            }
            *enter_only = true;
            return false;
        } else if (idx<buflen-1) {
            buf[idx++]=(char)ch;
            printk("%c", ch);    /* local echo */
        }
    }
    return false;
}

/* Motor currently jogged by the PR_INPUT menu state (set in ui_menu.c). */
enum motor_id current_motor = MOTOR_ROLL;

/* ===================================================================
 * Timers & event queue
 * =================================================================== */

static void timeout_cb(struct k_timer *tmr); K_TIMER_DEFINE(startup_timeout, timeout_cb, NULL);
static void tick_cb(struct k_timer *tmr);    K_TIMER_DEFINE(ui_tick, tick_cb, NULL);
K_MSGQ_DEFINE(evt_q, sizeof(event_t), 8, 4);

static inline void post_event(event_id_t id) { event_t e = {.id=id}; (void)k_msgq_put(&evt_q,&e,K_NO_WAIT); }
static void timeout_cb(struct k_timer *tmr){ARG_UNUSED(tmr);post_event(EVT_TIMEOUT);}
static void tick_cb(struct k_timer *tmr){ARG_UNUSED(tmr);post_event(EVT_TICK);}

/* ===================================================================
 * WiFi access point + telnet console server
 * =================================================================== */
#ifdef CONFIG_WIFI

#define AP_SSID       "Tuba-Glider"
#define AP_CHANNEL    6
#define AP_IPV4       0xc0a80401u   /* 192.168.4.1  */
#define AP_DHCP_BASE  0xc0a80402u   /* leases from 192.168.4.2 */
#define AP_NETMASK    0xffffff00u   /* /24 */

/* Log interface/IP lifecycle events — invaluable when debugging clients
 * that associate but never reach the telnet port. */
static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(cb); ARG_UNUSED(iface);
    switch (event) {
    case NET_EVENT_IF_UP:         app_printk("WiFi: IF UP\r\n"); break;
    case NET_EVENT_IF_DOWN:       app_printk("WiFi: IF DOWN\r\n"); break;
    case NET_EVENT_IPV4_ADDR_ADD: app_printk("WiFi: IPv4 address added\r\n"); break;
    case NET_EVENT_IPV4_ADDR_DEL: app_printk("WiFi: IPv4 address removed\r\n"); break;
    default: break;
    }
}

/* Enable AP mode with our fixed SSID/channel. Returns net_mgmt status. */
static int wifi_ap_enable(struct net_if *iface)
{
    struct wifi_connect_req_params ap_cfg = {
        .ssid = (uint8_t *)AP_SSID,
        .ssid_length = strlen(AP_SSID),
        .channel = AP_CHANNEL,
        .security = WIFI_SECURITY_TYPE_NONE,
    };
    return net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &ap_cfg, sizeof(ap_cfg));
}

/* Make sure 192.168.4.1/24 is configured on the interface (idempotent). */
static void wifi_ensure_ipv4(struct net_if *iface)
{
    struct in_addr addr = { .s_addr = htonl(AP_IPV4) };
    struct net_if *tmp = NULL;
    if (!net_if_ipv4_addr_lookup(&addr, &tmp)) {
        (void)net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
    }
    struct in_addr mask = { .s_addr = htonl(AP_NETMASK) };
    net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
}

/* WiFi bring-up + watchdog thread.
 *
 * Sequence: wait for the net stack, enable AP mode, assign the static IP,
 * start the DHCP server (clients never get an address without it — learned
 * the hard way), then loop as a watchdog that re-ups the interface and AP
 * if they ever drop. */
static void wifi_ap_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    /* Give the networking subsystem time to start. */
    k_sleep(K_SECONDS(2));

    struct net_if *iface = net_if_get_default();
    if (!iface) {
        app_printk("WiFi: ERROR - no default network interface\r\n");
        return;
    }

    int status = wifi_ap_enable(iface);
    if (status != 0) {
        app_printk("WiFi: ERROR - AP enable failed (%d)\r\n", status);
        return;
    }

    if (!net_if_is_up(iface)) {
        net_if_up(iface);
        k_sleep(K_MSEC(100));
    }

    wifi_ensure_ipv4(iface);

    /* DHCP server: without it clients associate but never get an IP,
     * telnet fails, and the OS eventually drops the WiFi connection.
     * Pool starts at .2 (count = CONFIG_NET_DHCPV4_SERVER_ADDR_COUNT). */
    struct in_addr dhcp_base = { .s_addr = htonl(AP_DHCP_BASE) };
    int dret = net_dhcpv4_server_start(iface, &dhcp_base);
    if (dret != 0) {
        app_printk("WiFi: WARNING - DHCP server start failed (%d)\r\n", dret);
    }

    /* Subscribe to interface/IP events for diagnostics. */
    static struct net_mgmt_event_callback wifi_cb;
    uint64_t mask = NET_EVENT_IF_UP | NET_EVENT_IF_DOWN |
                    NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL;
    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler, mask);
    net_mgmt_add_event_callback(&wifi_cb);

    app_printk("WiFi: AP '%s' up — telnet 192.168.4.1 23\r\n", AP_SSID);

    /* Watchdog: every 5 s make sure the interface, AP and IP are alive. */
    int down_count = 0;
    while (1) {
        k_sleep(K_SECONDS(5));
        if (!net_if_is_up(iface)) {
            down_count++;
            app_printk("WiFi: watchdog - IF DOWN (count=%d)\r\n", down_count);
            net_if_up(iface);
            k_sleep(K_MSEC(200));
            if (!net_if_is_up(iface)) {
                int rs = wifi_ap_enable(iface);
                app_printk("WiFi: watchdog - AP enable retry status: %d\r\n", rs);
            }
            wifi_ensure_ipv4(iface);
        } else {
            down_count = 0;
        }
    }
}

K_THREAD_DEFINE(wifi_ap, 4096, wifi_ap_task, NULL, NULL, NULL, 5, 0, 0);

/* Telnet console server.
 *
 * Field problem this solves: the glider's antenna dips below the surface
 * and the pilot's TCP connection stalls. The old server accepted ONE
 * client and then sat in a blocking recv() on it, with a listen backlog of
 * 1. A half-open socket therefore wedged the whole console — the operating
 * system on the laptop happily opened a new connection, but the glider
 * never got round to accept()ing it, and the pilot stared at a dead
 * terminal until TCP's multi-minute timeout finally reaped the old socket.
 *
 * The server now:
 *   - multiplexes with zsock_poll(), so a new connection is accepted
 *     immediately no matter what state the existing ones are in;
 *   - serves up to NET_CONSOLE_MAX_CLIENTS at once, and evicts the oldest
 *     when a new client arrives and every slot is taken, so a ghost session
 *     can never lock the pilot out;
 *   - enables aggressive TCP keepalive, so a silently dead peer is
 *     detected in ~25 s instead of hours;
 *   - sets a send timeout on every client, so one stalled socket cannot
 *     block console output to the others (the net_console sender thread
 *     writes to all clients from one loop).
 *
 * Reconnecting is the client's job — see tools/tuba_console.py, which
 * reconnects automatically and keeps a local capture.
 */

#define TELNET_PORT            23
#define TELNET_POLL_TIMEOUT_MS 1000
#define TELNET_SEND_TIMEOUT_S  3

/* Keepalive: start probing after 10 s idle, every 5 s, give up after 3.
 * A wedged socket is therefore gone in about 25 s. */
#define TELNET_KEEPALIVE_IDLE_S  10
#define TELNET_KEEPALIVE_INTVL_S 5
#define TELNET_KEEPALIVE_CNT     3

static void telnet_client_close(int *fd)
{
    if (*fd < 0) {
        return;
    }
    net_console_remove(*fd);
    zsock_close(*fd);
    *fd = -1;
}

/* Apply the options that keep one bad client from hurting the others. */
static void telnet_client_configure(int fd)
{
    int on = 1;
    (void)zsock_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));

    /* Per-socket keepalive tuning is only available when Zephyr is built
     * with CONFIG_NET_TCP_KEEPALIVE; without it the defaults still apply. */
#if defined(TCP_KEEPIDLE)
    int idle = TELNET_KEEPALIVE_IDLE_S;
    int intvl = TELNET_KEEPALIVE_INTVL_S;
    int cnt = TELNET_KEEPALIVE_CNT;
    (void)zsock_setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    (void)zsock_setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    (void)zsock_setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif

    /* Bounded sends: the net_console sender thread fans output out to every
     * client in turn, so an unbounded blocking send on one stalled socket
     * would stop mission telemetry reaching all of them. */
    struct zsock_timeval snd_to = { .tv_sec = TELNET_SEND_TIMEOUT_S, .tv_usec = 0 };
    (void)zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
}

static void telnet_server_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    /* Small delay to allow AP/IP to settle. */
    k_sleep(K_SECONDS(3));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TELNET_PORT),
        /* Bind to ANY so binding works even before the IPv4 addr is set. */
        .sin_addr = { .s_addr = htonl(INADDR_ANY) },
    };
    int opt = 1;

    /* Create/bind with one retry: the first attempt can race IP setup. */
    int srv = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (srv < 0) {
            app_printk("TCP: ERROR socket()=%d\r\n", srv);
            return;
        }
        (void)zsock_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            break;
        }
        app_printk("TCP: bind failed%s\r\n", attempt == 0 ? ", retrying" : " again");
        zsock_close(srv);
        srv = -1;
        k_sleep(K_SECONDS(2));
    }
    if (srv < 0) {
        return;
    }

    /* Backlog matches the client cap: a reconnecting pilot should never
     * find the queue full while an old session is still being reaped. */
    if (zsock_listen(srv, NET_CONSOLE_MAX_CLIENTS) != 0) {
        app_printk("TCP: listen failed\r\n");
        zsock_close(srv);
        return;
    }
    app_printk("TCP: telnet console listening on port %d (max %d clients)\r\n",
               TELNET_PORT, NET_CONSOLE_MAX_CLIENTS);

    int clients[NET_CONSOLE_MAX_CLIENTS];
    for (int i = 0; i < NET_CONSOLE_MAX_CLIENTS; i++) {
        clients[i] = -1;
    }

    while (1) {
        struct zsock_pollfd fds[1 + NET_CONSOLE_MAX_CLIENTS];
        int slot_of[1 + NET_CONSOLE_MAX_CLIENTS];
        int nfds = 0;

        fds[nfds].fd = srv;
        fds[nfds].events = ZSOCK_POLLIN;
        fds[nfds].revents = 0;
        slot_of[nfds] = -1;
        nfds++;

        for (int i = 0; i < NET_CONSOLE_MAX_CLIENTS; i++) {
            if (clients[i] >= 0) {
                fds[nfds].fd = clients[i];
                fds[nfds].events = ZSOCK_POLLIN;
                fds[nfds].revents = 0;
                slot_of[nfds] = i;
                nfds++;
            }
        }

        int ret = zsock_poll(fds, nfds, TELNET_POLL_TIMEOUT_MS);
        if (ret < 0) {
            k_sleep(K_MSEC(200));
            continue;
        }
        if (ret == 0) {
            continue;   /* idle tick */
        }

        /* --- Existing clients first, so a slot freed by a dead session is
         *     available to the accept() below in this same pass. --- */
        for (int k = 1; k < nfds; k++) {
            int i = slot_of[k];
            if (fds[k].revents == 0) {
                continue;
            }
            if (fds[k].revents & (ZSOCK_POLLERR | ZSOCK_POLLHUP | ZSOCK_POLLNVAL)) {
                app_printk("TCP: client disconnected (link lost)\r\n");
                telnet_client_close(&clients[i]);
                continue;
            }
            if (fds[k].revents & ZSOCK_POLLIN) {
                char buf[128];
                int n = zsock_recv(clients[i], buf, sizeof(buf), 0);
                if (n <= 0) {
                    app_printk("TCP: client disconnected\r\n");
                    telnet_client_close(&clients[i]);
                    continue;
                }
                /* Normalize line endings to CR for consistent echo. */
                for (int j = 0; j < n; j++) {
                    if (buf[j] == '\n') buf[j] = '\r';
                }
                if (zsock_send(clients[i], buf, n, 0) < 0) {
                    telnet_client_close(&clients[i]);
                    continue;
                }
                net_console_ingest_bytes(buf, n);
            }
        }

        /* --- New connections --- */
        if (fds[0].revents & ZSOCK_POLLIN) {
            struct sockaddr_in cli;
            socklen_t clilen = sizeof(cli);
            int fd = zsock_accept(srv, (struct sockaddr *)&cli, &clilen);
            if (fd >= 0) {
                int slot = -1;
                for (int i = 0; i < NET_CONSOLE_MAX_CLIENTS; i++) {
                    if (clients[i] < 0) { slot = i; break; }
                }
                if (slot < 0) {
                    /* All slots taken. The oldest is the most likely ghost
                     * (a session the pilot already gave up on), so evict it
                     * rather than refuse the pilot who is trying to get back
                     * in right now. */
                    app_printk("TCP: client limit reached — evicting oldest session\r\n");
                    telnet_client_close(&clients[0]);
                    for (int i = 0; i < NET_CONSOLE_MAX_CLIENTS - 1; i++) {
                        clients[i] = clients[i + 1];
                    }
                    slot = NET_CONSOLE_MAX_CLIENTS - 1;
                    clients[slot] = -1;
                }

                telnet_client_configure(fd);
                clients[slot] = fd;
                net_console_add(fd);
                app_printk("TCP: client connected\r\n");

                /* Greeting so the client immediately sees it is in the
                 * right place — and, after a reconnect, that it is back. */
                char banner[200];
                snprintk(banner, sizeof(banner),
                         "\r\nTuba-Glider console (build %d, %s)\r\n"
                         "Press ENTER to interact with the menu.\r\n\r\n",
                         BUILD_NUMBER, BUILD_DATE);
                if (zsock_send(fd, banner, strlen(banner), 0) < 0) {
                    telnet_client_close(&clients[slot]);
                }
            }
        }
    }
}

/* Generous stack: socket I/O + printf formatting on Xtensa. */
K_THREAD_DEFINE(telnet_srv, 4096, telnet_server_task, NULL, NULL, NULL, 6, 0, 0);
#endif /* CONFIG_WIFI */

/* ===================================================================
 * Leak monitor thread
 *
 * Samples the leak ADC once per second, independent of what the UI is
 * doing. Alerts on first detection and re-alerts every 30 s while wet.
 * The threshold is a mission parameter ("Leak threshold [mV]").
 * =================================================================== */
static void leak_monitor_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    k_sleep(K_SECONDS(5));  /* let the system fully start */

    bool was_leaking = false;
    int64_t last_alert_ms = 0;

    while (1) {
        k_sleep(K_SECONDS(1));

        /* Threshold 0 = monitoring disabled (default: no sensor installed;
         * a floating ADC pin would false-alarm otherwise). */
        int32_t thresh = (int32_t)app_params_get()->leak_threshold_mv;
        if (thresh == 0) {
            was_leaking = false;
            continue;
        }

        int32_t mv;
        if (leak_detector_read_mv(&mv) != 0) {
            continue;
        }

        bool leaking = (mv < thresh);
        int64_t now_ms = k_uptime_get();

        if (leaking) {
            /* Alert on first detection, then repeat every 30 s. */
            if (!was_leaking || (now_ms - last_alert_ms) >= 30000) {
                app_printk("\r\n[LEAK] *** LEAK DETECTED *** %d mV (threshold %d mV)\r\n",
                           mv, thresh);
                last_alert_ms = now_ms;
            }
        } else if (was_leaking) {
            app_printk("\r\n[LEAK] Leak cleared (%d mV)\r\n", mv);
        }

        was_leaking = leaking;
    }
}

/* 2 KB stack: app_printk formats into a 256-byte buffer and Xtensa call
 * frames are wide; 1 KB was borderline. */
K_THREAD_DEFINE(leak_mon, 2048, leak_monitor_task, NULL, NULL, NULL, 8, 0, 0);

/* ===================================================================
 * State machine dispatch table
 *
 * One row per state. Any hook may be NULL:
 *   on_entry — print the menu/prompt, start workers
 *   on_exit  — cleanup when leaving
 *   on_event — react to async events; returns the next state
 * =================================================================== */
typedef struct {
    const char *name;                          /* for debug/trace prints */
    void (*on_entry)(void);
    void (*on_exit)(void);
    state_id_t (*on_event)(const event_t *e);
} state_ops_t;

static const state_ops_t STATE_TABLE[ST__COUNT] = {
    [ST_POWERUP_WAIT] = { "POWERUP_WAIT", on_entry_POWERUP_WAIT, on_exit_POWERUP_WAIT, on_event_POWERUP_WAIT },
    [ST_MENU]         = { "MENU",         on_entry_MENU,         NULL, on_event_MENU         },
    [ST_HWTEST_MENU]  = { "HWTEST_MENU",  on_entry_HWTEST_MENU,  NULL, on_event_HWTEST_MENU  },
    [ST_SENSORS_MENU] = { "SENSORS_MENU", on_entry_SENSORS_MENU, NULL, on_event_SENSORS_MENU },
    [ST_TEMP_MENU]    = { "TEMP_MENU",    on_entry_TEMP_MENU,    NULL, on_event_TEMP_MENU    },
    [ST_PARAMS_MENU]  = { "PARAMS_MENU",  on_entry_PARAMS_MENU,  NULL, on_event_PARAMS_MENU  },
    [ST_PARAM_INPUT]  = { "PARAM_INPUT",  NULL /* prompt printed by PARAMS_MENU handler */,
                          NULL, on_event_PARAM_INPUT },
    [ST_PR_MENU]      = { "PR_MENU",      on_entry_PR_MENU,      NULL, on_event_PR_MENU      },
    [ST_PR_INPUT]     = { "PR_INPUT",     NULL /* prompt printed by PR_MENU handler */,
                          NULL, on_event_PR_INPUT },
    [ST_PUMP_INPUT]   = { "PUMP_INPUT",   NULL /* prompt printed by HWTEST handler */,
                          NULL, on_event_PUMP_INPUT },
    [ST_LIMIT_TEST]   = { "LIMIT_TEST",   NULL, NULL, NULL },   /* legacy, unused */
    [ST_RECOVERY]     = { "RECOVERY",     on_entry_RECOVERY,     NULL, on_event_RECOVERY     },
    [ST_DEPLOYED]     = { "DEPLOYED",     on_entry_DEPLOYED,     NULL, on_event_DEPLOYED     },
    [ST_SIMULATE]     = { "SIMULATE",     on_entry_SIMULATE,     NULL, on_event_SIMULATE     },
    [ST_COMPASS_MENU] = { "COMPASS_MENU", on_entry_COMPASS_MENU, NULL, on_event_COMPASS_MENU },
    [ST_OTA_MENU]     = { "OTA_MENU",     on_entry_OTA_MENU,     NULL, on_event_OTA_MENU     },
};

/* Perform a state transition: run the old state's exit hook and the new
 * state's entry hook. Returns the new state. ST__COUNT = "no change". */
static state_id_t transition(state_id_t from, state_id_t to)
{
    if (to == ST__COUNT || to == from) {
        return from;    /* no transition requested */
    }
    if (STATE_TABLE[from].on_exit) {
        STATE_TABLE[from].on_exit();
    }
    if (STATE_TABLE[to].on_entry) {
        STATE_TABLE[to].on_entry();
    }
    return to;
}

/* ===================================================================
 * Boot + main loop
 * =================================================================== */
int main(void) {
    /* --- Boot banner --- */
    printk("\r\n=== Tuba Glider — build %d (%s) ===\r\n", BUILD_NUMBER, BUILD_DATE);
    k_sleep(K_MSEC(50));

    /* --- Persistent parameters (also applies console input mode) --- */
    (void)app_params_init();

    /* --- Wall clock: build-date default, re-synced on every GPS fix --- */
    app_time_init();

    /* --- Sensor bus: probe every expected sensor and report --- */
#if defined(CONFIG_I2C)
    sensors_report();
#endif

    /* --- Actuators & safety hardware --- */
    (void)pump_init();
    (void)motors_init();
    (void)limit_switches_init();
    (void)ota_simple_init();
    (void)leak_detector_init();

    printk("Main loop starting...\r\n");
    k_sleep(K_MSEC(100));

    /* --- Timers: startup auto-deploy countdown + 20 Hz UI tick --- */
    k_timer_start(&startup_timeout, K_SECONDS(STARTUP_TIMEOUT_SEC), K_NO_WAIT);
    k_timer_start(&ui_tick, K_MSEC(50), K_MSEC(50));

    /* --- State machine --- */
    state_id_t state = ST_POWERUP_WAIT;
    if (STATE_TABLE[state].on_entry) {
        STATE_TABLE[state].on_entry();
    }

    while (1) {
        /* 1) Typed input lines (from either console). */
        bool enter_only = false;
        if (read_line_nonblock(line_buf, sizeof(line_buf), &enter_only)) {
            state = transition(state, ui_handle_line(state, line_buf));
        } else if (enter_only) {
            /* Bare ENTER is delivered as an event (used by POWERUP_WAIT). */
            post_event(EVT_ENTER);
        }

        /* 2) Async events (tick/timeout/enter), with a short wait that
         *    doubles as the main-loop pacing delay. */
        event_t event;
        if (k_msgq_get(&evt_q, &event, K_MSEC(10)) == 0) {
            state_id_t next = STATE_TABLE[state].on_event
                            ? STATE_TABLE[state].on_event(&event)
                            : state;

            /* SAFETY: react to limit switches on every event cycle, so a
             * pitch overtravel is stopped no matter which menu is open. */
            limit_switches_check_and_stop();

            state = transition(state, next);
        }
    }
    return 0;
}
