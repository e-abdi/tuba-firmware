
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <errno.h>

/* Resolve MS5837 device by compatible, independent of node label. */
static const struct device *const ms_dev = DEVICE_DT_GET_ONE(meas_ms5837_30ba);

/* Console UART for nonblocking keypress checks */
static const struct device *const uart_console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* Check if user pressed 'q' (or 'Q') to quit */
static bool quit_requested(void)
{
    if (!device_is_ready(uart_console)) {
        return false;
    }
    unsigned char c;
    int rc = uart_poll_in(uart_console, &c);
    if (rc == 0 && (c == 'q' || c == 'Q')) {
        return true;
    }
    return false;
}

/* Initialize sensor with gentle warm-up and readiness checks */
int ms5837_init(void)
{
    /* Allow sensor time to power-up and be ready */
    k_sleep(K_MSEC(100));

    if (!device_is_ready(ms_dev)) {
        /* Some boards need longer after power-up */
        k_sleep(K_MSEC(400));
        if (!device_is_ready(ms_dev)) {
            app_printk("[External Pressure] MS5837 device not ready\r\n");
            return -ENODEV;
        }
    }

    app_printk("[External Pressure] MS5837 detected on %s\r\n", ms_dev->name);
    return 0;
}

void ms5837_stream_interactive(void)
{
    if (ms5837_init() != 0) {
        return;
    }

    app_printk("[External Pressure] streaming — press 'q' to return\r\n");

    uint64_t next = k_uptime_get();
    for (;;) {
        int rc;

        /* Fetch a fresh sample, with a small retry window for first reads */
        rc = sensor_sample_fetch(ms_dev);
        if (rc != 0) {
            int attempts = 5;
            while (attempts-- > 0 && rc != 0) {
                k_sleep(K_MSEC(50));
                rc = sensor_sample_fetch(ms_dev);
            }
            if (rc != 0) {
                app_printk("[External Pressure] read failed (%d)\r\n", rc);
                goto wait_and_continue;
            }
        }

        struct sensor_value press_kpa = {0};
        struct sensor_value temp_c = {0};

        rc = sensor_channel_get(ms_dev, SENSOR_CHAN_PRESS, &press_kpa);
        if (rc != 0) {
            app_printk("[External Pressure] PRESS get failed (%d)\r\n", rc);
            goto wait_and_continue;
        }
        rc = sensor_channel_get(ms_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp_c);
        if (rc != 0) {
            app_printk("[External Pressure] TEMP get failed (%d)\r\n", rc);
            goto wait_and_continue;
        }

        /* Pretty-print as T=xx.xx C, P=xxx.xxx kPa */
        int T_whole = temp_c.val1;
        int T_frac2 = (int)((temp_c.val2 >= 0 ? temp_c.val2 : -temp_c.val2) / 10000) % 100;

        /* PRESS is in kPa for ms5837 driver */
        int P_whole = press_kpa.val1;
        int P_frac3 = (int)((press_kpa.val2 >= 0 ? press_kpa.val2 : -press_kpa.val2) / 1000) % 1000;

        app_printk("T=%d.%02d C, P=%d.%03d kPa\r\n", T_whole, T_frac2, P_whole, P_frac3);

wait_and_continue:
        next += 1000;
        while (k_uptime_get() < next) {
            if (quit_requested()) {
                app_printk("[External Pressure] exit requested → back to menu\r\n");
                return;
            }
            k_sleep(K_MSEC(20));
        }
    }
}

/* Non-interactive single-sample read of MS5837: returns temp (C) and pressure (kPa). */
int ms5837_read(double *temp_c, double *press_kpa)
{
    if (ms5837_init() != 0) return -ENODEV;

    int rc = sensor_sample_fetch(ms_dev);
    if (rc != 0) return rc;

    struct sensor_value press = {0};
    struct sensor_value temp = {0};
    rc = sensor_channel_get(ms_dev, SENSOR_CHAN_PRESS, &press);
    if (rc != 0) return rc;
    rc = sensor_channel_get(ms_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
    if (rc != 0) return rc;

    if (press_kpa) *press_kpa = (double)press.val1 + ((double)press.val2) / 1000000.0;
    if (temp_c) *temp_c = (double)temp.val1 + ((double)temp.val2) / 1000000.0;
    return 0;
}
