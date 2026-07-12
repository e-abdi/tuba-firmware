/*
 * hw_leak.c — Leak detector (analog probe on the ADC)
 *
 * The leak module pulls its signal line to 3.3 V when dry; water across
 * the probe pins drags the voltage down. A reading *below* the configured
 * threshold therefore means "leak". The threshold is a mission parameter
 * ("Leak threshold [mV]") and the continuous check runs in the
 * leak-monitor thread in main.c.
 */
#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include "net_console.h"
#include "hw_leak.h"

/* ADC0 node = ESP32 ADC unit 1 (GPIO32-39, safe with WiFi active)
 * Channel 6 = GPIO34 (input-only, no pull resistors needed —
 * the leak module provides its own pull-up to 3.3V). */
#define LEAK_ADC_CHANNEL  6
#define LEAK_ADC_VREF_MV  1100  /* ESP32 internal reference */
/* ADC_GAIN_1_4 → 12 dB attenuation → practical range 0–2550 mV */

static const struct device *adc_dev;
static bool initialized;
static int16_t sample_buf;

static const struct adc_channel_cfg ch_cfg = {
    .gain             = ADC_GAIN_1_4,
    .reference        = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME_DEFAULT,
    .channel_id       = LEAK_ADC_CHANNEL,
    .differential     = 0,
};

static struct adc_sequence sequence = {
    .channels    = BIT(LEAK_ADC_CHANNEL),
    .buffer      = &sample_buf,
    .buffer_size = sizeof(sample_buf),
    .resolution  = 12,
};

int leak_detector_init(void)
{
    adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc0));
    if (!device_is_ready(adc_dev)) {
        app_printk("[LEAK] ADC not ready\r\n");
        return -ENODEV;
    }
    int rc = adc_channel_setup(adc_dev, &ch_cfg);
    if (rc != 0) {
        app_printk("[LEAK] channel setup failed: %d\r\n", rc);
        return rc;
    }
    initialized = true;
    app_printk("[LEAK] detector ready on GPIO34 (ADC1 CH6)\r\n");
    return 0;
}

int leak_detector_read_mv(int32_t *voltage_mv)
{
    if (!initialized) {
        return -ENODEV;
    }
    int rc = adc_read(adc_dev, &sequence);
    if (rc != 0) {
        return rc;
    }
    int32_t val = sample_buf;
    rc = adc_raw_to_millivolts(LEAK_ADC_VREF_MV, ADC_GAIN_1_4, 12, &val);
    if (rc != 0) {
        return rc;
    }
    *voltage_mv = val;
    return 0;
}

void leak_detector_stream_interactive(uint32_t threshold_mv)
{
    if (!initialized && leak_detector_init() != 0) {
        app_printk("[LEAK] init failed\r\n");
        return;
    }
    app_printk("[LEAK] Streaming — press 'q' then ENTER to quit\r\n");
    if (threshold_mv == 0) {
        app_printk("[LEAK] Threshold: 0 (monitoring OFF — raw readings only)\r\n");
    } else {
        app_printk("[LEAK] Threshold: %u mV  (below = LEAK)\r\n", threshold_mv);
    }

    int64_t next = k_uptime_get();
    while (1) {
        int32_t mv;
        if (leak_detector_read_mv(&mv) == 0) {
            const char *status = (threshold_mv == 0) ? "(off)"
                               : ((uint32_t)mv < threshold_mv) ? "*** LEAK ***" : "dry";
            app_printk("[LEAK] %d mV — %s\r\n", mv, status);
        } else {
            app_printk("[LEAK] read error\r\n");
        }
        next += 1000;
        while (k_uptime_get() < next) {
            if (console_quit_requested()) {
                app_printk("[LEAK] exit\r\n");
                return;
            }
            k_sleep(K_MSEC(50));
        }
    }
}
