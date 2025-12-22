# Console Mode Quick Reference

## One-Command Mode Switching

### Mode 1: USB CDC ACM (Current Default)

**Configuration:**
```bash
# In prj.conf:
CONFIG_USB_DEVICE_STACK=y

# In boards/rpi_pico.overlay:
zephyr,console = &cdc_acm_uart;
```

**Build & Test:**
```bash
cd /home/ehsan/zephyrproject/tuba
source ../zephyr/zephyr-env.sh
west build -b rpi_pico && west flash
picocom /dev/ttyACM0 115200
```

**Access:** USB cable only

---

### Mode 2: UART Serial (MAX3232)

**Configuration:**
```bash
# In prj.conf:
CONFIG_SERIAL=y
CONFIG_UART_INTERRUPT_DRIVEN=y

# In boards/rpi_pico.overlay:
zephyr,console = &uart0;
```

**Build & Test:**
```bash
cd /home/ehsan/zephyrproject/tuba
source ../zephyr/zephyr-env.sh
west build -b rpi_pico && west flash
picocom /dev/ttyUSB0 115200  # or /dev/ttyS0 depending on MAX3232 connection
```

**Access:** Serial cable + MAX3232 converter

---

### Mode 3: WiFi Socket (Pico W)

**Configuration:**
```bash
# In prj.conf - UNCOMMENT these lines:
CONFIG_WIFI=y
CONFIG_WIFI_CYWIP=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_TCP=y
CONFIG_NET_IPV4=y
CONFIG_NET_DHCPV4=y
CONFIG_NET_CORE=y
CONFIG_NET_NATIVE=y
CONFIG_NET_NATIVE_TCP=y
CONFIG_HEAP_MEM_POOL_SIZE=16384

# In boards/rpi_pico.overlay:
zephyr,console = &cdc_acm_uart;  # Keep USB as fallback
```

**Build & Test:**
```bash
cd /home/ehsan/zephyrproject/tuba
source ../zephyr/zephyr-env.sh
west build -b rpi_pico && west flash

# Monitor USB for WiFi connection messages
picocom /dev/ttyACM0 115200

# In another terminal, find Pico IP from router DHCP list:
nc 192.168.1.100 9000
```

**Access:** WiFi network + TCP port 9000

---

## Platform Compatibility

| Mode | Pico | Pico W | Pico 2 |
|------|------|--------|--------|
| USB | ✅ | ✅ | ✅ |
| UART | ✅ | ✅ | ✅ |
| WiFi | ❌ | ✅ | ❌* |

*Pico 2 may have WiFi variant in future

---

## Switching Checklist

To change modes:

- [ ] Edit `prj.conf` (enable target mode config)
- [ ] Edit `boards/rpi_pico.overlay` (set correct console device)
- [ ] Run `west build -b rpi_pico`
- [ ] Run `west flash`
- [ ] Connect to console via appropriate method
- [ ] Verify with `help` command

---

## Useful Commands

### Find Pico W IP Address
```bash
# Method 1: Router DHCP client list (most reliable)
192.168.1.1  # Navigate to your router admin page

# Method 2: Network scanner
nmap -sn 192.168.1.0/24

# Method 3: DNS-SD (if mDNS enabled)
dns-sd -B _services._dns-sd._udp local
```

### Connect to WiFi Console
```bash
# netcat (most common)
nc <pico-ip> 9000

# telnet
telnet <pico-ip> 9000

# socat (piping to file)
socat - TCP:<pico-ip>:9000 | tee wifi_console.log

# Python
python3 -c "import socket; s = socket.socket(); s.connect(('<pico-ip>', 9000)); print(s.recv(1024))"
```

### Monitor USB Console (any mode)
```bash
# picocom
picocom /dev/ttyACM0 115200

# miniterm.py
python3 -m serial.tools.miniterm /dev/ttyACM0 115200

# minicom
minicom -D /dev/ttyACM0 -b 115200

# screen
screen /dev/ttyACM0 115200
```

### Build with Verbose Output
```bash
west build -b rpi_pico -- -v  # Verbose Ninja
west build -b rpi_pico -- -d explain  # Explain build rules
```

---

## Typical Workflow

### Development (USB + WiFi)
```bash
# Build once with both enabled
vi prj.conf  # Uncomment WiFi section
west build -b rpi_pico && west flash

# Terminal 1: USB debug console
picocom /dev/ttyACM0 115200

# Terminal 2: WiFi remote console
nc <pico-ip> 9000

# Terminal 3: Build monitor
west build -b rpi_pico  # Rebuild as needed
```

### Field Deployment (WiFi only)
```bash
# Build with USB disabled, WiFi only
vi prj.conf  # Disable USB, keep WiFi enabled
west build -b rpi_pico && west flash

# Remote access from any device on network
nc <pico-ip> 9000
# or web interface (future enhancement)
```

### Production Testing (UART only)
```bash
# Build with serial console, disable USB/WiFi
vi prj.conf  # Enable UART, disable USB/WiFi
west build -b rpi_pico && west flash

# Connect serial console
picocom /dev/ttyUSB0 115200
```

---

## File Locations

```
/home/ehsan/zephyrproject/tuba/
├── prj.conf                          ← Edit for mode selection
├── boards/rpi_pico.overlay           ← Edit for console routing
├── src/main.c                         ← Contains WiFi socket server
├── WIFI_CONSOLE_README.md            ← Detailed user guide
├── WIFI_CONFIG_CHECKLIST.md          ← Configuration steps
└── IMPLEMENTATION_SUMMARY.md         ← Technical overview
```

---

## Troubleshooting Quick Links

| Problem | Solution | Reference |
|---------|----------|-----------|
| Build fails | Check `CONFIG_HEAP_MEM_POOL_SIZE` | WIFI_CONFIG_CHECKLIST.md |
| USB not recognized | Try different USB port/cable | WIFI_CONSOLE_README.md |
| WiFi not connecting | Check credentials, signal strength | WIFI_CONSOLE_README.md § Troubleshooting |
| Socket connection refused | Wait 5 seconds after boot | WIFI_CONSOLE_README.md § Troubleshooting |
| Garbled output | Check baud rate, encoding | WIFI_CONFIG_CHECKLIST.md |

---

**Last Updated**: December 5, 2025  
**Quick Reference Version**: 1.0  
**Board**: Raspberry Pi Pico / Pico W  
**RTOS**: Zephyr v4.2.0+
