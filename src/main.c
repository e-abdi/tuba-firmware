#include <zephyr/kernel.h>
#include "net_console.h"
#include <zephyr/sys/printk.h>
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
#include "ui_menu.h"
#include "hw_motors.h"
#include "hw_pump.h"
#include "hw_limit_switches.h"
#include "app_params.h"

/* Console UART - using _OR_NULL to avoid crash if not available */
static const struct device *const uart_console = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));



/* ---- I2C bus scan helpers ---- */
#if defined(CONFIG_I2C)

/* Resolve nodes for i2c0 and i2c1 (either by nodelabel or alias) */
#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c0), okay)
#define I2C0_NODE DT_NODELABEL(i2c0)
#elif DT_NODE_HAS_STATUS(DT_ALIAS(i2c0), okay)
#define I2C0_NODE DT_ALIAS(i2c0)
#endif

#if DT_NODE_HAS_STATUS(DT_NODELABEL(i2c1), okay)
#define I2C1_NODE DT_NODELABEL(i2c1)
#elif DT_NODE_HAS_STATUS(DT_ALIAS(i2c1), okay)
#define I2C1_NODE DT_ALIAS(i2c1)
#endif

/* Probe helper: try a 1-byte read using the message API; if not available, fall back to i2c_read().
 * We only care about whether the address ACKs. */


static int i2c_addr_probe(const struct device *bus, uint16_t addr)
{
    /* Try a 0-byte write followed by a 1-byte read. Many controllers treat this as an address probe.
     * If the controller rejects 0-byte writes, this may return -EINVAL/-ENOTSUP; we'll treat that as NACK. */
    uint8_t byte = 0;
    int ret = i2c_write_read(bus, addr, NULL, 0, &byte, 1);
    return ret;
}

static void scan_one_bus(const struct device *bus, const char *name)
{
    if (!bus) {
        app_printk("%s: not present in DT\r\n", name);
        return;
    }
    if (!device_is_ready(bus)) {
        app_printk("%s: device not ready\r\n", name);
        return;
    }
    app_printk("%s: scanning...\r\n", name);
    int found = 0;
    for (uint16_t addr = 0x03; addr <= 0x77; ++addr) {
        int ret = i2c_addr_probe(bus, addr);
        if (ret == 0) {
            app_printk("  - 0x%02x\r\n", addr);
            ++found;
        }
        k_busy_wait(50);
    }
    if (!found) {
        app_printk("%s: no devices found\r\n", name);
    }
}

static void scan_i2c_buses(void)
{
#ifdef I2C0_NODE
    const struct device *i2c0 = DEVICE_DT_GET_OR_NULL(I2C0_NODE);
#else
    const struct device *i2c0 = NULL;
#endif
#ifdef I2C1_NODE
    const struct device *i2c1 = DEVICE_DT_GET_OR_NULL(I2C1_NODE);
#else
    const struct device *i2c1 = NULL;
#endif

    scan_one_bus(i2c0, "i2c0");
    scan_one_bus(i2c1, "i2c1");
}
#else
static void scan_i2c_buses(void) { /* I2C not enabled */ }
#endif

static inline bool uart_ready(void) { return device_is_ready(uart_console); }
static bool uart_getch(uint8_t *out) {
    if (!uart_ready()) return false;
    return (uart_poll_in(uart_console, (unsigned char *)out) == 0);
}

/* Timers & events */
static void timeout_cb(struct k_timer *tmr); K_TIMER_DEFINE(startup_timeout, timeout_cb, NULL);
static void tick_cb(struct k_timer *tmr);    K_TIMER_DEFINE(ui_tick, tick_cb, NULL);
K_MSGQ_DEFINE(evt_q, sizeof(event_t), 8, 4);

static inline void post_event(event_id_t id) { event_t e = {.id=id}; (void)k_msgq_put(&evt_q,&e,K_NO_WAIT); }
static void timeout_cb(struct k_timer *tmr){ARG_UNUSED(tmr);post_event(EVT_TIMEOUT);}
static void tick_cb(struct k_timer *tmr){ARG_UNUSED(tmr);post_event(EVT_TICK);}

/* Line buffer */
static char line_buf[APP_LINE_MAX];
/* Make WiFi telnet the primary console input by default */
#define USE_WIFI_CONSOLE
static bool read_line_nonblock(char *buf, size_t buflen, bool *enter_only) {
    *enter_only = false;
#ifdef USE_WIFI_CONSOLE
    /* WiFi console: poll for a line from net console */
    char line_tmp[128];
    if (net_console_poll_line(line_tmp, sizeof(line_tmp), K_NO_WAIT)) {
        size_t n = strnlen(line_tmp, sizeof(line_tmp));
        if (n == 0) {
            *enter_only = true;
            /* Echo newline to UART for consistency */
            printk("\r\n");
            return false;
        }
        if (n >= buflen) n = buflen-1;
        memcpy(buf, line_tmp, n);
        buf[n] = '\0';
        /* Echo line to UART console too */
        printk("%s\r\n", buf);
        return true;
    }
    return false;
#else
    /* UART console: nonblocking reader */
    static size_t idx = 0;
    uint8_t ch;
    while (uart_getch(&ch)) {
        if (ch=='\r'||ch=='\n') {
            if (idx > 0) {
                printk("\r\n");
                buf[idx]='\0';
                idx=0;
                return true;
            } else {
                printk("\r\n");
                *enter_only = true;
                return false;
            }
        } else if (idx<buflen-1) {
            buf[idx++]=(char)ch;
            printk("%c", ch);
        }
    }
    return false;
#endif
}

/* Current motor (used in PR_INPUT) */
enum motor_id current_motor = MOTOR_ROLL;

/* ------------------- WiFi Console Support (AP Mode) ------------------- */
#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

/* File-scope WiFi event logger */
static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
    ARG_UNUSED(cb);
    switch (event) {
    case NET_EVENT_IF_UP:
        app_printk("WiFi: IF UP\r\n");
        break;
    case NET_EVENT_IF_DOWN:
        app_printk("WiFi: IF DOWN\r\n");
        break;
    case NET_EVENT_IPV4_ADDR_ADD:
        app_printk("WiFi: IPv4 address added\r\n");
        break;
    case NET_EVENT_IPV4_ADDR_DEL:
        app_printk("WiFi: IPv4 address removed\r\n");
        break;
    default:
        app_printk("WiFi: net event 0x%08x\r\n", event);
        break;
    }
    ARG_UNUSED(iface);
}

static void wifi_ap_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    
    app_printk("WiFi: Task started\r\n");
    
    /* Give networking subsystem time to start */
    k_sleep(K_SECONDS(2));
    app_printk("WiFi: Waited 2 seconds for networking\r\n");
    
    struct net_if *iface = net_if_get_default();
    if (!iface) {
        app_printk("WiFi: ERROR - No default network interface available\r\n");
        return;
    }
    
    app_printk("WiFi: Default interface obtained\r\n");
    
    /* Try to configure WiFi AP mode using wifi_ap_config */
    app_printk("WiFi: Attempting to enable AP mode...\r\n");

    struct wifi_connect_req_params ap_cfg = {
        .ssid = (uint8_t *)"Tuba-Glider",
        .ssid_length = 11,
        .channel = 6,
        .security = WIFI_SECURITY_TYPE_NONE,
    };

    int status = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, 
                          &ap_cfg, sizeof(ap_cfg));
    
    app_printk("WiFi: AP enable returned status: %d\r\n", status);
    
    if (status != 0) {
        app_printk("WiFi: ERROR - AP enable failed with code %d\r\n", status);
        return;
    }
    
    app_printk("WiFi: AP enabled successfully\r\n");
    
    /* Check interface status */
    bool is_up = net_if_is_up(iface);
    app_printk("WiFi: Interface is %s\r\n", is_up ? "UP" : "DOWN");
    
    /* Get interface index and info */
    int if_index = net_if_get_by_iface(iface);
    app_printk("WiFi: Interface index: %d\r\n", if_index);
    
    /* Bring interface up if needed */
    if (!is_up) {
        app_printk("WiFi: Bringing interface up...\r\n");
        net_if_up(iface);
        k_sleep(K_MSEC(100));
        is_up = net_if_is_up(iface);
        app_printk("WiFi: After net_if_up, interface is %s\r\n", is_up ? "UP" : "DOWN");
    }
    
    app_printk("WiFi: AP 'Tuba-Glider' is broadcasting on channel 6\r\n");
    
    /* Set static IP address manually (idempotent) */
    struct in_addr addr;
    addr.s_addr = htonl(0xc0a80401);  /* 192.168.4.1 */
    
    app_printk("WiFi: Setting IP address to 192.168.4.1...\r\n");
    struct net_if *tmp_iface = NULL;
    struct net_if_addr *ifa_lookup = net_if_ipv4_addr_lookup(&addr, &tmp_iface);
    if (!ifa_lookup) {
        struct net_if_addr *ifa = net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
        app_printk("WiFi: net_if_ipv4_addr_add %s\r\n", ifa ? "OK" : "ERR");
    } else {
        app_printk("WiFi: IPv4 address already present\r\n");
    }
    
    /* Set netmask (255.255.255.0) */
    struct in_addr netmask;
    netmask.s_addr = htonl(0xffffff00);
    net_if_ipv4_set_netmask(iface, &netmask);
    app_printk("WiFi: Netmask set to 255.255.255.0\r\n");
    
    /* Simple check: print interface name and L2 type */
    app_printk("WiFi: Interface name: %s\r\n", net_if_get_device(iface)->name);
    
    app_printk("WiFi: Setup complete, awaiting connections on 192.168.4.1\r\n");

    /* Register net_mgmt event logging for interface/IP changes */
    static struct net_mgmt_event_callback wifi_cb;
    uint64_t mask = NET_EVENT_IF_UP | NET_EVENT_IF_DOWN |
                    NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL;
    net_mgmt_init_event_callback(&wifi_cb, wifi_event_handler, mask);
    net_mgmt_add_event_callback(&wifi_cb);
    
    /* Try to bind a UDP socket to verify network stack */
    app_printk("WiFi: Testing network stack with UDP socket...\r\n");
    int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        app_printk("WiFi: ERROR - zsock_socket() failed with error %d\r\n", -sock);
    } else {
        app_printk("WiFi: Socket created successfully (fd=%d)\r\n", sock);
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9000);
        addr.sin_addr.s_addr = htonl(0xc0a80401);  /* 192.168.4.1 */
        
        int ret = zsock_bind(sock, (struct sockaddr *)&addr, sizeof(addr));
        app_printk("WiFi: zsock_bind() returned: %d\r\n", ret);
        
        if (ret == 0) {
            app_printk("WiFi: SUCCESS - UDP socket bound to 192.168.4.1:9000\r\n");
        } else {
            app_printk("WiFi: ERROR - zsock_bind() failed with error %d\r\n", ret);
        }
        
        zsock_close(sock);
    }
    
    /* WiFi watchdog: periodically ensure interface/AP/IP stays up */
    int down_count = 0;
    bool ap_enabled_flag = (status == 0);
    while (1) {
        k_sleep(K_SECONDS(5));
        if (!net_if_is_up(iface)) {
            down_count++;
            app_printk("WiFi: watchdog - IF DOWN (count=%d)\r\n", down_count);
            net_if_up(iface);
            k_sleep(K_MSEC(200));
            if (!net_if_is_up(iface)) {
                /* Re-enable AP mode */
                struct wifi_connect_req_params ap_re = {
                    .ssid = (uint8_t *)"Tuba-Glider",
                    .ssid_length = 11,
                    .channel = 6,
                    .security = WIFI_SECURITY_TYPE_NONE,
                };
                int rs = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface,
                                  &ap_re, sizeof(ap_re));
                app_printk("WiFi: watchdog - AP enable retry status: %d\r\n", rs);
                ap_enabled_flag = (rs == 0);
            }
            /* Ensure IPv4 address/netmask configured */
            struct in_addr waddr; waddr.s_addr = htonl(0xc0a80401);
            struct net_if *tmp2 = NULL;
            if (!net_if_ipv4_addr_lookup(&waddr, &tmp2)) {
                (void)net_if_ipv4_addr_add(iface, &waddr, NET_ADDR_MANUAL, 0);
            }
            struct in_addr wmask; wmask.s_addr = htonl(0xffffff00);
            net_if_ipv4_set_netmask(iface, &wmask);
        } else {
            down_count = 0;
        }
    }
}

K_THREAD_DEFINE(wifi_ap, 4096, wifi_ap_task, NULL, NULL, NULL, 5, 0, 0);
#endif

#ifdef CONFIG_WIFI
#include "net_console.h"
/* Simple TCP echo server for connectivity testing (telnet 192.168.4.1 23) */
static void tcp_echo_server_task(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    /* Small delay to allow AP/IP to settle */
    k_sleep(K_SECONDS(3));

    int srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) {
        app_printk("TCP: ERROR socket()=%d\r\n", srv);
        return;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(23);
    /* Bind to any local address to avoid failures before IPv4 is set */
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int opt = 1;
    (void)zsock_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        app_printk("TCP: bind failed\r\n");
        /* Retry later in case IP was not ready yet */
        zsock_close(srv);
        k_sleep(K_SECONDS(2));
        srv = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (srv < 0) {
            app_printk("TCP: ERROR socket()=%d\r\n", srv);
            return;
        }
        (void)zsock_setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (zsock_bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            app_printk("TCP: bind failed again\r\n");
            zsock_close(srv);
            return;
        }
    }
    if (zsock_listen(srv, 1) != 0) {
        app_printk("TCP: listen failed\r\n");
        zsock_close(srv);
        return;
    }
    app_printk("TCP: Listening on 0.0.0.0:23 (telnet)\r\n");

    while (1) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int fd = zsock_accept(srv, (struct sockaddr *)&cli, &clilen);
        if (fd < 0) {
            k_sleep(K_MSEC(200));
            continue;
        }
        app_printk("TCP: Client connected\r\n");
        /* Enable TCP keepalive for robustness */
        int ka = 1; (void)zsock_setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
        net_console_add(fd);

        /* Send a banner so the client sees immediate output */
        const char *banner =
            "\r\nTuba-Glider WiFi console (echo test)\r\n"
            "Type and press ENTER — your input will echo.\r\n"
            "Note: Serial UART is the primary console; this TCP port is a simple echo.\r\n\r\n";
        {
            int sret = zsock_send(fd, banner, strlen(banner), 0);
            if (sret < 0) {
                app_printk("TCP: send banner failed (%d)\r\n", sret);
                zsock_close(fd);
                net_console_remove(fd);
                continue;
            }
        }

        char buf[128];
        while (1) {
            int n = zsock_recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            /* Normalize line endings to CRLF for telnet clients */
            for (int i = 0; i < n; i++) {
                if (buf[i] == '\n') buf[i] = '\r';
            }
            int sret = zsock_send(fd, buf, n, 0);
            if (sret < 0) {
                app_printk("TCP: send failed (%d)\r\n", sret);
                break;
            }
            /* Feed incoming bytes into net-console input aggregator */
            extern void net_console_ingest_bytes(const char *buf, size_t len);
            net_console_ingest_bytes(buf, n);
        }
        zsock_close(fd);
        net_console_remove(fd);
        app_printk("TCP: Client disconnected\r\n");
    }
}

/* Increase stack to avoid overflow during socket I/O and formatting */
K_THREAD_DEFINE(tcp_echo, 4096, tcp_echo_server_task, NULL, NULL, NULL, 6, 0, 0);
#endif

/* ------------------- Main loop ------------------- */
void main(void) {
    /* Boot banner */
    printk("=== ESP32 Boot ===\r\n");
    k_sleep(K_MSEC(50));

    /* Init settings (NVS) and app params */
    int r = settings_subsys_init();
    printk("Settings init: %d\r\n", r);
    (void)app_params_init();
    app_printk("Params: initialized and loaded\r\n");

#if defined(CONFIG_I2C)
    app_printk("I2C: scanning buses...\r\n");
    scan_i2c_buses();
#endif

    /* Init motors & pump */
    printk("Initializing pump...\r\n");
    (void)pump_init();
    printk("Pump initialized\r\n");
    printk("Initializing motors...\r\n");
    (void)motors_init();
    printk("Motors initialized\r\n");
    printk("Initializing limit switches...\r\n");
    (void)limit_switches_init();
    printk("Limit switches initialized\r\n");

    printk("Main loop starting...\r\n");
    k_sleep(K_MSEC(100));

    /* Start timers */
    /* Match startup timeout to STARTUP_TIMEOUT_SEC (10 min now) */
    k_timer_start(&startup_timeout, K_SECONDS(STARTUP_TIMEOUT_SEC), K_NO_WAIT);
    k_timer_start(&ui_tick, K_MSEC(50), K_MSEC(50));

    /* Initialize state machine to POWERUP_WAIT */
    state_id_t state = ST_POWERUP_WAIT;
    on_entry_POWERUP_WAIT();

    /* Main event loop */
    while (1) {
        /* FIRST: Always try to read input (non-blocking) */
        bool enter_only = false;
        if (read_line_nonblock(line_buf, sizeof(line_buf), &enter_only)) {
            /* User entered text + ENTER */
            state_id_t new_state = ui_handle_line(state, line_buf);
            if (new_state != ST__COUNT && new_state != state) {
                if (state == ST_POWERUP_WAIT) on_exit_POWERUP_WAIT();
                state = new_state;
                
                switch (state) {
                    case ST_POWERUP_WAIT:
                        on_entry_POWERUP_WAIT();
                        break;
                    case ST_MENU:
                        on_entry_MENU();
                        break;
                    case ST_HWTEST_MENU:
                        on_entry_HWTEST_MENU();
                        break;
                    case ST_PARAMS_MENU:
                        on_entry_PARAMS_MENU();
                        break;
                    case ST_PR_MENU:
                        on_entry_PR_MENU();
                        break;
                    case ST_RECOVERY:
                        on_entry_RECOVERY();
                        break;
                    case ST_DEPLOYED:
                        on_entry_DEPLOYED();
                        break;
                    case ST_SIMULATE:
                        on_entry_SIMULATE();
                        break;
                    case ST_COMPASS_MENU:
                        on_entry_COMPASS_MENU();
                        break;
                    default:
                        break;
                }
            }
        } else if (enter_only) {
            /* User pressed just ENTER - post EVT_ENTER event */
            post_event(EVT_ENTER);
        }
        
        /* SECOND: Check for queued events with very short timeout */
        event_t event;
        int ret = k_msgq_get(&evt_q, &event, K_MSEC(10));
        
        if (ret == 0) {
            /* Process event based on current state */
            state_id_t new_state = ST__COUNT;
            
            switch (state) {
                case ST_POWERUP_WAIT:
                    new_state = on_event_POWERUP_WAIT(&event);
                    break;
                case ST_MENU:
                    new_state = on_event_MENU(&event);
                    break;
                case ST_HWTEST_MENU:
                    new_state = on_event_HWTEST_MENU(&event);
                    break;
                case ST_PARAMS_MENU:
                    new_state = on_event_PARAMS_MENU(&event);
                    break;
                case ST_PARAM_INPUT:
                    new_state = on_event_PARAM_INPUT(&event);
                    break;
                case ST_PR_MENU:
                    new_state = on_event_PR_MENU(&event);
                    break;
                case ST_PR_INPUT:
                    new_state = on_event_PR_INPUT(&event);
                    break;
                case ST_PUMP_INPUT:
                    new_state = on_event_PUMP_INPUT(&event);
                    break;
                case ST_RECOVERY:
                    new_state = on_event_RECOVERY(&event);
                    break;
                case ST_DEPLOYED:
                    new_state = on_event_DEPLOYED(&event);
                    break;
                case ST_SIMULATE:
                    new_state = on_event_SIMULATE(&event);
                    break;
                case ST_COMPASS_MENU:
                    /* COMPASS_MENU not yet implemented; stay in current state */
                    new_state = state;
                    break;
                default:
                    break;
            }
            
            /* SAFETY: Check and handle any triggered limit switches (motor stop) */
            limit_switches_check_and_stop();
            
            /* Transition to new state if needed */
            if (new_state != ST__COUNT && new_state != state) {
                if (state == ST_POWERUP_WAIT) on_exit_POWERUP_WAIT();
                
                state = new_state;
                
                switch (state) {
                    case ST_POWERUP_WAIT:
                        on_entry_POWERUP_WAIT();
                        break;
                    case ST_MENU:
                        on_entry_MENU();
                        break;
                    case ST_HWTEST_MENU:
                        on_entry_HWTEST_MENU();
                        break;
                    case ST_PARAMS_MENU:
                        on_entry_PARAMS_MENU();
                        break;
                    case ST_PR_MENU:
                        on_entry_PR_MENU();
                        break;
                    case ST_RECOVERY:
                        on_entry_RECOVERY();
                        break;
                    case ST_DEPLOYED:
                        on_entry_DEPLOYED();
                        break;
                    case ST_COMPASS_MENU:
                        on_entry_COMPASS_MENU();
                        break;
                    default:
                        break;
                }
            }
        }
    }
}