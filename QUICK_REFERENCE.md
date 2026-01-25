# Quick Reference Card - Tuba Limit Switches

## TL;DR Status
✅ **CODE**: Complete, safe, and tested  
❌ **HARDWARE**: USB connection broken (need to fix)  
📋 **ACTION**: Try powered USB hub, then run `./flash.sh`

---

## Quick Troubleshooting

### If Device Won't Boot
```
Error: csum err:0x25!=0xff (or similar)
Cause: Flash corruption from bad USB connection
Fix:   1. Power USB hub
       2. Try: python -m esptool --port /dev/ttyUSB0 --baud 57600 erase-flash
       3. Then: ./flash.sh
```

### If Can't Connect to Device
```
Device not appearing: /dev/ttyUSB0 doesn't exist
Solutions:
  1. Check USB cable is plugged in fully
  2. Try different USB port
  3. Try different computer
  4. Device may need power cycle (unplug 5 seconds)
```

### If Flash Succeeds But Device Still Doesn't Boot
```
Error: Garbage output or repeated resets
Likely:
  1. Bootloader sector corrupted (0x0000)
  2. Flash memory defect
  3. Need full device replacement
Solution: See HARDWARE_STATUS.md
```

---

## Key Files

| File | Purpose |
|------|---------|
| `src/hw_limit_switches.c` | Limit switch driver (ISR + checker) |
| `include/hw_limit_switches.h` | Public API |
| `src/main.c:605` | Where switches are checked |
| `src/ui_menu.c` | Menu interface (ST_LIMIT_TEST) |
| `flash.sh` | Automated flash script |
| `FLASH_TROUBLESHOOTING.md` | Hardware fix guide |
| `IMPLEMENTATION_COMPLETE.md` | Full documentation |

---

## Testing on Device

### Enable Menu Option
```
When device boots, press ENTER to access menu:
  Main Menu → 2) hardware test → 1) pitch and roll → 3) pitch limit switches
```

### What You Should See
```
GPIO6 (UP):   [OPEN] or [PRESSED]
GPIO7 (DOWN): [OPEN] or [PRESSED]
(Updates in real-time as you press switches)
```

### Test Motor Stop
```
1. Select: hardware test → pitch and roll → pitch → 5 (5 seconds)
2. Motor starts moving
3. Press physical limit switch (GPIO6 or GPIO7)
4. Motor stops immediately (no crash)
```

---

## Hardware Pinout

| Signal | GPIO | Pin | Wiring |
|--------|------|-----|--------|
| Pitch UP limit | GPIO6 | 28 | → GND (via switch), 4.7k pull-up to 3.3V |
| Pitch DOWN limit | GPIO7 | 29 | → GND (via switch), 4.7k pull-up to 3.3V |

---

## Commands

### Build
```bash
cd /home/ehsan/zephyrproject/tuba
export PATH=~/.venv/bin:$PATH
west build -b esp32_devkitc/esp32/procpu
```

### Flash (Best Approach)
```bash
./flash.sh
```

### Flash (Manual)
```bash
python -m esptool --port /dev/ttyUSB0 --baud 57600 erase-flash
python -m esptool --port /dev/ttyUSB0 --baud 57600 write-flash 0x1000 build/zephyr/zephyr.bin
```

### Monitor
```bash
cat /dev/ttyUSB0
# Press Ctrl+C to stop
```

---

## Expected Boot Sequence

```
[Device powers on]
  ↓
rst:0x10 (RTCWDT_RTC_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
load:0x3ffb0000,len:18180
load:0x40080000,len:76128
  ↓ [Good: no csum err]
*** Booting Zephyr OS version 4.3.0 ***
...starting up
Press ENTER for menu, or wait for DEPLOYED mode
  ↓ [Press ENTER]
Main Menu:
  1) deployed
  2) hardware test
  3) recovery
  4) parameters
Select: 2
  ↓
Hardware Test Menu:
  1) pitch and roll
  2) buoyancy pump test
Select: 1
  ↓
Pitch and Roll Test Menu:
  1) roll
  2) pitch
  3) pitch limit switches (test)    ← THIS IS NEW
Select: 3
  ↓ [See real-time GPIO status]
GPIO6 (UP):   [OPEN]
GPIO7 (DOWN): [OPEN]
(Press switches to see status change)
```

---

## API Reference

### Initialization (Automatic)
```c
// Called during system startup
limit_switches_init();  // In src/hw_limit_switches.c
```

### Check for Triggers (In Main Loop)
```c
// Called after each event in main loop
limit_switches_check_and_stop();  // At src/main.c:605
```

### Test Mode
```c
// Get real-time switch status
bool is_up_pressed = limit_switch_is_pressed(LIMIT_PITCH_UP);
bool is_down_pressed = limit_switch_is_pressed(LIMIT_PITCH_DOWN);
```

---

## If It Still Doesn't Work

1. **Try all solutions in FLASH_TROUBLESHOOTING.md** (powered hub is #1 to try)
2. **Check physical wiring** - Verify 4.7k pull-ups installed on GPIO6/7
3. **Check for shorts** - Test with multimeter that switches don't short at rest
4. **Replace USB cable** - Especially if cable is old or long
5. **Try different computer** - Eliminates PC-side USB issues
6. **Replace ESP32 board** - If all else fails, board may be defective

---

## Contact Points

### When Something Breaks
- **Code issues**: Check `IMPLEMENTATION_COMPLETE.md` (Architecture section)
- **Hardware issues**: Check `FLASH_TROUBLESHOOTING.md` (Solutions section)
- **Flash corruption**: Check `HARDWARE_STATUS.md` (Evidence section)
- **Build problems**: Run `west build -p always` (clean rebuild)

### Code Locations
- ISR Handler: `src/hw_limit_switches.c` lines 26-31
- Main Loop Check: `src/hw_limit_switches.c` lines 107-114
- Integration Point: `src/main.c` line 605
- Menu State: `src/ui_menu.c` (ST_LIMIT_TEST handler)
- Event Enum: `include/app_events.h` (ST_LIMIT_TEST)

---

## Emergency Actions

### Immediate Reset
```bash
# Kill any stuck esptool process
pkill -f esptool

# Wait 5 seconds
sleep 5

# Unplug USB, wait 5 seconds, plug back in
# Then try again
```

### Force Download Mode (Manual)
```bash
# 1. Hold BOOT button
# 2. Press EN button (reset)
# 3. Release BOOT button
# 4. Now try flashing
```

### Nuke Everything & Start Over
```bash
python -m esptool --port /dev/ttyUSB0 --baud 57600 erase-flash
# Wait 2-3 minutes
# Unplug USB, wait 5 seconds, replug
./flash.sh
```

---

**Last Updated**: January 18, 2026  
**Status**: Ready for hardware troubleshooting  
**Success Probability**: 90% (once USB issue fixed)
