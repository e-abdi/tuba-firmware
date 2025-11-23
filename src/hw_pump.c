#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "hw_pump.h"

/* --- Devicetree bindings for Pump --- */
#define HAVE_PUMP_IN1 DT_NODE_HAS_STATUS(DT_ALIAS(pump_in1), okay)
#define HAVE_PUMP_IN2 DT_NODE_HAS_STATUS(DT_ALIAS(pump_in2), okay)

#if HAVE_PUMP_IN1
static const struct gpio_dt_spec gpio_pump_in1 = GPIO_DT_SPEC_GET(DT_ALIAS(pump_in1), gpios);
#endif
#if HAVE_PUMP_IN2
static const struct gpio_dt_spec gpio_pump_in2 = GPIO_DT_SPEC_GET(DT_ALIAS(pump_in2), gpios);
#endif

struct pump_ctx {
    const struct gpio_dt_spec *in1;
    const struct gpio_dt_spec *in2;
    struct k_work_delayable stop_work;
    volatile bool running;
    int32_t position_sec;
};

static struct pump_ctx pump = {
#if HAVE_PUMP_IN1 && HAVE_PUMP_IN2
    .in1 = &gpio_pump_in1, .in2 = &gpio_pump_in2,
#else
    .in1 = NULL, .in2 = NULL,
#endif
    .running = false,
    .position_sec = 0,
};

/* --- Helpers --- */
static void pump_all_low(const struct pump_ctx *p)
{
    if (!p || !p->in1 || !p->in2) return;
    gpio_pin_set_dt(p->in1, 0);
    gpio_pin_set_dt(p->in2, 0);
}

static void pump_stop_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct pump_ctx *p = CONTAINER_OF(dwork, struct pump_ctx, stop_work);
    pump_all_low(p);
    p->running = false;
    app_printk("[PUMP] stopped\r\n");
}

static int pump_init_one(struct pump_ctx *p)
{
    if (!p || !p->in1 || !p->in2) return -EINVAL;
    if (!device_is_ready(p->in1->port) || !device_is_ready(p->in2->port)) return -ENODEV;

    int r = 0;
    r |= gpio_pin_configure_dt(p->in1, GPIO_OUTPUT_INACTIVE);
    r |= gpio_pin_configure_dt(p->in2, GPIO_OUTPUT_INACTIVE);

    k_work_init_delayable(&p->stop_work, pump_stop_work_handler);
    return r;
}

/* --- Public API --- */
int pump_init(void)
{
    int r = pump_init_one(&pump);
    if (r) {
        app_printk("[PUMP] init error %d\r\n", r);
        return r;
    }
    app_printk("[PUMP] ready\r\n");
    return 0;
}

void pump_run(int dir, uint32_t duration_s)
{
    if (!pump.in1) { app_printk("[PUMP] Not configured\r\n"); return; }

    k_work_cancel_delayable(&pump.stop_work);
    pump_all_low(&pump);

    if (dir >= 0) {          /* extend */
        gpio_pin_set_dt(pump.in1, 1);
        gpio_pin_set_dt(pump.in2, 0);
    } else {                 /* retract */
        gpio_pin_set_dt(pump.in1, 0);
        gpio_pin_set_dt(pump.in2, 1);
    }
    pump.running = true;

    if (duration_s == 0) {
        pump_all_low(&pump);
        pump.running = false;
        app_printk("[PUMP] immediate stop\r\n");
        return;
    }

    int32_t delta = (int32_t)duration_s;
    if (dir < 0) {
        delta = -delta;
    }
    pump.position_sec += delta;
    k_work_schedule(&pump.stop_work, K_SECONDS(duration_s));
    app_printk("[PUMP] %s for %us\r\n",
           (dir >= 0 ? "EXTEND" : "RETRACT"),
           (unsigned)duration_s);
}


int32_t pump_get_position_sec(void)
{
    return pump.position_sec;
}

void pump_reset_position(void)
{
    pump.position_sec = 0;
}

