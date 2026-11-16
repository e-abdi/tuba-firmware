#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <string.h>

#include "sil_state.h"

static struct sil_sensors g_sens;
static bool g_have_pkt;
static struct k_spinlock g_lock;

void sil_state_update(const struct sil_sens_pkt *pkt)
{
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    g_sens.pkt = *pkt;
    g_sens.rx_uptime_ms = k_uptime_get();
    g_have_pkt = true;
    k_spin_unlock(&g_lock, key);
}

bool sil_state_get(struct sil_sensors *out)
{
    bool ok;
    k_spinlock_key_t key = k_spin_lock(&g_lock);
    ok = g_have_pkt && (k_uptime_get() - g_sens.rx_uptime_ms) < SIL_LINK_TIMEOUT_MS;
    if (ok && out) {
        *out = g_sens;
    }
    k_spin_unlock(&g_lock, key);
    return ok;
}

bool sil_link_up(void)
{
    return sil_state_get(NULL);
}
