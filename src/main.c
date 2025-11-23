#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/usb/usb_device.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "app_events.h"
#include "app_limits.h"
#include "ui_menu.h"
#include "hw_motors.h"
#include "hw_pump.h"
#include "app_params.h"
/* Console UART */
static const struct device *const uart_console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));



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
    const struct device *i2c1 = DEVICE_DT_GET_OR_NULL(I2C0_NODE);
#else
    const struct device *i2c1 = NULL;
#endif
#ifdef I2C1_NODE
    const struct device *i2c0 = DEVICE_DT_GET_OR_NULL(I2C1_NODE);
#else
    const struct device *i2c0 = NULL;
#endif

    scan_one_bus(i2c1, "i2c1");
    scan_one_bus(i2c0, "i2c0");
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
static bool read_line_nonblock(char *buf, size_t buflen) {
    static size_t idx = 0;
    uint8_t ch;
    while (uart_getch(&ch)) {
        if (ch=='\r'||ch=='\n') {
            if (idx>0) { buf[idx]='\0'; idx=0; return true; }
            else { post_event(EVT_ENTER); continue; }
        } else { if (idx<buflen-1) buf[idx++]=(char)ch; }
    }
    return false;
}

/* Current motor (used in PR_INPUT) */
enum motor_id current_motor = MOTOR_ROLL;

/* ------------------- Main loop ------------------- */
void main(void) {
#if defined(CONFIG_USB_DEVICE_STACK)
    /* Initialize USB and wait for console to be ready */
    if (usb_enable(NULL) != 0) {
        return;
    }
    /* Wait for USB console to be ready and flash to stabilize */
    k_sleep(K_MSEC(1000));
#endif

    if (!uart_ready()) {
        while (1) { app_printk("Console UART not ready!\r\n"); k_sleep(K_SECONDS(2)); }
    }

    /* Initialize parameters AFTER USB is ready so we can see debug output */
    app_printk("=== Tuba AUV Initializing ===\r\n");
    (void)app_params_init();
    app_printk("=== Initialization Complete ===\r\n");

    uint8_t dummy; (void)uart_poll_in(uart_console, &dummy);
    /* Scan I2C buses at startup */
    scan_i2c_buses();

    (void)motors_init();
    (void)pump_init();
    state_id_t state = ST_POWERUP_WAIT;
    on_entry_POWERUP_WAIT();

    /* Start POWERUP timers */
    k_timer_start(&startup_timeout, K_SECONDS(STARTUP_TIMEOUT_SEC), K_NO_WAIT);
    k_timer_start(&ui_tick, K_SECONDS(1), K_SECONDS(1));

    while (1) {
        if (read_line_nonblock(line_buf, sizeof(line_buf))) {
            state = ui_handle_line(state, line_buf);
        }

        event_t e;
        while (k_msgq_get(&evt_q,&e,K_NO_WAIT)==0) {
            state_id_t next = state;
            switch(state) {
                case ST_POWERUP_WAIT: next=on_event_POWERUP_WAIT(&e); break;
                case ST_MENU:         next=on_event_MENU(&e); break;
                case ST_HWTEST_MENU:  next=on_event_HWTEST_MENU(&e); break;
                case ST_PARAMS_MENU:  next=on_event_PARAMS_MENU(&e); break;
                case ST_PARAM_INPUT:  next=on_event_PARAM_INPUT(&e); break;
                case ST_PR_MENU:      next=on_event_PR_MENU(&e); break;
                case ST_PR_INPUT:     next=on_event_PR_INPUT(&e); break;
                case ST_PUMP_INPUT:   next=on_event_PUMP_INPUT(&e); break;
                case ST_RECOVERY:     next=on_event_RECOVERY(&e); break;
                case ST_DEPLOYED:     next=on_event_DEPLOYED(&e); break;
                default: break;
            }
            if (next!=state) {
                if (state == ST_POWERUP_WAIT) {
                    k_timer_stop(&startup_timeout);
                    k_timer_stop(&ui_tick);
                }
                state=next;
                switch(state){
                    case ST_MENU: on_entry_MENU(); break;
                    case ST_HWTEST_MENU: on_entry_HWTEST_MENU(); break;
                    case ST_PARAMS_MENU: on_entry_PARAMS_MENU(); break;
                    case ST_PR_MENU: on_entry_PR_MENU(); break;
                    case ST_RECOVERY: on_entry_RECOVERY(); break;
                    case ST_DEPLOYED: on_entry_DEPLOYED(); break;
                    default: break;
                }
            }
        }
        k_sleep(K_MSEC(5));
    }
}