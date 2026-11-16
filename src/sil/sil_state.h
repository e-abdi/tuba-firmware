/*
 * sil_state.h — Latest simulated sensor values, shared by the emulators
 *
 * sil_link.c writes a copy of every sil_sens_pkt it receives; the I2C/ADC
 * emulators read it when the firmware's drivers talk to them. When no
 * packet has arrived for SIL_LINK_TIMEOUT_MS the "sensors" vanish from the
 * bus (every transfer fails with -EIO), which is what an unplugged sensor
 * looks like to the drivers — the firmware's retry/abort paths see the
 * same thing they would see in the field.
 */
#ifndef SIL_STATE_H
#define SIL_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "sil_protocol.h"

#define SIL_LINK_TIMEOUT_MS 2000

struct sil_sensors {
    struct sil_sens_pkt pkt;
    int64_t rx_uptime_ms;   /* k_uptime when pkt arrived */
};

/* Store a freshly received packet (called by the link thread). */
void sil_state_update(const struct sil_sens_pkt *pkt);

/* Copy the latest values. Returns false (and leaves *out untouched) when
 * the link is down, i.e. no packet within SIL_LINK_TIMEOUT_MS. */
bool sil_state_get(struct sil_sensors *out);

/* True when the bridge is talking to us. */
bool sil_link_up(void);

#endif /* SIL_STATE_H */
