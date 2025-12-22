# WiFi Console Configuration Guide

## Quick Start

This document shows the exact configuration changes needed to enable WiFi console support.

## Configuration Files

### 1. `prj.conf` - Build Configuration

Add the following to enable WiFi socket console. You can keep USB enabled as a fallback.

**Current state** (USB active):
```ini
CONFIG_USB_DEVICE_STACK=y
CONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=n
```

**To enable WiFi** (uncomment/add these lines):
```ini
# Option 3: WiFi Socket Console
CONFIG_WIFI=y
CONFIG_WIFI_CYWIP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_TCP=y
CONFIG_NET_IPV4=y
CONFIG_NET_DHCPV4=y
CONFIG_NET_CORE=y
CONFIG_NET_NATIVE=y
CONFIG_NET_NATIVE_TCP=y

# Increase heap for network stack
CONFIG_HEAP_MEM_POOL_SIZE=16384

# Optional: Network logging
# CONFIG_NET_LOG=y
```

### 2. `boards/rpi_pico.overlay` - Device Tree

The overlay already supports console selection. No changes needed for WiFi socket mode - it uses UDP/TCP sockets, not device tree nodes.

The chosen console device controls only STDIN/STDOUT routing:
- Keep `zephyr,console = &cdc_acm_uart;` for USB to remain active
- Or change to `zephyr,console = &uart0;` for UART (MAX3232)
- WiFi socket server is independent and always listens on TCP 9000

## Code Changes

### 3. `src/main.c` - Initialization

Already modified to:
1. Include WiFi headers (`#ifdef CONFIG_WIFI_CYWIP`)
2. Define WiFi socket server task (`wifi_socket_server_task`)
3. Create WiFi thread (`K_THREAD_DEFINE`)
4. Add WiFi status message during boot

**Relevant code sections:**
```c
/* Lines 10-15: WiFi includes */
#ifdef CONFIG_WIFI_CYWIP
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/socket.h>
#endif

/* Lines 136-200: WiFi socket server */
#ifdef CONFIG_WIFI_CYWIP
static int wifi_socket_fd = -1;
static int wifi_listener_fd = -1;
/* ... socket server implementation ... */
#endif

/* Lines 241-244: WiFi startup message */
#ifdef CONFIG_WIFI_CYWIP
    app_printk("WiFi enabled: Waiting for network setup...\r\n");
#endif
```

## Compilation

### Build for Pico (USB console):
```bash
cd /home/ehsan/zephyrproject/tuba
source ../zephyr/zephyr-env.sh
west build -b rpi_pico
west flash
```

### Build for Pico W (with WiFi):
```bash
cd /home/ehsan/zephyrproject/tuba
source ../zephyr/zephyr-env.sh

# Edit prj.conf to uncomment WiFi section (see above)

west build -b rpi_pico  # Still build for rpi_pico, but with WiFi enabled
west flash
```

**Note**: The `rpi_pico_w` board in Zephyr is an alias that selects the correct WiFi radio driver. You may use either `-b rpi_pico` or `-b rpi_pico_w` - both work with `CONFIG_WIFI_CYWIP=y`.

## WiFi Credentials

Zephyr's `CONFIG_WIFI_CYWIP` driver behavior depends on your Zephyr version:

### Option A: Using Zephyr's WiFi configuration subsystem
```c
// In your application code (e.g., ui_menu.c):
#include <zephyr/net/wifi_mgmt.h>

static struct wifi_connect_req_params wifi_params = {
    .ssid = "YourSSID",
    .psk = "YourPassword",
    .security = WIFI_SECURITY_TYPE_PSK,
};

struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_CTX(WIFI));
net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &wifi_params, sizeof(wifi_params));
```

### Option B: Using Kconfig for hardcoded credentials (testing only)
```ini
# In prj.conf (NOT RECOMMENDED for production)
CONFIG_WIFI_CYWIP_SSID="YourSSID"
CONFIG_WIFI_CYWIP_PSK="YourPassword"
```

### Option C: Manual connection in main.c
```c
// After network stack is ready
struct wifi_connect_req_params conn_params = {
    .ssid = "YourSSID",
    .psk = "YourPassword",
    .security = WIFI_SECURITY_TYPE_PSK,
};
// ... connect ...
```

## Verification Steps

1. **Build succeeds**:
   ```bash
   west build -b rpi_pico 2>&1 | tail -20  # Should show no errors
   ```

2. **Flash the device**:
   ```bash
   west flash
   ```

3. **Monitor USB console**:
   ```bash
   picocom /dev/ttyACM0 115200  # or minicom, miniterm.py, etc.
   # Should see: "WiFi enabled: Waiting for network setup..."
   # Then: "WiFi: Client connected" (after WiFi connects)
   # And: "WiFi console server listening on port 9000"
   ```

4. **Find Pico W IP address**:
   ```bash
   # Check your router's DHCP client list
   # Or use network scanner: nmap, Angry IP Scanner, etc.
   # Or if mDNS is enabled: Pico_W.local or similar
   ```

5. **Connect via socket**:
   ```bash
   nc 192.168.1.100 9000
   # Type: help
   # Should execute and show menu
   ```

## Console Modes Comparison

| Feature | USB (Current) | UART (MAX3232) | WiFi (Socket) |
|---------|---------------|----------------|---------------|
| **Range** | ~2m (cable) | 10m+ (with extension) | 50m+ (WiFi range) |
| **Setup** | Just plug-in | External converter | Router + network |
| **Power** | USB power | Requires voltage divider | WiFi power draw (~50mA) |
| **Speed** | High (USB) | 115200 baud typical | Depends on WiFi (good) |
| **Reliability** | Very high | High (serial) | Good (WiFi interference) |
| **Cost** | Built-in | $2-5 (MAX3232) | $0 (Pico W built-in) |
| **Development** | Excellent | Good | Good (latency ~100ms) |
| **Field Deployment** | Cable required | Best for fixed install | Best for mobile |

## Switching Between Modes

### To use USB Console only (default):
```bash
# In prj.conf:
CONFIG_USB_DEVICE_STACK=y
# (Everything else commented out)

# In boards/rpi_pico.overlay:
zephyr,console = &cdc_acm_uart;
```

### To use UART Console:
```bash
# In prj.conf:
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y

# In boards/rpi_pico.overlay:
zephyr,console = &uart0;
```

### To use WiFi Socket Console (keeping USB as fallback):
```bash
# In prj.conf:
CONFIG_USB_DEVICE_STACK=y
CONFIG_WIFI=y
CONFIG_WIFI_CYWIP=y
CONFIG_NET_SOCKETS=y
# ... (full WiFi config from above)

# In boards/rpi_pico.overlay:
zephyr,console = &cdc_acm_uart;  # USB for debug, WiFi for remote access
```

## Memory Considerations

WiFi support increases memory usage:

| Component | Usage |
|-----------|-------|
| **WiFi stack** | ~200KB Flash |
| **Network stack** | ~100KB Flash |
| **Heap pool** | Configured via `CONFIG_HEAP_MEM_POOL_SIZE` |
| **WiFi socket server thread** | 2KB stack |

**Total Flash used** (approx):
- USB only: ~150KB
- WiFi enabled: ~450KB
- Available on Pico: ~1920KB (enough for both)

## Debugging

### Enable verbose logging:
```ini
# In prj.conf:
CONFIG_NET_LOG=y
CONFIG_NET_DEBUG_CORE=y
CONFIG_NET_DEBUG_TCP=y
CONFIG_WIFI_LOG_LEVEL_DBG=y
```

### Monitor logs:
```bash
# USB console will show:
# [00:00:01.234,000] <inf> wifi_cywip: WiFi subsystem initializing...
# [00:00:02.345,000] <inf> net_dhcpv4: DHCPv4 client started...
# [00:00:03.456,000] <inf> net_dhcpv4: Lease acquired: 192.168.1.100
```

### Check WiFi connection:
```bash
# In Zephyr shell (if enabled):
> wifi connect YourSSID YourPassword 2
# Output: status: Successfully connected
```

## Common Issues

| Issue | Solution |
|-------|----------|
| "Failed to create socket" | Increase `CONFIG_HEAP_MEM_POOL_SIZE` to 32768 |
| Connection refused | Wait 5 seconds after boot for WiFi to connect |
| Socket not listening | Check USB console for errors, verify WiFi enabled |
| Slow response | WiFi console polls at 100ms intervals; expected latency ~100-200ms |
| Frequent disconnects | Move closer to router; check interference |

## Next Steps

1. Edit `prj.conf` with WiFi configuration (copy section above)
2. Run `west build -b rpi_pico`
3. Run `west flash`
4. Monitor USB console for WiFi connection
5. Connect via `nc <pico-ip> 9000`
6. Test commands via WiFi console

---

**Created**: December 5, 2025  
**Board**: Raspberry Pi Pico W  
**WiFi Module**: CYW43xx  
**RTOS**: Zephyr v4.2.0+
