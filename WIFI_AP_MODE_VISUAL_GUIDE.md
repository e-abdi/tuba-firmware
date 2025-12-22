# WiFi AP Mode - Visual Guide & Diagrams

## System Architecture

### Field Deployment Setup

```
┌─────────────────────────────────────────────────┐
│                    FIELD AREA                    │
│                                                   │
│  ┌────────────────────────────────────────────┐  │
│  │         Pico W AUV (in water)             │  │
│  │  ┌──────────────────────────────────────┐ │  │
│  │  │    WiFi AP: "Tuba-AUV"               │ │  │
│  │  │    IP: 192.168.0.1                   │ │  │
│  │  │    Port: 9000                        │ │  │
│  │  │    Range: ~50-100m                   │ │  │
│  │  └──────────────────────────────────────┘ │  │
│  │                                             │  │
│  └─────────────────────────────────────────────┘  │
│              ↑↓ (WiFi Signal)                     │
│              ~~~~~~~~~~~~~                        │
│              ~~~~~~~~~~~~~                        │
│              ~~~~~~~~~~~~~                        │
│  ┌─────────────────────────────────────────────┐  │
│  │    Operator Device (on boat)                │  │
│  │    ┌──────────────────────────────────┐    │  │
│  │    │  WiFi: Connected to Tuba-AUV    │    │  │
│  │    │  Terminal: telnet 192.168.0.1:9000    │  │
│  │    │                                 │    │  │
│  │    │  [Console Access Active]        │    │  │
│  │    └──────────────────────────────────┘    │  │
│  └─────────────────────────────────────────────┘  │
│                                                   │
└─────────────────────────────────────────────────┘
```

---

## Connection Flow

### Step-by-Step Process

```
1. POWER ON Pico W
   └─→ USB Console: "WiFi: Starting Access Point mode..."
   └─→ Boot sequence runs
   └─→ Wait ~3 seconds

2. WiFi AP STARTS
   └─→ USB Console: "WiFi: Access Point 'Tuba-AUV' is active"
   └─→ Network "Tuba-AUV" appears on nearby devices
   └─→ DHCP server ready (assigns IPs to clients)
   └─→ Wait ~2 more seconds

3. SOCKET SERVER READY
   └─→ USB Console: "WiFi: TCP server listening on port 9000"
   └─→ Ready to accept connections
   └─→ Still accessible over USB

4. OPERATOR CONNECTS TO WIFI
   ┌─ On laptop/phone/tablet
   ├─→ WiFi Settings → Look for "Tuba-AUV"
   ├─→ Click Connect (no password needed)
   └─→ Connected! IP assigned by Pico (typically 192.168.0.2+)

5. OPERATOR OPENS CONSOLE
   ┌─ On laptop/phone/tablet (Terminal/Shell)
   ├─→ Type: telnet 192.168.0.1 9000
   ├─→ Connection established
   └─→ Console ready!

6. CONSOLE ACTIVE
   ┌─ Pico W side:
   │  └─→ USB Console: "WiFi: Client connected from 192.168.0.2"
   │
   └─ Operator side:
      ├─→ Can type commands
      ├─→ See responses in real-time
      └─→ Type 'help' to see menu

7. TERMINATE
   ┌─ Type: exit or help command
   ├─→ Or press: Ctrl+]  then type: quit
   ├─→ Or press: Ctrl+C
   └─→ Connection closes
```

---

## Console Data Flow

### How Data Moves

```
Operator Types:     "help"
       ↓
     Terminal Input Buffer
       ↓
     TCP Socket Send (port 9000)
       ↓
   Over WiFi (2.4 GHz)
       ↓
   Pico W Receives Data
       ↓
   Socket Buffer → Parse Command
       ↓
   State Machine (ui_menu.c)
       ↓
   Generate Response: Menu Text
       ↓
   USB/WiFi Console Output
       ↓
   Socket Send → Over WiFi
       ↓
   Operator Sees Menu on Screen
```

---

## Comparison: Old vs New

### Old: WiFi STA Mode (Router)

```
                WiFi Router (2.4 GHz)
                      ↓
        ┌─────────────────────────────┐
        │  Network: "Home" / "Office" │
        │  Password: Required         │
        │  IP Range: 192.168.1.0/24   │
        └─────────────────────────────┘
                  ↙        ↖
        Pico W (Client)   Laptop (Client)
        IP: 192.168.1.XX  IP: 192.168.1.YY

Problems:
  ❌ Requires router nearby
  ❌ IP is dynamic (need to find it)
  ❌ Can't work in remote field
  ❌ Need router credentials
  ❌ Not practical for AUV deployment
```

### New: WiFi AP Mode (No Router) ✅

```
        Pico W
        ↓
        Creates Network
        ↓
    "Tuba-AUV" SSID
    (No password)
    
    ↙                    ↘
  Laptop          Mobile Device
  Connects         Connects
  IP: 192.168.0.2  IP: 192.168.0.3
  
  Both can access:
  telnet 192.168.0.1 9000

Benefits:
  ✅ No infrastructure needed
  ✅ Fixed IP (always 192.168.0.1)
  ✅ Works anywhere with Pico W
  ✅ No passwords (field-friendly)
  ✅ Perfect for remote AUV
```

---

## Network Configuration

### IP Address Scheme

```
192.168.0.0/24 Network (255 addresses)

192.168.0.1     ← Pico W (Gateway + Server)
192.168.0.2-254 ← Client devices (DHCP assigned)

Example:
  Pico W:    192.168.0.1
  Laptop 1:  192.168.0.2
  Laptop 2:  192.168.0.3
  Mobile:    192.168.0.4
  Tablet:    192.168.0.5
  (... more clients possible)
```

### TCP Socket Details

```
Listening Socket:
  ┌─────────────────────────────────┐
  │ Address:     192.168.0.1:9000  │
  │ Protocol:    TCP/IPv4           │
  │ Queue:       1 client           │
  │ Polling:     100ms intervals    │
  │ Data Format: Plain ASCII (telnet) │
  └─────────────────────────────────┘
```

---

## Boot Sequence Timeline

```
Time (seconds)  Event                           USB Console Message
─────────────────────────────────────────────────────────────────────
0.0             Power on Pico W
0.5             USB Console initialized        "=== Tuba AUV Initializing ==="
1.0             Parameters loaded              "[parameters from flash]"
1.0             WiFi task starts               "=== Initialization Complete ==="
2.0             WiFi stack initializes         "WiFi: Starting Access Point..."
2.5             AP configuration               
3.0             AP enabled                     "WiFi: Access Point 'Tuba-AUV' active"
3.5             TCP socket created
4.0             Socket listening               "WiFi: TCP server listening on port 9000"
4.0+            Ready for connections

Field timing:
0-3s  : Wait for AP to appear in WiFi list
3-5s  : Connect to "Tuba-AUV" network
5-6s  : Open telnet 192.168.0.1 9000
6+s   : Console operational ✅
```

---

## Range Visualization

```
Pico W Access Point (AP) Coverage

                    50m typical range
                    ~~~~~~~~~~~~~~~
              ~~~~~~~~~~~~~~~~~~~~~
          ~~~~~~~~~~~~ Pico W ~~~~~~~~~~~~
          ~                             ~
       ~                                  ~
     ~                                      ~
    ~  Good Signal                            ~
   ~   (up to 100m)                            ~
  ~                                             ~
 ~     100m max (line-of-sight)                  ~
~~~~~~                                      ~~~~~~
      ↑
    Obstacles Reduce Range:
      - Walls: 50% reduction
      - Water: 30% reduction  
      - Buildings: 70% reduction
      - Line-of-sight: Full range
```

---

## Key Files & Their Purpose

```
/home/ehsan/zephyrproject/tuba/
│
├── SOURCE CODE
│   ├── src/main.c
│   │   ├── wifi_ap_setup_task()     ← Main WiFi AP code
│   │   ├── wifi_listener_fd          ← Server socket
│   │   └── wifi_socket_fd            ← Client connection
│   │
│   └── prj.conf
│       └── CONFIG_WIFI_CYWIP_*       ← AP mode settings
│
├── DOCUMENTATION
│   ├── WIFI_AP_MODE_QUICK_START.md
│   │   └── 3-step quick start ⭐ START HERE
│   │
│   ├── WIFI_AP_MODE_IMPLEMENTATION.md
│   │   └── Technical details & complete guide
│   │
│   ├── MIGRATION_AP_MODE.md
│   │   └── What changed from old STA mode
│   │
│   ├── WIFI_CONSOLE_README.md
│   │   └── Full technical reference
│   │
│   └── README_WIFI_CONSOLE.md
│       └── Navigation/index of all docs
│
└── CONFIGURATION
    └── boards/rpi_pico.overlay
        └── Device tree (console routing)
```

---

## Typical Use Case: River Survey

```
Day: Remote AUV deployment on river

Morning:
  └─→ Power on Pico W at river
  └─→ Place in waterproof case with floats
  └─→ Lower into water via tether

Operator (on bridge):
  ├─→ Laptop ready with telnet
  ├─→ WiFi connects to "Tuba-AUV" (strong signal from bridge)
  ├─→ "telnet 192.168.0.1 9000"
  ├─→ Commands sent to AUV:
  │   ├─ Check sensor readings (depth, temperature)
  │   ├─ Deploy pump (buoyancy)
  │   ├─ Adjust motors (pitch, roll)
  │   └─ Monitor real-time status
  │
  └─→ Data streams back immediately

Evening:
  ├─→ Retrieve AUV from water
  ├─→ Connect USB to laptop
  ├─→ Download logged data (flash persistence)
  └─→ Process results

Benefits:
  ✅ No wired connection needed
  ✅ Mobile operator (move around on bridge)
  ✅ Real-time monitoring
  ✅ Independent control
  ✅ Safe distance from water
```

---

## Troubleshooting Flowchart

```
Problem: Can't connect to Pico

    ↓
Do you see "Tuba-AUV" in WiFi list?
    ├─→ NO  → Is Pico powered? → Check USB console
    │                              for boot messages
    │
    └─→ YES → Can you connect to WiFi?
            ├─→ NO  → Try again
            │         (sometimes fails first try)
            │
            └─→ YES → Can you ping 192.168.0.1?
                    ├─→ NO  → Wait 5 more seconds
                    │          (network initializing)
                    │
                    └─→ YES → Can you telnet?
                            ├─→ NO  → Check port (9000)
                            │          Type exact: telnet 192.168.0.1 9000
                            │
                            └─→ YES → Connected! ✅
                                    Type: help
```

---

## Hardware Connections

```
Raspberry Pi Pico W (top-down view)

    ┌─────────────────────────┐
    │   Pico W Board          │
    │                         │
    │  [USB Port] ← USB to Computer (optional)
    │             └─→ Console Output
    │                 (USB CDC ACM)
    │
    │  [WiFi Antenna] ← CYW43xx Radio
    │                   Creates AP: "Tuba-AUV"
    │
    │  [I2C Pins] ← Sensors
    │  [SPI Pins] ← Motors/Pump
    │  [GPIO]     ← Various functions
    │
    └─────────────────────────┘
         ↓
    Device Field Placement
    (waterproof case + floats)
         ↓
    Connected to AUV via:
    - Tether (optional)
    - Direct mounting
    - Floating above
```

---

## Power Considerations

```
Power Budget (Pico W):

Base System:     ~30mA (Pico ARM core + SRAM)
WiFi AP Mode:    +20mA (radio active)
Full System:     ~50mA typical

Total:           50mA @ 3.3V = 165mW

Battery Runtime (examples):
  500mAh battery  → ~10 hours
  2000mAh battery → ~40 hours
  USB powered     → Unlimited

Note: Add sensor + motor current
      Total system may be 100-200mA
      depending on motor usage
```

---

## Summary Diagram

```
    ┌─────────────────────────────────┐
    │   Pico W in Field (No USB)      │
    │   ┌─────────────────────────┐   │
    │   │ AP: "Tuba-AUV"          │   │
    │   │ IP: 192.168.0.1         │   │
    │   │ Port: 9000              │   │
    │   │ Ready!                  │   │
    │   └─────────────────────────┘   │
    └────────────────┬─────────────────┘
                     │ WiFi Radio
                     │ 2.4 GHz
         ┌───────────┴───────────┐
         │                       │
    ┌────▼─────┐           ┌─────▼────┐
    │  Laptop  │           │ Tablet   │
    │ telnet   │           │ telnet   │
    │ 192.0.1  │           │ 192.0.1  │
    │ :9000    │           │ :9000    │
    └──────────┘           └──────────┘

Result: Console access from multiple
        devices simultaneously! ✅
```

---

**Visual Guide Created**: December 5, 2025  
**Board**: Raspberry Pi Pico W  
**RTOS**: Zephyr v4.2.0+
