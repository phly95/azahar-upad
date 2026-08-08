#!/usr/bin/env python3
"""Azahar stream receiver with touch input.

Displays the emulator's streamed video (RTP H.264/UDP, the same stream
gst-launch could show) in a Qt window. Clicking / dragging inside the video
sends touch commands to the emulator's 3DS touch screen over UDP, so the
receiver acts as a remote touch screen (e.g. a phone or Steam Deck showing
the bottom screen).

Dependencies (Python 3):
    pip install PySide6 pygobject
and a GStreamer install with the rtph264depay / avdec_h264 plugins.

Usage (recommended — negotiate with the emulator):
    stream_receiver.py --azahar-host 127.0.0.1 [--screen bottom|top]
                       [--width 640] [--height 360] [--bitrate 4000]

Usage (legacy — read the emulator's config yourself):
    stream_receiver.py [--video-port 5000] [--touch-host 127.0.0.1]
                       [--touch-port 5002] [--screen bottom|top]

With --azahar-host, the receiver asks azahar's control server (TCP 5003)
which screen to stream, at what resolution/codec/bitrate, and learns the
video/touch ports from the reply. Without it, the legacy flags are used
and you must match the emulator's streaming configuration yourself.

Touch packet protocol (sent to touch-host:touch-port, 10 bytes, LE):
    [0]     magic 0x54 ('T')
    [1]     flags: bit 0 = pressed
    [2..5]  float x, normalized 0..1 of the visible game screen
    [6..9]  float y, normalized 0..1 of the visible game screen
"""
import argparse
import json
import queue
import socket
import struct
import sys
import threading
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

from PySide6.QtCore import Qt, Signal, QObject, QTimer  # noqa: E402
from PySide6.QtGui import QImage, QPixmap  # noqa: E402
from PySide6.QtWidgets import (  # noqa: E402
    QApplication,
    QLabel,
    QMainWindow,
    QVBoxLayout,
    QWidget,
)

Gst.init(None)

PACKET_SIZE = 10
MAGIC = 0x54


class FrameBridge(QObject):
    frame_ready = Signal(QImage)
    touch_status = Signal(str)


class VideoWidget(QLabel):
    """Displays the latest video frame and reports click/drag touches."""

    def __init__(self, bridge: FrameBridge):
        super().__init__()
        self.bridge = bridge
        self.video_size = (0, 0)
        self.setMinimumSize(320, 180)
        self.setMouseTracking(True)
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setStyleSheet("background-color: black;")
        self._frame = None

    def set_frame(self, image: QImage):
        self._frame = image
        self.video_size = (image.width(), image.height())
        self._update_pixmap()

    def _update_pixmap(self):
        if self._frame is None or self.video_size == (0, 0):
            return
        self.setPixmap(
            QPixmap.fromImage(self._frame).scaled(
                self.size(),
                Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
        )

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._update_pixmap()

    # -- touch mapping -----------------------------------------------------

    def _map_to_screen(self, pos) -> tuple[float, float] | None:
        """Map a widget click to normalized (x, y) of the visible game screen.

        Returns None if the click landed on a letterbox bar.
        """
        if self.video_size == (0, 0):
            return None
        vw, vh = self.video_size
        ww, wh = self.width(), self.height()
        if ww <= 0 or wh <= 0:
            return None
        scale = min(ww / vw, wh / vh)
        disp_w, disp_h = vw * scale, vh * scale
        off_x = (ww - disp_w) / 2.0
        off_y = (wh - disp_h) / 2.0
        vx = (pos.x() - off_x) / scale
        vy = (pos.y() - off_y) / scale
        if vx < 0 or vx >= vw or vy < 0 or vy >= vh:
            return None
        # The emulator letterboxes the game screen inside the video frame
        # (SingleFrameLayout: screen fills the height, is centered).
        aspect = 4.0 / 3.0 if self.screen_name == "bottom" else 5.0 / 3.0
        content_w = vh * aspect
        if content_w > vw:  # safety
            content_w = vw
        content_left = (vw - content_w) / 2.0
        if vx < content_left or vx >= content_left + content_w:
            return None
        nx = (vx - content_left) / content_w
        ny = vy / vh
        return max(0.0, min(1.0, nx)), max(0.0, min(1.0, ny))

    def _send_touch(self, pos, pressed: bool):
        mapped = self._map_to_screen(pos)
        if mapped is None:
            return
        nx, ny = mapped
        self.bridge.touch_status.emit(f"{'press ' if pressed else 'release'} ({nx:.3f}, {ny:.3f})")
        self.window().send_touch(nx, ny, pressed)

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._send_touch(event.position(), True)

    def mouseMoveEvent(self, event):
        if event.buttons() & Qt.MouseButton.LeftButton:
            self._send_touch(event.position(), True)

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._send_touch(event.position(), False)


class ReceiverWindow(QMainWindow):
    def __init__(self, args):
        super().__init__()
        self.args = args
        self.bridge = FrameBridge()
        self.video = VideoWidget(self.bridge)
        self.video.screen_name = args.screen

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.video)
        self.setCentralWidget(central)

        self.setWindowTitle(
            f"Azahar Receiver — {args.screen} screen "
            f"(video udp:{args.video_port} → touch udp:{args.touch_host}:{args.touch_port})"
        )
        self.resize(640, 380)
        self.statusBar().showMessage("waiting for stream...")

        self.bridge.frame_ready.connect(self.video.set_frame)
        self.bridge.touch_status.connect(
            lambda s: self.statusBar().showMessage(f"touch {s}", 3000)
        )

        self.touch_host = args.touch_host or args.azahar_host or "127.0.0.1"
        self.touch_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._frame_count = 0
        self._fps_timer = QTimer(self)
        self._fps_timer.timeout.connect(self._update_fps)
        self._fps_timer.start(1000)
        self._fps_start = time.monotonic()

        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._gst_loop, daemon=True)
        self._thread.start()

    # -- GStreamer ---------------------------------------------------------

    def _on_new_sample(self, sink):
        sample = sink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.OK
        buf = sample.get_buffer()
        caps = sample.get_caps()
        if caps is None or caps.get_size() == 0:
            return Gst.FlowReturn.OK
        structure = caps.get_structure(0)
        width = structure.get_int("width")[1]
        height = structure.get_int("height")[1]
        stride = structure.get_int("stride")[1] if structure.has_field("stride") else width * 3
        ok, mapinfo = buf.map(Gst.MapFlags.READ)
        if not ok:
            return Gst.FlowReturn.OK
        try:
            img = QImage(
                mapinfo.data, width, height, stride, QImage.Format.Format_RGB888
            ).copy()
        finally:
            buf.unmap(mapinfo)
        self._frame_count += 1
        self.bridge.frame_ready.emit(img)
        return Gst.FlowReturn.OK

    def _gst_loop(self):
        pipeline_str = (
            f'udpsrc port={self.args.video_port} '
            'caps="application/x-rtp,media=(string)video,clock-rate=(int)90000,'
            'encoding-name=(string)H264,payload=(int)96" '
            "! rtph264depay ! h264parse ! avdec_h264 "
            "! videoconvert ! video/x-raw,format=RGB "
            "! appsink name=sink emit-signals=true max-buffers=1 drop=true"
        )
        try:
            pipeline = Gst.parse_launch(pipeline_str)
        except Exception as exc:  # GLib.Error
            self.bridge.touch_status.emit(f"pipeline error: {exc}")
            return
        sink = pipeline.get_by_name("sink")
        sink.connect("new-sample", self._on_new_sample)
        pipeline.set_state(Gst.State.PLAYING)
        while not self._stop.is_set():
            time.sleep(0.1)
        pipeline.set_state(Gst.State.NULL)

    def _update_fps(self):
        self.statusBar().showMessage(
            f"receiving… {self._frame_count} fps", 1000
        )
        self._frame_count = 0

    # -- touch sender ------------------------------------------------------

    def send_touch(self, x: float, y: float, pressed: bool):
        pkt = struct.pack("<BBff", MAGIC, 1 if pressed else 0, float(x), float(y))
        self.touch_sock.sendto(pkt, (self.touch_host, self.args.touch_port))

    def closeEvent(self, event):
        self._stop.set()
        # Closing the control connection tells azahar to stop the negotiated
        # stream and restore its previous settings.
        if getattr(self, "control_sock", None) is not None:
            try:
                self.control_sock.close()
            except OSError:
                pass
        event.accept()


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--azahar-host", default=None,
                    help="Azahar IP/hostname. If set, negotiate the stream with "
                         "azahar's control server instead of using fixed flags")
    ap.add_argument("--control-port", type=int, default=5003,
                    help="Azahar control server TCP port (with --azahar-host)")
    ap.add_argument("--video-port", type=int, default=5000,
                    help="UDP port of the RTP video stream (legacy mode)")
    ap.add_argument("--touch-host", default=None,
                    help="Emulator IP for touch input (defaults to --azahar-host)")
    ap.add_argument("--touch-port", type=int, default=5002,
                    help="Emulator UDP port for touch input")
    ap.add_argument("--screen", choices=["bottom", "top"], default="bottom",
                    help="Screen to stream (requested in negotiation mode; "
                         "assumed in legacy mode)")
    ap.add_argument("--width", type=int, default=None,
                    help="Requested stream width (negotiation mode, optional)")
    ap.add_argument("--height", type=int, default=None,
                    help="Requested stream height (negotiation mode, optional)")
    ap.add_argument("--bitrate", type=int, default=None,
                    help="Requested bitrate in kbps (negotiation mode, optional)")
    return ap.parse_args()


def negotiate(args):
    """Negotiate the stream with azahar's control server.

    Sends the receiver's request (screen / resolution / codec / bitrate),
    applies the negotiated video port, touch port and screen to ``args``,
    and returns the open control socket, kept open for the session. Returns
    None when --azahar-host is not given (legacy mode).
    """
    if not args.azahar_host:
        return None
    try:
        sock = socket.create_connection((args.azahar_host, args.control_port), timeout=5)
    except OSError as exc:
        print(f"error: cannot reach azahar control server at "
              f"{args.azahar_host}:{args.control_port}: {exc}", file=sys.stderr)
        sys.exit(1)
    req = {"version": 1, "screen": args.screen, "codec": "h264"}
    if args.width:
        req["width"] = args.width
    if args.height:
        req["height"] = args.height
    if args.bitrate:
        req["bitrate"] = args.bitrate
    sock.sendall((json.dumps(req) + "\n").encode())
    line = b""
    try:
        while not line.endswith(b"\n"):
            chunk = sock.recv(4096)
            if not chunk:
                break
            line += chunk
        resp = json.loads(line.decode())
    except (OSError, ValueError) as exc:
        print(f"error: bad response from azahar control server: {exc}", file=sys.stderr)
        sock.close()
        sys.exit(1)
    if not resp.get("ok"):
        print(f"error: azahar rejected the request: {resp.get('error')}", file=sys.stderr)
        sock.close()
        sys.exit(1)
    args.video_port = int(resp["video_port"])
    args.touch_port = int(resp["touch_port"])
    args.screen = resp["screen"]
    if args.touch_host is None:
        args.touch_host = args.azahar_host
    print(f"negotiated with azahar: {resp['screen']} screen "
          f"{resp['width']}x{resp['height']} {resp['codec']} "
          f"video udp:{resp['video_port']} -> touch udp:{args.touch_host}:{resp['touch_port']}")
    return sock


def main():
    args = parse_args()
    control_sock = negotiate(args)
    app = QApplication(sys.argv)
    win = ReceiverWindow(args)
    win.control_sock = control_sock
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
