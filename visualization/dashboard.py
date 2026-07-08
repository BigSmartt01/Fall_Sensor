import sys
import serial
import socket
import threading
import time
from collections import deque
from datetime import datetime, timezone

import numpy as np
import pyqtgraph as pg
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout,
    QHBoxLayout, QLabel, QFrame, QPushButton
)
from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QFont

from influxdb_client import InfluxDBClient, Point, WritePrecision
from influxdb_client.client.write_api import WriteOptions

# ─── CONFIG ───────────────────────────────────────────────────────────────────
CONNECTION_MODE = "wifi"   # "serial" or "wifi" - switch this to change source

SERIAL_PORT   = "COM7"
BAUD_RATE     = 115200

WIFI_HOST     = "192.168.101.204"   # ESP32-C3 IP, printed on boot under [WiFi] 10.164.56.82 | 192.168.101.204
WIFI_PORT     = 3333

INFLUX_URL    = "http://localhost:8086"
INFLUX_TOKEN  = "rd0KRAFD_U49DEXyHtVr4hSoshABJB65JfuII5LjNuOM_DvflAp_G9lwBjPsRBpGTQf6pMdPNiPoLhwkHAfaNQ=="
INFLUX_ORG    = "smartlab"
INFLUX_BUCKET = "fallsensor"

WINDOW_SIZE   = 300
UI_REFRESH_MS = 20

STATE_NAMES = {
    0: "NORMAL",
    1: "FREEFALL",
    2: "IMPACT",
    3: "POST-IMPACT STILLNESS",
    4: "FALL CONFIRMED",
    5: "FALL ALERTED"
}

STATE_COLORS = {
    0: "#00FF88",
    1: "#FFD700",
    2: "#FF6B35",
    3: "#FF8C00",
    4: "#FF0000",
    5: "#FF4444"
}

# ─── INFLUXDB ─────────────────────────────────────────────────────────────────
try:
    influx_client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    write_api = influx_client.write_api(write_options=WriteOptions(
        batch_size=20, flush_interval=500, jitter_interval=0, retry_interval=1000
    ))
    influx_ok = True
    print("[InfluxDB] Connected.")
except Exception as e:
    influx_ok = False
    print(f"[InfluxDB] Not available: {e}. Logging disabled.")

# ─── SHARED DATA ──────────────────────────────────────────────────────────────
accel_buf           = deque([0.0] * WINDOW_SIZE, maxlen=WINDOW_SIZE)
gyro_buf            = deque([0.0] * WINDOW_SIZE, maxlen=WINDOW_SIZE)
fall_markers        = []        # absolute sample indices of confirmed falls
global_sample_index = 0         # increments on every DATA line received

latest = {
    "accel":   0.0,
    "gyro":    0.0,
    "state":   0,
    "fall":    False,
    "fall_ts": None
}
data_lock = threading.Lock()


# ─── SHARED LINE PARSER ───────────────────────────────────────────────────────
# Used by both serial_reader() and wifi_reader() so DATA/FALL handling
# never drifts out of sync between the two connection modes.
def process_line(line):
    global global_sample_index

    if line.startswith("DATA,"):
        parts = line.split(",")
        if len(parts) == 5:
            ts  = int(parts[1])
            acc = float(parts[2])
            gyr = float(parts[3])
            st  = int(parts[4])
            with data_lock:
                accel_buf.append(acc)
                gyro_buf.append(gyr)
                latest["accel"] = acc
                latest["gyro"]  = gyr
                latest["state"] = st
                global_sample_index += 1
            if influx_ok:
                point = (
                    Point("sensor_data")
                    .field("accel_mag", acc)
                    .field("gyro_mag",  gyr)
                    .field("state",     st)
                    .tag("state_name",  STATE_NAMES.get(st, "UNKNOWN"))
                    .time(datetime.now(timezone.utc), WritePrecision.MS)
                )
                write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)

    elif line.startswith("FALL,"):
        parts = line.split(",")
        if len(parts) == 2:
            ts = int(parts[1])
            with data_lock:
                latest["fall"]    = True
                latest["fall_ts"] = ts
                fall_markers.append(global_sample_index)
            if influx_ok:
                point = (
                    Point("fall_events")
                    .field("fall_detected",       1)
                    .field("device_timestamp_ms", ts)
                    .time(datetime.now(timezone.utc), WritePrecision.MS)
                )
                write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
            print(f"[FALL DETECTED] device_ts={ts}ms")


# ─── SERIAL READER ────────────────────────────────────────────────────────────
def serial_reader():
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        print(f"[Serial] Opened {SERIAL_PORT}")
    except Exception as e:
        print(f"[Serial] Failed to open {SERIAL_PORT}: {e}")
        return

    while True:
        try:
            raw  = ser.readline()
            line = raw.decode("utf-8", errors="ignore").strip()
            if line:
                process_line(line)
        except Exception as e:
            print(f"[Serial] Read error: {e}")
            time.sleep(0.1)


# ─── WIFI READER ──────────────────────────────────────────────────────────────
# Connects to the ESP32-C3's TCP server (see wifi_stream.cpp) and reads the
# exact same DATA/FALL line format as Serial - just over a socket instead.
def wifi_reader():
    while True:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(10)
            print(f"[WiFi] Connecting to {WIFI_HOST}:{WIFI_PORT}...")
            sock.connect((WIFI_HOST, WIFI_PORT))
            sock.settimeout(None)
            print(f"[WiFi] Connected to {WIFI_HOST}:{WIFI_PORT}")

            buffer = ""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    print("[WiFi] Connection closed by device")
                    break

                buffer += chunk.decode("utf-8", errors="ignore")
                while "\n" in buffer:
                    line, buffer = buffer.split("\n", 1)
                    line = line.strip()
                    if line:
                        process_line(line)

        except Exception as e:
            print(f"[WiFi] Connection error: {e} - retrying in 2s")
            try:
                sock.close()
            except Exception:
                pass
            time.sleep(2)


# ─── DASHBOARD ────────────────────────────────────────────────────────────────
class FallSensorDashboard(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Fall Sensor Monitor")
        self.setMinimumSize(1100, 700)

        self._paused      = False
        self._frozen_accel = None
        self._frozen_gyro  = None
        self._frozen_index = 0

        # each entry: (accel_InfiniteLine, gyro_InfiniteLine, abs_sample_index)
        self._fall_line_items = []

        pg.setConfigOption("background", "#0D0D0D")
        pg.setConfigOption("foreground", "#CCCCCC")

        self._build_ui()

        self.timer = QTimer()
        self.timer.timeout.connect(self._update)
        self.timer.start(UI_REFRESH_MS)

    # ── UI construction ───────────────────────────────────────────────────────
    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setSpacing(8)
        root.setContentsMargins(12, 12, 12, 12)

        # Title bar
        title_row = QHBoxLayout()
        title = QLabel("FALL SENSOR MONITOR")
        title.setFont(QFont("Courier New", 16, QFont.Weight.Bold))
        title.setStyleSheet("color: #00FF88; letter-spacing: 4px;")
        title_row.addWidget(title)
        title_row.addStretch()

        self.pause_btn = QPushButton("PAUSE")
        self.pause_btn.setFont(QFont("Courier New", 9, QFont.Weight.Bold))
        self.pause_btn.setFixedWidth(110)
        self._style_pause_btn(paused=False)
        self.pause_btn.clicked.connect(self._toggle_pause)
        title_row.addWidget(self.pause_btn)

        self.clock_label = QLabel("")
        self.clock_label.setFont(QFont("Courier New", 11))
        self.clock_label.setStyleSheet("color: #555555; margin-left: 12px;")
        title_row.addWidget(self.clock_label)

        mode_text  = f"WiFi {WIFI_HOST}:{WIFI_PORT}" if CONNECTION_MODE == "wifi" else f"Serial {SERIAL_PORT}"
        mode_color = "#00BFFF" if CONNECTION_MODE == "wifi" else "#FFD700"
        self.mode_label = QLabel(mode_text)
        self.mode_label.setFont(QFont("Courier New", 9))
        self.mode_label.setStyleSheet(f"color: {mode_color}; margin-left: 12px;")
        title_row.addWidget(self.mode_label)
        root.addLayout(title_row)

        # Stat cards
        stat_row = QHBoxLayout()
        stat_row.setSpacing(12)
        self.accel_card = self._stat_card("ACCEL MAG", "m/s²", "#00FF88")
        self.gyro_card  = self._stat_card("GYRO MAG",  "°/s",  "#00BFFF")
        self.state_card = self._state_card()
        self.fall_card  = self._fall_card()
        for c in [self.accel_card, self.gyro_card, self.state_card, self.fall_card]:
            stat_row.addWidget(c["frame"])
        root.addLayout(stat_row)

        # Plots
        self.plot_widget = pg.GraphicsLayoutWidget()
        self.plot_widget.setStyleSheet("border: 1px solid #1A1A1A;")
        root.addWidget(self.plot_widget)

        self.accel_plot = self.plot_widget.addPlot(row=0, col=0)
        self._style_plot(self.accel_plot, "Acceleration Magnitude", "m/s²", "#00FF88")
        self.accel_curve = self.accel_plot.plot(pen=pg.mkPen("#00FF88", width=2))
        self.accel_plot.addItem(pg.InfiniteLine(
            pos=3.0, angle=0,
            pen=pg.mkPen("#FFD700", width=1, style=Qt.PenStyle.DashLine),
            label="Freefall 3.0", labelOpts={"color": "#FFD700", "position": 0.95}
        ))
        self.accel_plot.addItem(pg.InfiniteLine(
            pos=20.0, angle=0,
            pen=pg.mkPen("#FF6B35", width=1, style=Qt.PenStyle.DashLine),
            label="Impact 20.0", labelOpts={"color": "#FF6B35", "position": 0.85}
        ))

        self.gyro_plot = self.plot_widget.addPlot(row=1, col=0)
        self._style_plot(self.gyro_plot, "Gyro Magnitude", "°/s", "#00BFFF")
        self.gyro_curve = self.gyro_plot.plot(pen=pg.mkPen("#00BFFF", width=2))
        self.gyro_plot.addItem(pg.InfiniteLine(
            pos=25.0, angle=0,
            pen=pg.mkPen("#FF8C00", width=1, style=Qt.PenStyle.DashLine),
            label="Stillness 25.0", labelOpts={"color": "#FF8C00", "position": 0.95}
        ))
        self.gyro_plot.setXLink(self.accel_plot)

        # Lock x range - lines can never push or compress the view
        self.accel_plot.setXRange(0, WINDOW_SIZE, padding=0)
        self.accel_plot.setLimits(xMin=0, xMax=WINDOW_SIZE)
        self.gyro_plot.setXRange(0, WINDOW_SIZE, padding=0)
        self.gyro_plot.setLimits(xMin=0, xMax=WINDOW_SIZE)

        # Status bar
        self.status_label = QLabel("Waiting for data...")
        self.status_label.setFont(QFont("Courier New", 9))
        self.status_label.setStyleSheet("color: #444444; padding: 2px;")
        root.addWidget(self.status_label)

    # ── Pause toggle ──────────────────────────────────────────────────────────
    def _toggle_pause(self):
        self._paused = not self._paused
        if self._paused:
            with data_lock:
                self._frozen_accel = list(accel_buf)
                self._frozen_gyro  = list(gyro_buf)
                self._frozen_index = global_sample_index
        else:
            self._frozen_accel = None
            self._frozen_gyro  = None
        self._style_pause_btn(self._paused)

    def _style_pause_btn(self, paused):
        color = "#00FF88" if paused else "#FFD700"
        border = "#00FF8866" if paused else "#FFD70066"
        label  = "RESUME" if paused else "PAUSE"
        self.pause_btn.setText(label)
        self.pause_btn.setStyleSheet(f"""
            QPushButton {{
                background: #1A1A1A;
                color: {color};
                border: 1px solid {border};
                border-radius: 4px;
                padding: 4px 8px;
            }}
            QPushButton:hover {{ background: #252525; }}
            QPushButton:pressed {{ background: #333333; }}
        """)

    # ── Card builders ─────────────────────────────────────────────────────────
    def _stat_card(self, label, unit, color):
        frame = QFrame()
        frame.setStyleSheet(f"""
            QFrame {{
                background: #111111;
                border: 1px solid {color}33;
                border-radius: 6px;
                padding: 8px;
            }}
        """)
        lay = QVBoxLayout(frame)
        lay.setSpacing(2)
        lbl = QLabel(label)
        lbl.setFont(QFont("Courier New", 8))
        lbl.setStyleSheet(f"color: {color}99; letter-spacing: 2px;")
        val = QLabel("---")
        val.setFont(QFont("Courier New", 22, QFont.Weight.Bold))
        val.setStyleSheet(f"color: {color};")
        unit_lbl = QLabel(unit)
        unit_lbl.setFont(QFont("Courier New", 8))
        unit_lbl.setStyleSheet("color: #444444;")
        lay.addWidget(lbl); lay.addWidget(val); lay.addWidget(unit_lbl)
        return {"frame": frame, "val": val}

    def _state_card(self):
        frame = QFrame()
        frame.setStyleSheet("""
            QFrame { background: #111111; border: 1px solid #333333;
                     border-radius: 6px; padding: 8px; }
        """)
        lay = QVBoxLayout(frame); lay.setSpacing(2)
        lbl = QLabel("STATE")
        lbl.setFont(QFont("Courier New", 8))
        lbl.setStyleSheet("color: #555555; letter-spacing: 2px;")
        val = QLabel("NORMAL")
        val.setFont(QFont("Courier New", 14, QFont.Weight.Bold))
        val.setStyleSheet("color: #00FF88;")
        lay.addWidget(lbl); lay.addWidget(val)
        return {"frame": frame, "val": val}

    def _fall_card(self):
        frame = QFrame()
        frame.setStyleSheet("""
            QFrame { background: #111111; border: 1px solid #33000088;
                     border-radius: 6px; padding: 8px; }
        """)
        lay = QVBoxLayout(frame); lay.setSpacing(2)
        lbl = QLabel("LAST FALL")
        lbl.setFont(QFont("Courier New", 8))
        lbl.setStyleSheet("color: #555555; letter-spacing: 2px;")
        val = QLabel("none")
        val.setFont(QFont("Courier New", 11))
        val.setStyleSheet("color: #FF4444;")
        count = QLabel("Total: 0")
        count.setFont(QFont("Courier New", 8))
        count.setStyleSheet("color: #444444;")
        lay.addWidget(lbl); lay.addWidget(val); lay.addWidget(count)
        return {"frame": frame, "val": val, "count": count}

    def _style_plot(self, plot, title, ylabel, color):
        plot.setTitle(title, color=color, size="10pt")
        plot.setLabel("left", ylabel, color="#666666")
        plot.showGrid(x=True, y=True, alpha=0.15)
        plot.getAxis("bottom").setStyle(tickFont=QFont("Courier New", 8))
        plot.getAxis("left").setStyle(tickFont=QFont("Courier New", 8))

    # ── Main update loop ──────────────────────────────────────────────────────
    def _update(self):
        with data_lock:
            acc      = latest["accel"]
            gyr      = latest["gyro"]
            state    = latest["state"]
            fell     = latest["fall"]
            fall_ts  = latest["fall_ts"]
            markers  = list(fall_markers)
            cur_idx  = global_sample_index
            if fell:
                latest["fall"] = False

        # Decide which buffer to render
        if self._paused and self._frozen_accel is not None:
            accel_data = self._frozen_accel
            gyro_data  = self._frozen_gyro
            ref_idx    = self._frozen_index
        else:
            with data_lock:
                accel_data = list(accel_buf)
                gyro_data  = list(gyro_buf)
            ref_idx = cur_idx

        x = np.arange(len(accel_data))

        if not self._paused:
            self.accel_curve.setData(x, accel_data)
            self.gyro_curve.setData(x, gyro_data)

        # ── Reposition fall marker lines as the window scrolls ───────────────
        # window_start is the absolute index corresponding to x=0 on screen
        window_start = ref_idx - WINDOW_SIZE
        still_visible = []
        for (a_line, g_line, abs_idx) in self._fall_line_items:
            rel_pos = abs_idx - window_start
            if 0 <= rel_pos <= WINDOW_SIZE:
                # still on screen - update position
                a_line.setValue(rel_pos)
                g_line.setValue(rel_pos)
                still_visible.append((a_line, g_line, abs_idx))
            else:
                # scrolled off screen - remove from both plots cleanly
                self.accel_plot.removeItem(a_line)
                self.gyro_plot.removeItem(g_line)
        self._fall_line_items = still_visible

        # ── Draw a new fall line if a fall just fired ─────────────────────────
        if fell and not self._paused and markers:
            abs_fall_idx = markers[-1]
            rel_pos      = abs_fall_idx - window_start

            a_line = pg.InfiniteLine(
                pos=rel_pos, angle=90,
                pen=pg.mkPen("#FF0000", width=2, style=Qt.PenStyle.DashLine),
                label="FALL", labelOpts={"color": "#FF0000", "position": 0.9}
            )
            g_line = pg.InfiniteLine(
                pos=rel_pos, angle=90,
                pen=pg.mkPen("#FF0000", width=2, style=Qt.PenStyle.DashLine),
            )
            self.accel_plot.addItem(a_line)
            self.gyro_plot.addItem(g_line)
            self._fall_line_items.append((a_line, g_line, abs_fall_idx))

            ts_str = datetime.now().strftime("%H:%M:%S")
            self.fall_card["val"].setText(ts_str)
            self.fall_card["count"].setText(f"Total: {len(markers)}")

        # ── Stat cards (always live, even when paused) ────────────────────────
        self.accel_card["val"].setText(f"{acc:.2f}")
        self.gyro_card["val"].setText(f"{gyr:.2f}")

        state_name  = STATE_NAMES.get(state, "UNKNOWN")
        state_color = STATE_COLORS.get(state, "#CCCCCC")
        self.state_card["val"].setText(state_name)
        self.state_card["val"].setStyleSheet(f"color: {state_color};")

        self.clock_label.setText(datetime.now().strftime("%H:%M:%S"))

        pause_tag = "  [PAUSED - scroll/zoom freely]" if self._paused else ""
        self.status_label.setText(
            f"acc={acc:.2f} m/s²  |  gyro={gyr:.2f} °/s  |  state={state_name}  |  "
            f"influxdb={'ok' if influx_ok else 'offline'}  |  "
            f"falls={len(markers)}{pause_tag}"
        )


# ─── ENTRY POINT ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    if CONNECTION_MODE == "wifi":
        reader_thread = threading.Thread(target=wifi_reader, daemon=True)
        print(f"[Mode] WiFi - target {WIFI_HOST}:{WIFI_PORT}")
    else:
        reader_thread = threading.Thread(target=serial_reader, daemon=True)
        print(f"[Mode] Serial - port {SERIAL_PORT}")

    reader_thread.start()

    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    window = FallSensorDashboard()
    window.show()

    sys.exit(app.exec())