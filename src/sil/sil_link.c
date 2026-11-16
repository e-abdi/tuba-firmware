/*
 * sil_link.c — UDP link between the virtual hardware and the Gazebo bridge
 *
 * Runs only on native_sim (CONFIG_TUBA_SIL). A 20 Hz thread:
 *
 *   1. samples the emulated H-bridge GPIO outputs (the very pins the real
 *      motor drivers toggle), integrates signed run time per actuator and
 *      sends a sil_act_pkt to the bridge;
 *   2. drains incoming sil_sens_pkt datagrams into sil_state (consumed by
 *      the I2C sensor emulators), and mirrors the limit switches and the
 *      leak ADC into the GPIO/ADC emulators.
 *
 * Sockets are the native-simulator offloaded sockets
 * (CONFIG_NET_NATIVE_OFFLOADED_SOCKETS): plain host UDP, no TAP setup.
 * The bridge's address is learned from the last packet received, so the
 * same code works when the bridge lives on another machine (HIL over the
 * glider's WiFi); 127.0.0.1:SIL_BRIDGE_PORT is used until then.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

#include "sil_protocol.h"
#include "sil_state.h"
#include "hw_motors.h"
#include "hw_pump.h"
#include "app_print.h"

#define SIL_PERIOD_MS 50

/* The motor pins, exactly as hw_motors.c / hw_pump.c see them. */
static const struct gpio_dt_spec pitch_in1 = GPIO_DT_SPEC_GET(DT_ALIAS(pitch_in_1), gpios);
static const struct gpio_dt_spec pitch_in2 = GPIO_DT_SPEC_GET(DT_ALIAS(pitch_in_2), gpios);
static const struct gpio_dt_spec roll_in1  = GPIO_DT_SPEC_GET(DT_ALIAS(roll_in_1), gpios);
static const struct gpio_dt_spec roll_in2  = GPIO_DT_SPEC_GET(DT_ALIAS(roll_in_2), gpios);
static const struct gpio_dt_spec pump_in1  = GPIO_DT_SPEC_GET(DT_ALIAS(pump_in_1), gpios);
static const struct gpio_dt_spec pump_in2  = GPIO_DT_SPEC_GET(DT_ALIAS(pump_in_2), gpios);

/* Limit switches live on gpio1 pins 0/1 (see hw_limit_switches.c), active low. */
static const struct device *const limit_port = DEVICE_DT_GET(DT_NODELABEL(gpio1));
#define LIMIT_UP_PIN   0
#define LIMIT_DOWN_PIN 1

/* Leak probe: adc0 channel 6 (hw_leak.c). */
static const struct device *const adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc0));
#define LEAK_ADC_CHANNEL 6

/* gpio_emul rejects input_set() on a pin not (yet) configured as input and
 * logs an error each time; hw_limit_switches.c configures the pins a little
 * after boot, so check first. */
static void limit_set(gpio_pin_t pin, int value)
{
    gpio_flags_t flags = 0;
    if (gpio_emul_flags_get(limit_port, pin, &flags) == 0 && (flags & GPIO_INPUT)) {
        (void)gpio_emul_input_set(limit_port, pin, value);
    }
}

static int pin_out(const struct gpio_dt_spec *s)
{
    int v = gpio_emul_output_get_dt(s);
    return v > 0 ? 1 : 0;   /* not configured yet -> treated as low */
}

/* IN1=1,IN2=0 -> +1 (direction A: FWD / PORT / OUT); IN1=0,IN2=1 -> -1. */
static int8_t bridge_dir(const struct gpio_dt_spec *in1, const struct gpio_dt_spec *in2)
{
    int a = pin_out(in1), b = pin_out(in2);
    if (a && !b) return +1;
    if (!a && b) return -1;
    return 0;
}

static void sil_link_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    int sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        app_printk("[SIL] socket() failed: %d\r\n", errno);
        return;
    }
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(SIL_FW_PORT) };
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (zsock_bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        app_printk("[SIL] bind(%d) failed: %d\r\n", SIL_FW_PORT, errno);
        zsock_close(sock);
        return;
    }

    struct sockaddr_in peer = { .sin_family = AF_INET, .sin_port = htons(SIL_BRIDGE_PORT) };
    zsock_inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr);

    app_printk("[SIL] virtual hardware: listening UDP:%d, bridge %s:%d\r\n",
               SIL_FW_PORT, "127.0.0.1", SIL_BRIDGE_PORT);

    struct sil_act_pkt act = {
        .magic = SIL_MAGIC_ACT,
        .boot_id = sys_rand32_get(),
    };
    int64_t last = k_uptime_get();
    bool was_up = false;

    while (1) {
        k_msleep(SIL_PERIOD_MS);
        int64_t now = k_uptime_get();
        float dt = (float)(now - last) / 1000.0f;
        last = now;

        /* --- 1. actuators: sample the H-bridge pins, integrate run time --- */
        act.pitch_dir = bridge_dir(&pitch_in1, &pitch_in2);
        act.roll_dir  = bridge_dir(&roll_in1,  &roll_in2);
        act.pump_dir  = bridge_dir(&pump_in1,  &pump_in2);
        act.pitch_run_s += act.pitch_dir * dt;
        act.roll_run_s  += act.roll_dir  * dt;
        act.pump_run_s  += act.pump_dir  * dt;
        act.pitch_pos_s = motor_get_position_sec(MOTOR_PITCH);
        act.roll_pos_s  = motor_get_position_sec(MOTOR_ROLL);
        act.pump_pos_s  = pump_get_position_sec();
        act.uptime_ms = (uint32_t)now;
        act.seq++;
        act.crc = sil_fnv1a(&act, sizeof(act) - sizeof(act.crc));
        (void)zsock_sendto(sock, &act, sizeof(act), 0,
                           (struct sockaddr *)&peer, sizeof(peer));

        /* --- 2. sensors: drain everything that arrived --- */
        for (;;) {
            struct sil_sens_pkt pkt;
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int n = zsock_recvfrom(sock, &pkt, sizeof(pkt), ZSOCK_MSG_DONTWAIT,
                                   (struct sockaddr *)&from, &fromlen);
            if (n <= 0) {
                break;
            }
            if (n != (int)sizeof(pkt) || pkt.magic != SIL_MAGIC_SENS ||
                pkt.crc != sil_fnv1a(&pkt, sizeof(pkt) - sizeof(pkt.crc))) {
                continue;
            }
            sil_state_update(&pkt);
            peer = from;   /* reply to whoever is driving us */
        }

        /* --- 3. mirror the digital/analog inputs into the emulators --- */
        struct sil_sensors s;
        bool up = sil_state_get(&s);
        if (up != was_up) {
            app_printk("\r\n[SIL] bridge link %s\r\n", up ? "UP" : "LOST");
            was_up = up;
        }
        if (up) {
            /* active low: 0 = pressed */
            limit_set(LIMIT_UP_PIN,   (s.pkt.flags & SIL_F_LIMIT_UP)   ? 0 : 1);
            limit_set(LIMIT_DOWN_PIN, (s.pkt.flags & SIL_F_LIMIT_DOWN) ? 0 : 1);
            adc_emul_const_value_set(adc_dev, LEAK_ADC_CHANNEL, (int)s.pkt.leak_mv);
        } else {
            limit_set(LIMIT_UP_PIN, 1);
            limit_set(LIMIT_DOWN_PIN, 1);
            adc_emul_const_value_set(adc_dev, LEAK_ADC_CHANNEL, 3300);
        }
    }
}

K_THREAD_DEFINE(sil_link, 4096, sil_link_thread, NULL, NULL, NULL, 6, 0, 500);
