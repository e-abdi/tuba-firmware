#!/usr/bin/env python3
"""
Simple HTTP server for OTA firmware updates.
Serves firmware binaries to ESP32 devices over WiFi.

Usage:
    python3 ota_server.py --port 8000 --firmware build/zephyr/zephyr.bin

Then connect to ESP32 WiFi, select OTA menu option, and enter:
    http://192.168.4.100:8000/zephyr.bin
"""

import http.server
import socketserver
import argparse
import os
from pathlib import Path


class OTARequestHandler(http.server.SimpleHTTPRequestHandler):
    """HTTP handler for OTA firmware downloads with logging."""
    
    def log_message(self, format, *args):
        """Log with timestamp."""
        self.server.log(f"[OTA] {format % args}")
    
    def do_GET(self):
        """Handle GET request for firmware download."""
        # Log firmware access
        if self.path.endswith('.bin'):
            file_size = os.path.getsize(os.path.join(self.directory, self.path.lstrip('/')))
            self.server.log(f"GET {self.path} ({file_size} bytes)")
        
        # Serve the file
        super().do_GET()
        
        # Log completion
        self.server.log(f"Sent {self.path}")


class OTAServer(socketserver.TCPServer):
    """OTA HTTP server with logging."""
    
    allow_reuse_address = True
    
    def __init__(self, *args, **kwargs):
        self.logs = []
        super().__init__(*args, **kwargs)
    
    def log(self, msg):
        """Log message to both console and history."""
        print(msg)
        self.logs.append(msg)


def main():
    parser = argparse.ArgumentParser(
        description='HTTP server for OTA firmware updates',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
  # Serve current directory on port 8000
  python3 ota_server.py --port 8000
  
  # Serve build directory on custom port
  python3 ota_server.py --directory build/zephyr --port 9000
  
  # Serve specific firmware file
  python3 ota_server.py --firmware build/zephyr/zephyr.bin
        '''
    )
    
    parser.add_argument('--port', type=int, default=8000,
                       help='HTTP server port (default: 8000)')
    parser.add_argument('--directory', type=str, default='.',
                       help='Directory to serve (default: current directory)')
    parser.add_argument('--firmware', type=str, default=None,
                       help='Specific firmware file to serve')
    parser.add_argument('--address', type=str, default='0.0.0.0',
                       help='Bind address (default: 0.0.0.0 - all interfaces)')
    
    args = parser.parse_args()
    
    # Determine directory and file to serve
    if args.firmware:
        firmware_path = Path(args.firmware)
        if not firmware_path.exists():
            print(f"Error: firmware file not found: {args.firmware}")
            return 1
        
        serve_dir = str(firmware_path.parent.resolve())
        firmware_url = firmware_path.name
    else:
        serve_dir = os.path.abspath(args.directory)
        firmware_url = 'zephyr.bin'
    
    if not os.path.isdir(serve_dir):
        print(f"Error: directory not found: {serve_dir}")
        return 1
    
    # Create handler with the directory
    handler = lambda *args: OTARequestHandler(*args, directory=serve_dir)
    
    try:
        server = OTAServer((args.address, args.port), handler)
        
        # Print info
        print("=" * 60)
        print("OTA Firmware Server")
        print("=" * 60)
        print(f"Serving: {serve_dir}")
        print(f"Listening on: {args.address}:{args.port}")
        print(f"\nFirmware URL for ESP32:")
        print(f"  http://<your-ip>:{args.port}/{firmware_url}")
        print(f"\nExample (if this PC is 192.168.4.100):")
        print(f"  http://192.168.4.100:{args.port}/{firmware_url}")
        print("\nPress Ctrl+C to stop")
        print("=" * 60)
        
        server.serve_forever()
        
    except KeyboardInterrupt:
        print("\n\nShutting down OTA server...")
        return 0
    except OSError as e:
        print(f"Error: {e}")
        return 1


if __name__ == '__main__':
    exit(main())
