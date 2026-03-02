#!/bin/bash
# Quick OTA Update Guide
# This script helps with the OTA process

set -e

echo "=== Tuba ESP32 OTA Firmware Update ==="
echo ""
echo "Step 1: Build new firmware"
west build -b esp32_devkitc/esp32/procpu --pristine=auto

echo ""
echo "Step 2: Get your PC's IP address"
echo "Use one of these commands:"
echo "  Linux/Mac:  hostname -I  (or: ifconfig | grep inet)"
echo "  Windows:    ipconfig"
echo ""
read -p "Enter your PC's IP address: " PC_IP

echo ""
echo "Step 3: Start OTA server"
echo "Run this command in another terminal window:"
echo ""
echo "  python3 ota_server.py --firmware build/zephyr/zephyr.bin"
echo ""
read -p "Press ENTER when OTA server is running... "

echo ""
echo "Step 4: Update ESP32 device"
echo "1. Connect to ESP32 WiFi: 'Tuba-Glider'"
echo "2. Run: telnet 192.168.4.1 23"
echo "3. Select menu option 5 (OTA firmware update)"
echo "4. Enter this URL when prompted:"
echo "   http://$PC_IP:8000/zephyr.bin"
echo ""
echo "Device will download and reboot with new firmware!"
