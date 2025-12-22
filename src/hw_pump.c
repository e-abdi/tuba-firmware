#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "hw_pump.h"

/* --- Devicetree bindings for Pump --- */
#define HAVE_PUMP_IN1 DT_NODE_HAS_STATUS(DT_ALIAS(pump_in1), okay)
#define HAVE_PUMP_IN2 DT_NODE_HAS_STATUS(DT_ALIAS(pump_in2), okay)
#define PUMP_SUPPORTED (HAVE_PUMP_IN1 && HAVE_PUMP_IN2)

#if PUMP_SUPPORTED
static const struct gpio_dt_spec gpio_pump_in1 = GPIO_DT_SPEC_GET(DT_ALIAS(pump_in1), gpios);
static const struct gpio_dt_spec gpio_pump_in2 = GPIO_DT_SPEC_GET(DT_ALIAS(pump_in2), gpios);

struct pump_ctx {
    const struct gpio_dt_spec *in1;
    const struct gpio_dt_spec *in2;
    struct k_work_delayable stop_work;
    volatile bool running;
    int32_t position_sec;
};

static struct pump_ctx pump = {
    .in1 = &gpio_pump_in1,
    .in2 = &gpio_pump_in2,
    .running = false,
    .position_sec = 0,
};

/* --- Helpers (only defined when PUMP_SUPPORTED) --- */
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

/* --- Public API (PUMP_SUPPORTED) --- */
int pump_init(void)
{
    int r = pump_init_one(&pump);
    if (r == 0) {
        app_printk("[PUMP] initialized\r\n");
    } else {
        app_printk("[PUMP] init failed: %d\r\n", r);
    }
    return r;
}

void pump_run(int dir, uint32_t duration_s)
{
    if (pump.running) {
        return;
    }

    pump_all_low(&pump);
    app_printk("[PUMP] running %d for %u sec\r\n", dir, duration_s);

    if (dir == 0) {  /* PUMP_DIR_IN */
        gpio_pin_set_dt(&gpio_pump_in1, 1);
    } else if (dir == 1) {  /* PUMP_DIR_OUT */
        gpio_pin_set_dt(&gpio_pump_in2, 1);
    } else {
        return;
    }

    pump.running = true;
    pump.position_sec = duration_s;

    if (duration_s > 0) {
        k_work_schedule(&pump.stop_work, K_SECONDS(duration_s));
    }
}

int32_t pump_get_position_sec(void)
{
    return pump.position_sec;
}

void pump_reset_position(void)
{
    pump_all_low(&pump);
    pump.running = false;
    pump.position_sec = 0;
    k_work_cancel_delayable(&pump.stop_work);
}

#else

/* --- Stubs (PUMP not supported) --- */
struct pump_ctx {
    int unused;
};
static struct pump_ctx pump = {0};

int pump_init(void)
{
    app_printk("[PUMP] not supported on this platform\r\n");
    return 0;
}

void pump_run(int dir, uint32_t duration_s)
{
    (void)dir;
    (void)duration_s;
}

int32_t pump_get_position_sec(void)
{
    return 0;
}

void pump_reset_position(void)
{
}

#endif

