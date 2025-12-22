# WiFi Console Support for Tuba AUV (Raspberry Pi Pico W)

## Overview

The Tuba AUV firmware now supports WiFi console access for Pico W boards, enabling remote debugging and monitoring without USB cables. The console can be accessed via TCP socket on port 9000.

## Available Console Modes

Three console modes are available and can be switched via configuration:

### Option 1: USB CDC ACM (Default - Currently Active)
- **Best for**: Development with USB cable connected
- **Activation**: `CONFIG_USB_DEVICE_STACK=y` in `prj.conf`
- **Device tree**: `zephyr,console = &cdc_acm_uart;` in overlay
- **Latency**: Immediate
- **Range**: Limited to USB cable length

### Option 2: UART Serial (MAX3232)
- **Best for**: Embedded deployment with external converter
- **Activation**: Uncomment UART section in `prj.conf`, set `zephyr,console = &uart0;` in overlay
- **Device tree**: Requires MAX3232 serial converter connected to UART0
- **Latency**: Low
- **Range**: Limited by serial cable/wireless bridge

### Option 3: WiFi Socket (Pico W Only)
- **Best for**: Field testing, remote monitoring
- **Activation**: Uncomment WiFi section in `prj.conf`, activate `CONFIG_WIFI_CYWIP=y`
- **Port**: TCP 9000 (configurable in code)
- **Latency**: ~100ms polling interval
- **Range**: WiFi network coverage

## Switching Console Modes

### To Enable WiFi Console:

1. **Edit `prj.conf`:**
   ```bash
   # Option 1: Disable USB (if desired)
   # CONFIG_USB_DEVICE_STACK=y
   
   # Option 3: Enable WiFi
   CONFIG_WIFI=y
   CONFIG_WIFI_CYWIP=y
   CONFIG_NET_SOCKETS=y
   CONFIG_NET_TCP=y
   CONFIG_NET_IPV4=y
   CONFIG_NET_DHCPV4=y
   ```

2. **Edit `boards/rpi_pico.overlay`:**
   ```dts
   /* Option 3: WiFi socket console */
   zephyr,console = &cdc_acm_uart;  /* Keep USB active as fallback */
   /* WiFi socket console will be available on port 9000 */
   ```

3. **Configure WiFi credentials** (method depends on your Zephyr WiFi driver setup):
   - Default: Check Zephyr's WiFi driver documentation for credential storage
   - Alternative: Add menu option in `ui_menu.c` to configure WiFi interactively

4. **Rebuild:**
   ```bash
   cd /home/ehsan/zephyrproject/tuba
   source ../zephyr/zephyr-env.sh
   west build -b rpi_pico
   west flash
   ```

## Accessing WiFi Console

### Method 1: Using `nc` (netcat)
```bash
# Find Pico W's IP address (check router DHCP list or use mDNS)
# If mDNS is available: nc <hostname>.local 9000
nc 192.168.1.100 9000

# Pico W will print: "WiFi: Client connected"
# Now interact with console normally
# Press Ctrl+C to disconnect
```

### Method 2: Using `telnet`
```bash
telnet 192.168.1.100 9000
```

### Method 3: Using Python
```python
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("192.168.1.100", 9000))
sock.send(b"help\n")
print(sock.recv(1024).decode())
sock.close()
```

### Method 4: Using `socat` (advanced)
```bash
# Local serial-like access:
socat - TCP:192.168.1.100:9000
```

## Technical Details

### Socket Server Implementation

The WiFi socket server runs in a dedicated thread:

```c
K_THREAD_DEFINE(wifi_server, 2048, wifi_socket_server_task, NULL, NULL, NULL, 5, 0, 0);
```

- **Stack size**: 2048 bytes
- **Priority**: 5 (normal)
- **Port**: 9000
- **Protocol**: TCP IPv4
- **Binding**: INADDR_ANY (all interfaces)

### Console Data Flow

1. **USB Console** (if enabled): Primary console for debugging
2. **WiFi Socket** (if enabled): Secondary console via TCP
3. **UART** (if enabled): Hardware serial console

Data written to `app_printk()` goes to the configured console device (via Zephyr's logging subsystem).

### WiFi Startup Sequence

1. USB initializes first (if CONFIG_USB_DEVICE_STACK=y)
2. Parameters load from flash
3. WiFi stack initializes via CONFIG_WIFI_CYWIP
4. WiFi socket server thread starts, waits 2 seconds for network
5. Server binds to port 9000 and listens for connections
6. Main state machine begins

### Configuration Summary

**Required Kconfig symbols** (add to `prj.conf`):
```
CONFIG_WIFI=y
CONFIG_WIFI_CYWIP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_TCP=y
CONFIG_NET_IPV4=y
CONFIG_NET_DHCPV4=y
CONFIG_HEAP_MEM_POOL_SIZE=16384
```

**Optional symbols** (tuning):
```
CONFIG_NET_SOCKET_OFFLOAD_POLL=y          # For better socket polling
CONFIG_NET_CONFIG_INIT_PRIO=999           # Delay network init slightly
CONFIG_NET_LOG=y                          # Enable network logging
```

## Troubleshooting

### "WiFi: Failed to create socket"
- **Cause**: Network stack not ready or out of memory
- **Solution**: Increase `CONFIG_HEAP_MEM_POOL_SIZE` in `prj.conf`

### Connection refused
- **Cause**: Socket server not listening (may still initializing)
- **Solution**: Wait 3-5 seconds after power-on, check USB console for startup messages

### Data appears garbled
- **Cause**: Encoding mismatch or buffering issue
- **Solution**: Use `nc -l` with explicit ASCII mode; check WiFi signal strength

### Cannot find Pico W on network
- **Cause**: DHCP not working or credentials invalid
- **Solution**: Check router logs; use mDNS scanner (`dns-sd -B _http._tcp local`)

### Frequent disconnections
- **Cause**: WiFi interference or sleep mode conflict
- **Solution**: Move closer to router; disable power management features if available

## Future Enhancements

1. **WiFi credentials menu**: Add interactive option to configure SSID/password
2. **mDNS hostname**: Register Pico W with hostname for easier discovery
3. **WebSocket console**: Browser-based console access
4. **TLS/certificate support**: Encrypted WiFi console for security
5. **Multiple client support**: Handle multiple simultaneous connections
6. **Console logging**: Log WiFi console activity to flash storage

## Files Modified

- `src/main.c`: Added WiFi socket server task and includes
- `prj.conf`: Added WiFi configuration options (commented)
- `boards/rpi_pico.overlay`: Added console routing comments for WiFi mode

## Related Files

- `app_params.c`: Flash persistence (unchanged)
- `include/hw_motors.h`: Motor control (unchanged)
- `include/hw_pump.h`: Buoyancy pump (unchanged)
- `ui_menu.c`: User interface state machine (unchanged for now)

## Testing Checklist

- [ ] Build completes without errors for `rpi_pico` board with WiFi disabled
- [ ] Build completes for `rpi_pico_w` board with `CONFIG_WIFI_CYWIP=y`
- [ ] USB console works (default configuration)
- [ ] WiFi connection establishes within 5 seconds of boot
- [ ] Socket server listens on port 9000
- [ ] Client connects successfully via `nc`
- [ ] Commands typed via WiFi execute correctly
- [ ] Response data appears on WiFi console
- [ ] Disconnection handled gracefully
- [ ] Multiple connections handled properly (or error message clear)

---

**Last Updated**: December 5, 2025  
**Board**: Raspberry Pi Pico W  
**RTOS**: Zephyr v4.2.0+  
**WiFi Module**: CYW43xx (Pico W on-board)
