# WiFi Credentials Setup Guide

This guide shows how to configure WiFi credentials for your Pico W device.

## Option A: Hardcoded Credentials (Testing Only)

**Not recommended for production**, but simplest for initial testing.

### Step 1: Create credentials file (Optional)
Create `src/wifi_credentials.c`:
```c
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

const char *wifi_ssid = "YourSSID";
const char *wifi_psk = "YourPassword";
const uint8_t wifi_security = WIFI_SECURITY_TYPE_PSK;
```

### Step 2: Modify main.c to use credentials
Add to `main()` after WiFi stack initializes:
```c
#ifdef CONFIG_WIFI_CYWIP
    /* Connect to WiFi after 2 seconds to let stack initialize */
    k_sleep(K_SECONDS(2));
    
    struct wifi_connect_req_params conn_params = {
        .ssid = (uint8_t *)wifi_ssid,
        .ssid_length = strlen(wifi_ssid),
        .psk = (uint8_t *)wifi_psk,
        .psk_length = strlen(wifi_psk),
        .security = wifi_security,
    };
    
    struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_CTX(WIFI));
    if (iface) {
        int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &conn_params, sizeof(conn_params));
        app_printk("WiFi connect request: %d\r\n", ret);
    }
#endif
```

### Step 3: Build and test
```bash
west build -b rpi_pico
west flash
# Monitor USB console - should see "WiFi connect request: 0"
```

---

## Option B: Zephyr Kconfig (Development)

Configure credentials via build-time symbols.

### Step 1: Edit prj.conf
```ini
# WiFi credentials (development only)
CONFIG_WIFI_CYWIP_SSID="MyNetwork"
CONFIG_WIFI_CYWIP_PSK="MyPassword123"
```

### Step 2: Update main.c to read from Kconfig
```c
#ifdef CONFIG_WIFI_CYWIP
    struct wifi_connect_req_params conn_params = {
        .ssid = (uint8_t *)CONFIG_WIFI_CYWIP_SSID,
        .ssid_length = strlen(CONFIG_WIFI_CYWIP_SSID),
        .psk = (uint8_t *)CONFIG_WIFI_CYWIP_PSK,
        .psk_length = strlen(CONFIG_WIFI_CYWIP_PSK),
        .security = WIFI_SECURITY_TYPE_PSK,
    };
    // ... connect logic ...
#endif
```

### Step 3: Build
```bash
west build -b rpi_pico
west flash
```

**Pros**: No code changes, easy to switch networks  
**Cons**: Credentials visible in built config

---

## Option C: Interactive Menu (Recommended)

Add WiFi credentials configuration via the existing menu system.

### Step 1: Create credentials management file
Create `src/wifi_config.c`:
```c
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <ctype.h>

#include "app_params.h"
#include "app_print.h"

/* WiFi credentials stored in app_params struct */
typedef struct {
    char ssid[32];
    char psk[64];
    uint8_t security;  /* WIFI_SECURITY_TYPE_PSK, etc. */
} wifi_creds_t;

static wifi_creds_t wifi_creds = {0};

int wifi_config_get_ssid(char *buf, size_t len)
{
    if (!buf || len < 1) return -1;
    strncpy(buf, wifi_creds.ssid, len - 1);
    buf[len - 1] = '\0';
    return strlen(buf);
}

int wifi_config_set_ssid(const char *ssid)
{
    if (!ssid) return -1;
    strncpy(wifi_creds.ssid, ssid, sizeof(wifi_creds.ssid) - 1);
    wifi_creds.ssid[sizeof(wifi_creds.ssid) - 1] = '\0';
    return 0;
}

int wifi_config_get_psk(char *buf, size_t len)
{
    if (!buf || len < 1) return -1;
    strncpy(buf, wifi_creds.psk, len - 1);
    buf[len - 1] = '\0';
    return strlen(buf);
}

int wifi_config_set_psk(const char *psk)
{
    if (!psk) return -1;
    strncpy(wifi_creds.psk, psk, sizeof(wifi_creds.psk) - 1);
    wifi_creds.psk[sizeof(wifi_creds.psk) - 1] = '\0';
    return 0;
}

int wifi_config_connect(void)
{
    #ifdef CONFIG_WIFI_CYWIP
    #include <zephyr/net/net_if.h>
    #include <zephyr/net/wifi_mgmt.h>
    
    struct wifi_connect_req_params params = {
        .ssid = (uint8_t *)wifi_creds.ssid,
        .ssid_length = strlen(wifi_creds.ssid),
        .psk = (uint8_t *)wifi_creds.psk,
        .psk_length = strlen(wifi_creds.psk),
        .security = WIFI_SECURITY_TYPE_PSK,
    };
    
    struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_CTX(WIFI));
    return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
    #else
    return -1;
    #endif
}
```

### Step 2: Add to UI menu (ui_menu.c)
```c
/* Add new state for WiFi credentials */
typedef enum {
    ST_WIFI_MENU,
    ST_WIFI_SSID_INPUT,
    ST_WIFI_PSK_INPUT,
    /* ... existing states ... */
} state_id_t;

static void on_entry_WIFI_MENU(void)
{
    char ssid[32];
    char psk[64];
    wifi_config_get_ssid(ssid, sizeof(ssid));
    wifi_config_get_psk(psk, sizeof(psk));
    
    app_printk("=== WiFi Configuration ===\r\n");
    app_printk("1. SSID:     [%s]\r\n", strlen(ssid) ? ssid : "(not set)");
    app_printk("2. Password: [%s]\r\n", strlen(psk) ? psk : "(not set)");
    app_printk("3. Connect\r\n");
    app_printk("4. Disconnect\r\n");
    app_printk("b. Back\r\n");
}

state_id_t on_event_WIFI_MENU(const event_t *evt)
{
    if (evt->id != EVT_ENTER) return ST_WIFI_MENU;
    
    /* Parse menu selection and transition */
    return ST_WIFI_MENU;  /* Stay in menu or transition */
}
```

### Step 3: Integrate into main menu
```c
/* In on_entry_MENU() */
app_printk("5. WiFi Configuration (Pico W)\r\n");

/* In on_event_MENU() */
case '5':
    if (/* check if Pico W */) {
        return ST_WIFI_MENU;
    }
    break;
```

### Step 4: Build and test
```bash
west build -b rpi_pico
west flash

# On USB console:
# > menu
# 5. WiFi Configuration (Pico W)
# > 5
# [WiFi menu appears]
```

---

## Option D: Environment Variables (Advanced)

Store credentials in environment and pass to build.

### Step 1: Set environment
```bash
export WIFI_SSID="MyNetwork"
export WIFI_PSK="MyPassword123"
```

### Step 2: Create CMakeLists.txt helper
```cmake
set(WIFI_SSID "$ENV{WIFI_SSID}")
set(WIFI_PSK "$ENV{WIFI_PSK}")

# Generate header with credentials
configure_file(
    ${PROJECT_SOURCE_DIR}/wifi_creds.h.in
    ${PROJECT_BINARY_DIR}/include/wifi_creds.h
)
```

### Step 3: Create template `src/wifi_creds.h.in`
```c
#ifndef WIFI_CREDS_H
#define WIFI_CREDS_H

#define WIFI_SSID "@WIFI_SSID@"
#define WIFI_PSK "@WIFI_PSK@"

#endif
```

### Step 4: Use in code
```c
#include "wifi_creds.h"

struct wifi_connect_req_params params = {
    .ssid = (uint8_t *)WIFI_SSID,
    .ssid_length = strlen(WIFI_SSID),
    .psk = (uint8_t *)WIFI_PSK,
    .psk_length = strlen(WIFI_PSK),
    .security = WIFI_SECURITY_TYPE_PSK,
};
```

**Pros**: Secure (not in repo), flexible  
**Cons**: Requires environment setup

---

## Option E: Persistent Storage (Production Ready)

Store credentials in flash alongside other parameters.

### Step 1: Extend app_params struct
Edit `include/app_params.h`:
```c
struct app_params {
    /* Existing fields... */
    
    /* WiFi credentials (new) */
    char wifi_ssid[32];
    char wifi_psk[64];
    uint8_t wifi_enabled;
};
```

### Step 2: Update app_params.c
```c
void app_params_init(void)
{
    /* Existing init code... */
    
    #ifdef CONFIG_WIFI_CYWIP
    if (strlen(g_params.wifi_ssid) > 0) {
        app_printk("WiFi: Connecting to [%s]...\r\n", g_params.wifi_ssid);
        wifi_connect_with_saved_creds();
    }
    #endif
}
```

### Step 3: Add WiFi connection function
```c
#ifdef CONFIG_WIFI_CYWIP
static void wifi_connect_with_saved_creds(void)
{
    struct wifi_connect_req_params params = {
        .ssid = (uint8_t *)g_params.wifi_ssid,
        .ssid_length = strlen(g_params.wifi_ssid),
        .psk = (uint8_t *)g_params.wifi_psk,
        .psk_length = strlen(g_params.wifi_psk),
        .security = WIFI_SECURITY_TYPE_PSK,
    };
    
    struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_CTX(WIFI));
    net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}
#endif
```

### Step 4: Add menu options to save credentials
```c
/* In ui_menu.c */
int set_wifi_ssid(const char *ssid)
{
    strncpy(g_params.wifi_ssid, ssid, sizeof(g_params.wifi_ssid) - 1);
    g_params.wifi_ssid[sizeof(g_params.wifi_ssid) - 1] = '\0';
    return app_params_save();  /* Persist to flash */
}
```

**Pros**: Survives power cycles, field updates, no hardcoding  
**Cons**: Slightly more complex, requires menu integration

---

## Recommended Approach

For Tuba AUV deployment:

**Phase 1 (Testing)**: Option A - Hardcoded credentials, quick iteration  
**Phase 2 (Development)**: Option C - Interactive menu, full control  
**Phase 3 (Production)**: Option E - Persistent storage in flash

---

## Testing Connection

### Verify WiFi credentials work
```bash
# From computer on same network
nc <pico-ip> 9000

# Type a command
help

# If you see menu output, WiFi is working!
```

### Debug WiFi issues
Add to `prj.conf`:
```ini
CONFIG_NET_LOG=y
CONFIG_NET_DEBUG_CORE=y
CONFIG_WIFI_LOG_LEVEL_DBG=y
```

Monitor USB console for detailed logs:
```
[00:00:01.000,000] <inf> wifi_cywip: WiFi subsystem init...
[00:00:02.000,000] <inf> net_dhcpv4: DHCPv4 client started
[00:00:03.000,000] <inf> net_dhcpv4: Lease acquired: 192.168.1.100
[00:00:04.000,000] <inf> main: WiFi: Client connected
```

---

## Security Notes

⚠️ **Production Recommendations**:
1. Never commit credentials to Git (add to `.gitignore`)
2. Use environment variables or config files excluded from repo
3. Consider WPA3 security if supported by network
4. Use TLS/certificate in future (not in current implementation)
5. Rotate credentials periodically
6. Monitor for unauthorized connections

---

**Last Updated**: December 5, 2025  
**Zephyr Version**: v4.2.0+  
**Board**: Raspberry Pi Pico W  
**WiFi Module**: CYW43xx
