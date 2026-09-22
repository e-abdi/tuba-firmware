#!/usr/bin/env python3
"""
tuba_console.py — reconnecting console client for the Tuba glider.

Why not just `telnet 192.168.4.1`?

  1. `telnet` gives up the moment the TCP connection breaks. In the field the
     glider's antenna dips below the surface for a few seconds, the link
     drops, and the pilot is left retyping the command by hand — usually
     right when something interesting is happening.
  2. `telnet` opens with IAC option negotiation. The glider's console is a
     plain line server that does not speak the telnet protocol: it echoes
     those negotiation bytes straight back, which is where the occasional
     burst of garbage at connect time comes from. This client sends raw
     bytes only.
  3. Nothing gets kept. Every session here is written to a capture file, so
     a dive watched from the laptop is recoverable even if the OpenLog card
     is not.

Usage:
    ./tuba_console.py                      # 192.168.4.1:23, capture in ./captures
    ./tuba_console.py --host 192.168.4.1 --port 23
    ./tuba_console.py --log mydive.txt
    ./tuba_console.py --no-timestamps      # raw capture, no [hh:mm:ss] prefixes

Type normally; lines are sent on ENTER. Ctrl-C quits.
"""

import argparse
import errno
import os
import socket
import sys
import threading
import time
from datetime import datetime, timezone

DEFAULT_HOST = "192.168.4.1"
DEFAULT_PORT = 23

# Reconnect backoff: fast at first (a wave passing over the antenna clears in
# a second or two), then slower so a powered-down glider is not hammered.
BACKOFF_START_S = 0.5
BACKOFF_MAX_S = 10.0

# Detect a silently dead link rather than waiting out the OS default.
KEEPALIVE_IDLE_S = 10
KEEPALIVE_INTVL_S = 5
KEEPALIVE_CNT = 3

# How long a send may block before we treat the link as gone.
SEND_TIMEOUT_S = 5.0


class Capture:
    """Append-only session log, flushed after every write."""

    def __init__(self, path, timestamps=True):
        self.path = path
        self.timestamps = timestamps
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        self.fh = open(path, "a", buffering=1, encoding="utf-8", errors="replace")
        self._at_line_start = True

    def note(self, text):
        """Write a client-side annotation (connects, drops, and so on)."""
        stamp = datetime.now(timezone.utc).strftime("%H:%M:%S")
        if not self._at_line_start:
            self.fh.write("\n")
            self._at_line_start = True
        self.fh.write(f"=== [{stamp}] {text} ===\n")

    def data(self, text):
        """Write bytes received from the glider, optionally timestamped."""
        if not self.timestamps:
            self.fh.write(text)
            self._at_line_start = text.endswith("\n")
            return
        # Prefix each line so a capture can be correlated with wall time.
        # Written character-wise to keep partial lines intact across reads.
        for ch in text:
            if self._at_line_start and ch not in "\r\n":
                stamp = datetime.now(timezone.utc).strftime("%H:%M:%S")
                self.fh.write(f"[{stamp}] ")
                self._at_line_start = False
            self.fh.write(ch)
            if ch == "\n":
                self._at_line_start = True

    def close(self):
        try:
            self.fh.close()
        except OSError:
            pass


def configure_socket(sock):
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
    # Per-socket keepalive tuning is Linux-specific; skip it elsewhere.
    for opt, val in (
        ("TCP_KEEPIDLE", KEEPALIVE_IDLE_S),
        ("TCP_KEEPINTVL", KEEPALIVE_INTVL_S),
        ("TCP_KEEPCNT", KEEPALIVE_CNT),
    ):
        if hasattr(socket, opt):
            try:
                sock.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), val)
            except OSError:
                pass


class Session:
    """Holds the current socket. Replaced wholesale on every reconnect."""

    def __init__(self):
        self.sock = None
        self.lock = threading.Lock()

    def set(self, sock):
        with self.lock:
            self.sock = sock

    def get(self):
        with self.lock:
            return self.sock

    def drop(self):
        with self.lock:
            if self.sock is not None:
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None


def sender_loop(session, capture, stop):
    """Read the operator's lines from stdin and forward them to the glider.

    Runs on its own thread so a broken link never blocks typing: if there is
    no connection when a line is entered, we say so rather than silently
    swallowing the command — a swallowed 'deploy' is a bad surprise.
    """
    while not stop.is_set():
        try:
            line = sys.stdin.readline()
        except (KeyboardInterrupt, ValueError):
            break
        if line == "":          # EOF (Ctrl-D)
            stop.set()
            break

        sock = session.get()
        if sock is None:
            print("  [not connected — command NOT sent, retype it once "
                  "the link is back]", file=sys.stderr)
            continue

        payload = line.rstrip("\n") + "\r"
        try:
            sock.sendall(payload.encode("utf-8", errors="replace"))
            capture.note(f"sent: {line.rstrip()!r}")
        except OSError as exc:
            print(f"  [send failed: {exc} — command NOT sent]", file=sys.stderr)
            session.drop()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--log", default=None,
                    help="capture file (default: captures/tuba_<UTC timestamp>.txt)")
    ap.add_argument("--no-timestamps", action="store_true",
                    help="do not prefix captured lines with [hh:mm:ss]")
    args = ap.parse_args()

    log_path = args.log
    if log_path is None:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        log_path = os.path.join("captures", f"tuba_{stamp}.txt")

    capture = Capture(log_path, timestamps=not args.no_timestamps)
    session = Session()
    stop = threading.Event()

    print(f"tuba console -> {args.host}:{args.port}")
    print(f"capture       -> {log_path}")
    print("reconnects automatically; Ctrl-C to quit\n")
    capture.note(f"client started, target {args.host}:{args.port}")

    tx = threading.Thread(target=sender_loop, args=(session, capture, stop),
                          daemon=True)
    tx.start()

    backoff = BACKOFF_START_S
    connected_once = False

    try:
        while not stop.is_set():
            # --- connect ---
            try:
                sock = socket.create_connection((args.host, args.port), timeout=5.0)
            except OSError as exc:
                if connected_once or backoff == BACKOFF_START_S:
                    print(f"  [connect failed: {exc}; retrying in {backoff:.1f}s]",
                          file=sys.stderr)
                time.sleep(backoff)
                backoff = min(backoff * 2, BACKOFF_MAX_S)
                continue

            configure_socket(sock)
            sock.settimeout(SEND_TIMEOUT_S)
            session.set(sock)
            backoff = BACKOFF_START_S
            connected_once = True
            print("  [connected]", file=sys.stderr)
            capture.note("connected")

            # --- receive until the link breaks ---
            while not stop.is_set():
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    # No data is normal — the glider is quiet between
                    # samples. Keepalive, not this timeout, detects a dead
                    # peer, so just keep waiting.
                    continue
                except OSError as exc:
                    if exc.errno == errno.EINTR:
                        continue
                    capture.note(f"link lost: {exc}")
                    print(f"  [disconnected: {exc}]", file=sys.stderr)
                    break
                if not data:
                    capture.note("link closed by glider")
                    print("  [disconnected]", file=sys.stderr)
                    break

                text = data.decode("utf-8", errors="replace")
                sys.stdout.write(text)
                sys.stdout.flush()
                capture.data(text)

            session.drop()
            if not stop.is_set():
                time.sleep(backoff)
                backoff = min(backoff * 2, BACKOFF_MAX_S)

    except KeyboardInterrupt:
        print("\n  [quit]", file=sys.stderr)
    finally:
        stop.set()
        session.drop()
        capture.note("client stopped")
        capture.close()
        print(f"capture written to {log_path}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
