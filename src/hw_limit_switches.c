#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/devicetree.h>

#include "hw_limit_switches.h"
#include "hw_motors.h"
#include "app_print.h"
#include "net_console.h"

/* ESP32 HAL for direct register access to GPIO32/33 */
#include <soc/gpio_reg.h>
#include <hal/gpio_hal.h>

/* GPIO specs for limit switches - direct GPIO definitions
   GPIO32 = Pitch Limit UP (input-only pin, safe from boot strapping)
   GPIO33 = Pitch Limit DOWN (input-only pin, safe from boot strapping)
*/
static struct device *gpio_dev = NULL;

struct limit_switch_state {
    uint32_t pin;
    struct gpio_callback callback;
    volatile bool triggered;
    volatile int64_t last_trigger_time;
};

static struct limit_switch_state g_limit_switches[2] = {
    {.pin = 32, .last_trigger_time = 0},   /* LIMIT_PITCH_UP */
    {.pin = 33, .last_trigger_time = 0}    /* LIMIT_PITCH_DOWN */
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
    /* Check if either limit switch is currently pressed */
    bool up_pressed = limit_switch_is_pressed(LIMIT_PITCH_UP);
    bool down_pressed = limit_switch_is_pressed(LIMIT_PITCH_DOWN);
    
    if (up_pressed) {
        app_printk("[LIMIT] Pitch LIMIT UP (GPIO32) triggered, stopping pitch motor\r\n");
        motor_run(MOTOR_PITCH, 0, 0);  /* Stop motor */
    }
    
    if (down_pressed) {
        app_printk("[LIMIT] Pitch LIMIT DOWN (GPIO33) triggered, stopping pitch motor\r\n");
        motor_run(MOTOR_PITCH, 0, 0);  /* Stop motor */
    }
}

int limit_switches_init(void)
{
    int err = 0;

    /* Get GPIO0 device using device tree */
    gpio_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpio0));
    if (!gpio_dev) {
        app_printk("[LIMIT] GPIO_0 device not found\r\n");
        return -ENODEV;
    }

    if (!device_is_ready(gpio_dev)) {
        app_printk("[LIMIT] GPIO_0 device not ready\r\n");
        return -ENODEV;
    }

    /* Initialize pitch limit up (GPIO32) */
    err = gpio_pin_configure(gpio_dev, 32, GPIO_INPUT | GPIO_PULL_UP);
    if (err) {
        app_printk("[LIMIT] Failed to configure GPIO32: %d\r\n", err);
        return err;
    }

    gpio_init_callback(&g_limit_switches[LIMIT_PITCH_UP].callback, limit_switch_isr, 1ULL << 32);
    err = gpio_add_callback(gpio_dev, &g_limit_switches[LIMIT_PITCH_UP].callback);
    if (err) {
        app_printk("[LIMIT] Failed to add GPIO32 callback: %d\r\n", err);
        return err;
    }

    err = gpio_pin_interrupt_configure(gpio_dev, 32, GPIO_INT_EDGE_TO_ACTIVE);
    if (err) {
        app_printk("[LIMIT] Failed to configure GPIO32 interrupt: %d\r\n", err);
        return err;
    }

    app_printk("[LIMIT] Pitch limit UP (GPIO32) initialized\r\n");

    /* Initialize pitch limit down (GPIO33) */
    err = gpio_pin_configure(gpio_dev, 33, GPIO_INPUT | GPIO_PULL_UP);
    if (err) {
        app_printk("[LIMIT] Failed to configure GPIO33: %d\r\n", err);
        return err;
    }

    gpio_init_callback(&g_limit_switches[LIMIT_PITCH_DOWN].callback, limit_switch_isr, 1ULL << 33);
    err = gpio_add_callback(gpio_dev, &g_limit_switches[LIMIT_PITCH_DOWN].callback);
    if (err) {
        app_printk("[LIMIT] Failed to add GPIO33 callback: %d\r\n", err);
        return err;
    }

    err = gpio_pin_interrupt_configure(gpio_dev, 33, GPIO_INT_EDGE_TO_ACTIVE);
    if (err) {
        app_printk("[LIMIT] Failed to configure GPIO33 interrupt: %d\r\n", err);
        return err;
    }

    app_printk("[LIMIT] Pitch limit DOWN (GPIO33) initialized\r\n");

    return 0;
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
