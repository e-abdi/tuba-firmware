
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>

#include "hw_motors.h"

/* Devicetree aliases expected:
 *   roll-in1, roll-in2
 *   pitch-in1, pitch-in2
 * No enable pins required; H-bridges are always enabled via external pull-ups.
 */

#define HAVE_ROLL_IN1  DT_NODE_HAS_STATUS(DT_ALIAS(roll_in_1), okay)
#define HAVE_ROLL_IN2  DT_NODE_HAS_STATUS(DT_ALIAS(roll_in_2), okay)
#define HAVE_PITCH_IN1 DT_NODE_HAS_STATUS(DT_ALIAS(pitch_in_1), okay)
#define HAVE_PITCH_IN2 DT_NODE_HAS_STATUS(DT_ALIAS(pitch_in_2), okay)

#if !HAVE_ROLL_IN1 || !HAVE_ROLL_IN2 || !HAVE_PITCH_IN1 || !HAVE_PITCH_IN2
#warning "One or more motor GPIO aliases are missing in the devicetree"
#endif

#if HAVE_ROLL_IN1
static const struct gpio_dt_spec ROLL_IN1 = GPIO_DT_SPEC_GET(DT_ALIAS(roll_in_1), gpios);
#else
static const struct gpio_dt_spec ROLL_IN1 = {0};
#endif
#if HAVE_ROLL_IN2
static const struct gpio_dt_spec ROLL_IN2 = GPIO_DT_SPEC_GET(DT_ALIAS(roll_in_2), gpios);
#else
static const struct gpio_dt_spec ROLL_IN2 = {0};
#endif

#if HAVE_PITCH_IN1
static const struct gpio_dt_spec PITCH_IN1 = GPIO_DT_SPEC_GET(DT_ALIAS(pitch_in_1), gpios);
#else
static const struct gpio_dt_spec PITCH_IN1 = {0};
#endif
#if HAVE_PITCH_IN2
static const struct gpio_dt_spec PITCH_IN2 = GPIO_DT_SPEC_GET(DT_ALIAS(pitch_in_2), gpios);
#else
static const struct gpio_dt_spec PITCH_IN2 = {0};
#endif

struct motor_gpio {
    struct gpio_dt_spec in1;
    struct gpio_dt_spec in2;
};

struct motor_state {
    struct motor_gpio io;
    struct k_work_delayable stop_work;
    atomic_t running;
    int32_t position_sec;
};

static struct motor_state g_motors[2];

static inline struct motor_state *get_motor(enum motor_id id)
{
    return (id == MOTOR_ROLL) ? &g_motors[0] : &g_motors[1];
}

static int motor_all_low(struct motor_state *m)
{
    int err = 0;
    if (m->io.in1.port) {
        int r = gpio_pin_set_dt(&m->io.in1, 0);
        if (r && !err) { err = r; }
    }
    if (m->io.in2.port) {
        int r = gpio_pin_set_dt(&m->io.in2, 0);
        if (r && !err) { err = r; }
    }
    return err;
}

static void motor_stop_work(struct k_work *work)
{
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct motor_state *m = CONTAINER_OF(dwork, struct motor_state, stop_work);
    (void)motor_all_low(m);
    atomic_clear(&m->running);
    app_printk("[MOTOR] timed stop\r\n");
}

static int motor_configure(struct motor_state *m, const struct motor_gpio *io)
{
    int err;

    m->io = *io;
    m->position_sec = 0;

    if (m->io.in1.port) {
        if (!gpio_is_ready_dt(&m->io.in1)) { return -ENODEV; }
        err = gpio_pin_configure_dt(&m->io.in1, GPIO_OUTPUT_INACTIVE);
        if (err) { return err; }
    }
    if (m->io.in2.port) {
        if (!gpio_is_ready_dt(&m->io.in2)) { return -ENODEV; }
        err = gpio_pin_configure_dt(&m->io.in2, GPIO_OUTPUT_INACTIVE);
        if (err) { return err; }
    }

    k_work_init_delayable(&m->stop_work, motor_stop_work);
    atomic_clear(&m->running);
    return 0;
}

void motor_cmd(enum motor_id id, int dir, uint32_t duration_s)
{
    struct motor_state *m = get_motor(id);

    k_work_cancel_delayable(&m->stop_work);

    if (dir == 0) {
        (void)motor_all_low(m);
        atomic_clear(&m->running);
        app_printk("[MOTOR] stop\r\n");
        return;
    }

    /* Guard: ensure GPIOs were configured */
    if (!m->io.in1.port || !m->io.in2.port) {
        app_printk("[MOTOR] GPIO not configured for %s\r\n",
               (id == MOTOR_ROLL ? "ROLL" : "PITCH"));
        return;
    }

    /* H-bridge (no EN pins):
     * IN1 IN2
     *  1   0  -> direction A
     *  0   1  -> direction B
     *  0   0  -> coast
     *  1   1  -> brake (avoid unless desired)
     */
    if (dir > 0) {
        (void)gpio_pin_set_dt(&m->io.in1, 1);
        (void)gpio_pin_set_dt(&m->io.in2, 0);
    } else {
        (void)gpio_pin_set_dt(&m->io.in1, 0);
        (void)gpio_pin_set_dt(&m->io.in2, 1);
    }
    atomic_set(&m->running, 1);

    if (duration_s == 0) {
        app_printk("[MOTOR] %s start (continuous)\r\n",
               (id == MOTOR_ROLL ? "ROLL" : "PITCH"));
        return;
    }

    int32_t delta = (int32_t)duration_s;
    if (dir < 0) {
        delta = -delta;
    }
    m->position_sec += delta;
    k_work_schedule(&m->stop_work, K_SECONDS(duration_s));
    app_printk("[MOTOR] %s run %s for %us\r\n",
           (id == MOTOR_ROLL ? "ROLL" : "PITCH"),
           (dir > 0 ? "FWD" : "REV"),
           (unsigned)duration_s);
}

/* Backward compatibility: old code calls motor_run(). */
void motor_run(enum motor_id id, int dir, uint32_t duration_s)
{
    motor_cmd(id, dir, duration_s);
}

bool motor_is_running(enum motor_id id)
{
    return atomic_get(&get_motor(id)->running);
}

int32_t motor_get_position_sec(enum motor_id id)
{
    struct motor_state *m = get_motor(id);
    return m->position_sec;
}

void motor_reset_position(enum motor_id id)
{
    struct motor_state *m = get_motor(id);
    m->position_sec = 0;
}

void motors_reset_all_positions(void)
{
    for (int i = 0; i < 2; i++) {
        g_motors[i].position_sec = 0;
    }
}
int motors_init(void)
{
    int err;

    const struct motor_gpio roll_io = {
        .in1 = ROLL_IN1,
        .in2 = ROLL_IN2,
    };
    const struct motor_gpio pitch_io = {
        .in1 = PITCH_IN1,
        .in2 = PITCH_IN2,
    };

    err = motor_configure(&g_motors[0], &roll_io);
    if (err) {
        app_printk("[MOTOR] roll configuration failed: %d\r\n", err);
        return err;
    }
    err = motor_configure(&g_motors[1], &pitch_io);
    if (err) {
        app_printk("[MOTOR] pitch configuration failed: %d\r\n", err);
        return err;
    }

    app_printk("[MOTOR] init OK (EN pins not used)\r\n");
    return 0;
}