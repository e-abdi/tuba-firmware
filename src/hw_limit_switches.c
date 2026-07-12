/*
 * hw_limit_switches.c — Pitch-mass travel limit switches (safety)
 *
 * Two active-low switches stop the pitch mass at the ends of its rail:
 *   GPIO32 = pitch limit UP, GPIO33 = pitch limit DOWN (input-only pins,
 *   safe from ESP32 boot strapping).
 *
 * Detection is belt-and-braces: a GPIO interrupt records triggers, and the
 * main loop calls limit_switches_check_and_stop() every event cycle, which
 * reads the pin levels directly (via the ESP32 GPIO_IN1 register — the
 * safest read path from any context) and, on a hit, stops the pitch motor
 * and backs it off for one second to release the switch.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/devicetree.h>

#include "hw_limit_switches.h"
#include "hw_motors.h"
#include "app_print.h"
#include "net_console.h"

/* ESP32 HAL for direct register access to GPIO32/33 */
#ifdef CONFIG_SOC_ESP32
#include <soc/gpio_reg.h>
#include <hal/gpio_hal.h>
#endif

/* GPIO32/33 belong to the ESP32's *second* GPIO port (gpio1 = pins 32-39),
 * addressed there as pins 0 and 1. Using gpio0 with pin numbers 32/33
 * silently truncates the pin mask to zero — a real bug in earlier firmware
 * (the polling path via direct register reads is what made it work).
 */
#define LIMIT_UP_PIN    0   /* GPIO32 on port gpio1 */
#define LIMIT_DOWN_PIN  1   /* GPIO33 on port gpio1 */
static const struct device *gpio_dev = NULL;

struct limit_switch_state {
    uint32_t pin;
    struct gpio_callback callback;
    volatile bool triggered;
    volatile int64_t last_trigger_time;
    volatile bool ignore_switches;  /* Flag to disable switch checking during reverse */
};

static struct limit_switch_state g_limit_switches[2] = {
    {.pin = 32, .last_trigger_time = 0, .ignore_switches = false},   /* LIMIT_PITCH_UP */
    {.pin = 33, .last_trigger_time = 0, .ignore_switches = false}    /* LIMIT_PITCH_DOWN */
};

static void limit_switch_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    struct limit_switch_state *sw = CONTAINER_OF(cb, struct limit_switch_state, callback);
    
    /* Record when the interrupt fired */
    sw->last_trigger_time = k_uptime_get();
    sw->triggered = true;
}

bool limit_switch_is_pressed(int switch_id)
{
    if (switch_id < 0 || switch_id >= 2) return false;
    
#ifdef CONFIG_SOC_ESP32
    /* Read GPIO32/33 directly from ESP32 registers
       For GPIO32-39: use GPIO_IN1_REG (bits 0-7 map to GPIO32-39)
       Active low: 0 = pressed, 1 = open
    */
    uint32_t pin = (switch_id == LIMIT_PITCH_UP) ? 32 : 33;
    
    /* GPIO_IN1_REG contains GPIO32-39 status in bits 0-7 */
    uint32_t gpio_in1 = REG_READ(GPIO_IN1_REG);
    
    /* Extract bit for this pin (pin 32 = bit 0, pin 33 = bit 1, etc) */
    uint32_t bit_position = pin - 32;
    uint32_t pin_bit = (gpio_in1 >> bit_position) & 0x01;
    
    /* pin_bit = 0 means pressed (active low), pin_bit = 1 means open */
    return (pin_bit == 0);
#else
    /* For non-ESP32 platforms, use Zephyr GPIO API */
    if (gpio_dev == NULL) return false;

    int pin_num = (switch_id == LIMIT_PITCH_UP) ? LIMIT_UP_PIN : LIMIT_DOWN_PIN;
    int val = gpio_pin_get(gpio_dev, pin_num);
    return (val == 0);  /* Active low */
#endif
}

void limit_switch_callback(int switch_id)
{
    if (switch_id < 0 || switch_id >= 2) return;

    app_printk("[LIMIT] Manual callback for switch %d\r\n", switch_id);
    motor_run(MOTOR_PITCH, 0, 0);
}

/* Called from main loop to handle triggered limit switches safely */
void limit_switches_check_and_stop(void)
{
    /* Skip if we're in the middle of a reverse procedure */
    if (g_limit_switches[0].ignore_switches) {
        return;
    }
    
    /* Check if either limit switch is currently pressed */
    bool up_pressed = limit_switch_is_pressed(LIMIT_PITCH_UP);
    bool down_pressed = limit_switch_is_pressed(LIMIT_PITCH_DOWN);
    
    if (up_pressed) {
        app_printk("[LIMIT] Pitch LIMIT UP (GPIO32) triggered, stopping then reversing\r\n");
        motor_run(MOTOR_PITCH, 0, 0);  /* Stop motor immediately */
        k_sleep(K_MSEC(100));           /* Brief pause */
        
        /* Ignore switches during reverse to prevent immediate re-trigger */
        g_limit_switches[0].ignore_switches = true;
        motor_run(MOTOR_PITCH, -1, 1);  /* Reverse (AFT) for 1 second to release switch */
        k_sleep(K_SECONDS(1));
        g_limit_switches[0].ignore_switches = false;
    }
    
    if (down_pressed) {
        app_printk("[LIMIT] Pitch LIMIT DOWN (GPIO33) triggered, stopping then reversing\r\n");
        motor_run(MOTOR_PITCH, 0, 0);  /* Stop motor immediately */
        k_sleep(K_MSEC(100));           /* Brief pause */
        
        /* Ignore switches during reverse to prevent immediate re-trigger */
        g_limit_switches[0].ignore_switches = true;
        motor_run(MOTOR_PITCH, +1, 1);  /* Reverse (FWD) for 1 second to release switch */
        k_sleep(K_SECONDS(1));
        g_limit_switches[0].ignore_switches = false;
    }
}

/* Configure one switch pin: input with pull-up, interrupt on press
 * (active edge), and register its ISR callback. */
static int limit_switch_setup_one(int switch_id, int pin, const char *name)
{
    int err = gpio_pin_configure(gpio_dev, pin, GPIO_INPUT | GPIO_PULL_UP);
    if (err) {
        app_printk("[LIMIT] Failed to configure %s: %d\r\n", name, err);
        return err;
    }

    gpio_init_callback(&g_limit_switches[switch_id].callback, limit_switch_isr, BIT(pin));
    err = gpio_add_callback(gpio_dev, &g_limit_switches[switch_id].callback);
    if (err) {
        app_printk("[LIMIT] Failed to add %s callback: %d\r\n", name, err);
        return err;
    }

    err = gpio_pin_interrupt_configure(gpio_dev, pin, GPIO_INT_EDGE_TO_ACTIVE);
    if (err) {
        app_printk("[LIMIT] Failed to configure %s interrupt: %d\r\n", name, err);
        return err;
    }

    app_printk("[LIMIT] Pitch limit %s initialized\r\n", name);
    return 0;
}

int limit_switches_init(void)
{
    int err;

    /* GPIO32/33 are on the second GPIO port (gpio1, pins 0/1). */
    gpio_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpio1));
    if (!gpio_dev || !device_is_ready(gpio_dev)) {
        app_printk("[LIMIT] gpio1 device not ready\r\n");
        return -ENODEV;
    }

    err = limit_switch_setup_one(LIMIT_PITCH_UP, LIMIT_UP_PIN, "UP (GPIO32)");
    if (err) {
        return err;
    }
    return limit_switch_setup_one(LIMIT_PITCH_DOWN, LIMIT_DOWN_PIN, "DOWN (GPIO33)");
}

/**
 * Interactive test loop for limit switches.
 * Uses interrupt-based detection to show real-time button press status.
 * Type 'q' + ENTER to exit.
 */
void limit_switches_test_interactive(void) {
    app_printk("\r\n=== Limit Switch Test ===\r\n");
    app_printk("GPIO32 (UP):   Press to trigger\r\n");
    app_printk("GPIO33 (DOWN): Press to trigger\r\n");
    app_printk("Type 'q' + ENTER to exit.\r\n\r\n");
    
    if (!gpio_dev) {
        app_printk("[ERROR] GPIO not initialized\r\n");
        return;
    }
    
    /* Clear any pending triggered flags */
    g_limit_switches[LIMIT_PITCH_UP].triggered = false;
    g_limit_switches[LIMIT_PITCH_DOWN].triggered = false;
    
    bool should_exit = false;
    
    while (!should_exit) {
        /* Non-blocking check for 'q' + Enter from network console */
        char line[128];
        if (net_console_poll_line(line, sizeof(line), K_MSEC(50))) {
            if ((line[0] == 'q' || line[0] == 'Q') && line[1] == '\0') {
                app_printk("\r\n[TEST] Exiting limit switch test\r\n\r\n");
                should_exit = true;
                break;
            }
        }
        
        /* Read current button states using ISR info */
        bool up_pressed = limit_switch_is_pressed(LIMIT_PITCH_UP);
        bool down_pressed = limit_switch_is_pressed(LIMIT_PITCH_DOWN);
        
        /* Display current state */
        app_printk("\rGPIO32 (UP):   %-9s  |  GPIO33 (DOWN): %-9s",
                   up_pressed ? "[PRESSED]" : "[OPEN]",
                   down_pressed ? "[PRESSED]" : "[OPEN]");
        
        k_sleep(K_MSEC(50));
    }
}
