#!/usr/bin/env python3
"""Send touch commands to Azahar's stream touch input (UDP).

Protocol (10 bytes, little-endian):
  [0]     magic 0x54 ('T')
  [1]     flags: bit 0 = pressed
  [2..5]  float x, normalized 0.0 (left) .. 1.0 (right)
  [6..9]  float y, normalized 0.0 (top) .. 1.0 (bottom)

Coordinates are relative to the visible game screen content area. The
stream receiver must account for letterboxing in the video frame: map the
local click position into the screen content rectangle first, then send
normalized coordinates (0..1) of that rectangle. The emulator forwards them
directly to the 3DS touch screen (320x240).

Receiver integration example (Python, e.g. in a touch-enabled player):
    def on_click(content_rect, click_pos):
        x = (click_pos[0] - content_rect.x) / content_rect.w
        y = (click_pos[1] - content_rect.y) / content_rect.h
        send(x, y, True)      # press
        send(x, y, False)     # release (after a short hold)

Usage:
  stream_touch_client.py x y [duration_ms] [port]
    x, y           normalized 0..1 coordinates of the tap
    duration_ms    hold time in ms (default 120 = tap)
    port           UDP port (default 5002)

Examples:
  stream_touch_client.py 0.5 0.5        # quick tap at center
  stream_touch_client.py 0.25 0.8 2000  # press and hold 2 s
"""
import socket
import struct
import sys
import time


def send(x, y, pressed, port=5002, host="127.0.0.1"):
    pkt = struct.pack("<BBff", 0x54, 1 if pressed else 0, float(x), float(y))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(pkt, (host, port))
    s.close()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    x = float(sys.argv[1])
    y = float(sys.argv[2])
    dur = float(sys.argv[3]) / 1000.0 if len(sys.argv) > 3 else 0.12
    port = int(sys.argv[4]) if len(sys.argv) > 4 else 5002
    send(x, y, True, port)
    print(f"pressed at ({x:.3f}, {y:.3f}) on port {port}, holding {dur*1000:.0f} ms")
    time.sleep(dur)
    send(x, y, False, port)
    print("released")


if __name__ == "__main__":
    main()
