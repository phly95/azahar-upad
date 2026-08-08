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

The window has a settings bar: enter azahar's address, pick the screen
(bottom/top), resolution and bitrate, and press Connect. The receiver then
asks azahar's control server (TCP 5003) to stream exactly that, and learns
the video/touch ports from the reply - no emulator-side configuration
needed. Changing the values and pressing Connect again re-negotiates.

Usage:
    stream_receiver.py [--azahar-host 127.0.0.1] [--control-port 5003]
                       [--screen bottom|top] [--width 640] [--height 360]
                       [--bitrate 4000]

All options only set the initial GUI values; you can change everything from
the window. Press F11 (or the Fullscreen button) to toggle fullscreen; in
fullscreen the toolbar hides so the video fills the screen, and clicks still
map to the game screen. For scripted/legacy use without negotiation:
    stream_receiver.py --video-port 5000 --touch-host 127.0.0.1
                       --touch-port 5002 --screen bottom

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
    QComboBox,
    QLabel,
    QLineEdit,
    QMainWindow,
    QPushButton,
    QSpinBox,
    QToolBar,
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
    RESOLUTIONS = [
        "320x240", "400x240", "480x360", "640x360",
        "800x480", "960x540", "1280x720",
    ]

    def __init__(self, args):
        super().__init__()
        self.args = args
        self.control_sock = None
        self.bridge = FrameBridge()
        self.video = VideoWidget(self.bridge)
        self.video.screen_name = args.screen

        # Effective stream/touch settings (negotiated or legacy).
        self.video_port = args.video_port or 5000
        self.touch_port = args.touch_port or 5002
        self.touch_host = args.touch_host or args.azahar_host or "127.0.0.1"

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self.video)
        self.setCentralWidget(central)
        self.toolbar = self._build_toolbar()
        self.addToolBar(Qt.ToolBarArea.TopToolBarArea, self.toolbar)

        self.resize(640, 416)
        self.statusBar().showMessage("enter azahar address and press Connect")

        self.bridge.frame_ready.connect(self.video.set_frame)
        self.bridge.touch_status.connect(
            lambda s: self.statusBar().showMessage(f"touch {s}", 3000)
        )

        self.touch_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._frame_count = 0
        self._fps_timer = QTimer(self)
        self._fps_timer.timeout.connect(self._update_fps)
        self._fps_timer.start(1000)

        self._stop = threading.Event()
        self._thread = None
        self._update_title()

    # -- toolbar -----------------------------------------------------------

    def _build_toolbar(self):
        tb = QToolBar("Stream")
        tb.setMovable(False)
        tb.setFloatable(False)
        tb.setStyleSheet(
            "QToolBar { spacing: 4px; padding: 2px; }"
            "QToolBar QLabel { color: #ccc; }"
        )
        self.host_edit = QLineEdit(self.args.azahar_host or "127.0.0.1")
        self.host_edit.setPlaceholderText("azahar address")
        self.host_edit.setMaximumWidth(130)
        self.host_edit.setFixedHeight(24)
        tb.addWidget(QLabel("Azahar:"))
        tb.addWidget(self.host_edit)

        self.screen_combo = QComboBox()
        self.screen_combo.addItems(["bottom", "top"])
        self.screen_combo.setCurrentText(self.args.screen)
        self.screen_combo.setFixedHeight(24)
        tb.addWidget(QLabel("Screen:"))
        tb.addWidget(self.screen_combo)

        self.res_edit = QComboBox()
        self.res_edit.setEditable(True)
        self.res_edit.addItems(self.RESOLUTIONS)
        default_res = f"{self.args.width or 640}x{self.args.height or 360}"
        self.res_edit.setCurrentText(default_res)
        self.res_edit.lineEdit().setPlaceholderText("WxH, e.g. 640x360")
        self.res_edit.setFixedHeight(24)
        self.res_edit.setMaximumWidth(110)
        tb.addWidget(QLabel("Res:"))
        tb.addWidget(self.res_edit)

        self.bitrate_spin = QSpinBox()
        self.bitrate_spin.setRange(100, 100000)
        self.bitrate_spin.setSingleStep(100)
        self.bitrate_spin.setValue(self.args.bitrate or 4000)
        self.bitrate_spin.setSuffix(" k")
        self.bitrate_spin.setMaximumWidth(100)
        self.bitrate_spin.setFixedHeight(24)
        tb.addWidget(QLabel("Bitrate:"))
        tb.addWidget(self.bitrate_spin)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.on_connect)
        self.connect_btn.setFixedHeight(24)
        tb.addWidget(self.connect_btn)

        self.fullscreen_btn = QPushButton("Fullscreen")
        self.fullscreen_btn.setToolTip("Toggle fullscreen (F11)")
        self.fullscreen_btn.clicked.connect(self.toggle_fullscreen)
        self.fullscreen_btn.setFixedHeight(24)
        tb.addWidget(self.fullscreen_btn)

        tb.addSeparator()
        self.fps_label = QLabel("—")
        self.fps_label.setStyleSheet("color: #888;")
        tb.addWidget(self.fps_label)
        return tb

    def on_connect(self):
        """Negotiate (or re-negotiate) the stream with the current GUI values."""
        host = self.host_edit.text().strip()
        if not host:
            self.statusBar().showMessage("enter an azahar address first", 4000)
            return
        screen = self.screen_combo.currentText()
        resolution = self._parse_resolution(self.res_edit.currentText())
        if resolution is None:
            self.statusBar().showMessage(
                "invalid resolution (use WxH, e.g. 640x360, even values)", 5000
            )
            return
        w, h = resolution
        bitrate = self.bitrate_spin.value()

        self.connect_btn.setEnabled(False)
        self.statusBar().showMessage(f"connecting to {host}...")
        try:
            # Release the previous session first: azahar stops the old stream
            # and restores its config, then we ask for the new one.
            self._close_control()
            sock, resp = negotiate(host, self.args.control_port, screen, w, h, bitrate)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            self.statusBar().showMessage(f"connect failed: {exc}", 6000)
            self.connect_btn.setEnabled(True)
            return

        self.control_sock = sock
        self.video.screen_name = resp["screen"]
        self.video_port = int(resp["video_port"])
        self.touch_port = int(resp["touch_port"])
        self.touch_host = host
        self.restart_stream(self.video_port)
        self._update_title()
        self.statusBar().showMessage(
            f"connected: {resp['screen']} {resp['width']}x{resp['height']} "
            f"{resp['codec']} video udp:{resp['video_port']} "
            f"touch udp:{host}:{resp['touch_port']}",
            6000,
        )
        self.connect_btn.setEnabled(True)

    @staticmethod
    def _parse_resolution(text):
        """Parse 'WxH' free-form input; returns (w, h) or None."""
        t = text.strip().lower().replace(" ", "").replace("x", "x")
        if "x" not in t:
            return None
        parts = t.split("x")
        if len(parts) != 2:
            return None
        try:
            w, h = int(parts[0]), int(parts[1])
        except ValueError:
            return None
        if w < 64 or w > 3840 or h < 64 or h > 2160 or w % 2 or h % 2:
            return None
        return w, h

    def toggle_fullscreen(self):
        """Enter/leave fullscreen; hide the chrome so the video fills the screen."""
        if self.isFullScreen():
            self.showNormal()
            self.toolbar.setVisible(True)
            self.statusBar().setVisible(True)
            self._update_title()
        else:
            self.showFullScreen()
            self.toolbar.setVisible(False)
            self.statusBar().setVisible(False)
            self.setWindowTitle("Azahar Receiver — fullscreen (F11 to exit)")

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_F11:
            self.toggle_fullscreen()
        else:
            super().keyPressEvent(event)

    def _close_control(self):
        if self.control_sock is not None:
            try:
                self.control_sock.close()
            except OSError:
                pass
            self.control_sock = None

    def _update_title(self):
        self.setWindowTitle(
            f"Azahar Receiver — {self.video.screen_name} screen "
            f"(video udp:{self.video_port} → touch udp:{self.touch_host}:{self.touch_port})"
        )

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

    def _gst_loop(self, video_port):
        pipeline_str = (
            f'udpsrc port={video_port} '
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

    def restart_stream(self, video_port):
        """Stop any running pipeline and start one on the given UDP port."""
        self._stop.set()
        if self._thread is not None and self._thread.is_alive():
            self._thread.join(timeout=5)
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._gst_loop, args=(video_port,), daemon=True
        )
        self._thread.start()

    def _update_fps(self):
        self.fps_label.setText(f"{self._frame_count} fps")
        self._frame_count = 0

    # -- touch sender ------------------------------------------------------

    def send_touch(self, x: float, y: float, pressed: bool):
        pkt = struct.pack("<BBff", MAGIC, 1 if pressed else 0, float(x), float(y))
        self.touch_sock.sendto(pkt, (self.touch_host, self.args.touch_port))

    def closeEvent(self, event):
        self._stop.set()
        # Closing the control connection tells azahar to stop the negotiated
        # stream and restore its previous settings.
        self._close_control()
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


def negotiate(host, control_port, screen, width, height, bitrate):
    """Ask azahar's control server to stream the requested settings.

    Returns ``(sock, resp)``: the open control socket (kept open for the
    session) and the parsed JSON reply with the negotiated video/touch ports,
    screen and effective resolution. Raises OSError/ValueError on failure.
    """
    sock = socket.create_connection((host, control_port), timeout=5)
    req = {"version": 1, "screen": screen, "codec": "h264"}
    if width:
        req["width"] = width
    if height:
        req["height"] = height
    if bitrate:
        req["bitrate"] = bitrate
    sock.sendall((json.dumps(req) + "\n").encode())
    line = b""
    while not line.endswith(b"\n"):
        chunk = sock.recv(4096)
        if not chunk:
            break
        line += chunk
    resp = json.loads(line.decode())
    if not resp.get("ok"):
        raise ValueError(f"azahar rejected the request: {resp.get('error')}")
    return sock, resp


def main():
    args = parse_args()
    app = QApplication(sys.argv)
    win = ReceiverWindow(args)
    win.show()
    if args.azahar_host:
        # Negotiate with azahar right away (GUI values were pre-filled).
        win.on_connect()
    elif any(v is not None for v in (args.video_port, args.touch_port, args.touch_host)):
        # Legacy scripted mode: play the stream without negotiating.
        win.restart_stream(win.video_port)
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
