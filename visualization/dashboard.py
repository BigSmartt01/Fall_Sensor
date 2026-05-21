import sys
import serial
import threading
import time
from collections import deque
from datetime import datetime, timezone

import numpy as np
import pyqtgraph as pg
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout,
    QHBoxLayout, QLabel, QFrame, QLCDNumber
)
from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QFont, QColor

from influxdb_client import InfluxDBClient, Point, WritePrecision
from influxdb_client.client.write_api import WriteOptions

# ─── CONFIG ───────────────────────────────────────────────────────────────────
SERIAL_PORT   = "COM3"
BAUD_RATE     = 115200
INFLUX_URL    = "http://localhost:8086"
INFLUX_TOKEN  = "rd0KRAFD_U49DEXyHtVr4hSoshABJB65JfuII5LjNuOM_DvflAp_G9lwBjPsRBpGTQf6pMdPNiPoLhwkHAfaNQ=="
INFLUX_ORG    = "smartlab"
INFLUX_BUCKET = "fallsensor"

WINDOW_SIZE   = 300       # number of samples visible on graph (~60s at 200ms)
UI_REFRESH_MS = 20       # graph refresh rate in ms

STATE_NAMES = {
    0: "NORMAL",
    1: "FREEFALL",
    2: "IMPACT",
    3: "POST-IMPACT STILLNESS",
    4: "FALL CONFIRMED"
}

STATE_COLORS = {
    0: "#00FF88",   # green
    1: "#FFD700",   # yellow
    2: "#FF6B35",   # orange
    3: "#FF8C00",   # dark orange
    4: "#FF0000"    # red
}

# ─── INFLUXDB SETUP ───────────────────────────────────────────────────────────
try:
    influx_client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
    write_api = influx_client.write_api(write_options=WriteOptions(
        batch_size=20,
        flush_interval=500,
        jitter_interval=0,
        retry_interval=1000
    ))
    influx_ok = True
    print("[InfluxDB] Connected.")
except Exception as e:
    influx_ok = False
    print(f"[InfluxDB] Not available: {e}. Logging disabled.")

# ─── SHARED DATA ──────────────────────────────────────────────────────────────
accel_buf  = deque([0.0] * WINDOW_SIZE, maxlen=WINDOW_SIZE)
gyro_buf   = deque([0.0] * WINDOW_SIZE, maxlen=WINDOW_SIZE)
time_buf   = deque([0]   * WINDOW_SIZE, maxlen=WINDOW_SIZE)
fall_markers = []       # list of x-indices where falls occurred

latest = {
    "accel": 0.0,
    "gyro":  0.0,
    "state": 0,
    "fall":  False,
    "fall_ts": None
}
data_lock = threading.Lock()

# ─── SERIAL READER THREAD ─────────────────────────────────────────────────────
def serial_reader():
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
        print(f"[Serial] Opened {SERIAL_PORT}")
    except Exception as e:
        print(f"[Serial] Failed to open {SERIAL_PORT}: {e}")
        return

    sample_index = 0

    while True:
        try:
            raw = ser.readline()
            line = raw.decode("utf-8", errors="ignore").strip()

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
                        time_buf.append(ts)
                        latest["accel"] = acc
                        latest["gyro"]  = gyr
                        latest["state"] = st
                        sample_index += 1

                    if influx_ok:
                        state_name = STATE_NAMES.get(st, "UNKNOWN")
                        point = (
                            Point("sensor_data")
                            .field("accel_mag", acc)
                            .field("gyro_mag", gyr)
                            .field("state", st)
                            .tag("state_name", state_name)
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
                        fall_markers.append(sample_index)

                    if influx_ok:
                        point = (
                            Point("fall_events")
                            .field("fall_detected", 1)
                            .field("device_timestamp_ms", ts)
                            .time(datetime.now(timezone.utc), WritePrecision.MS)
                        )
                        write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
                    print(f"[FALL DETECTED] device_ts={ts}ms")

        except Exception as e:
            print(f"[Serial] Read error: {e}")
            time.sleep(0.1)


# ─── MAIN WINDOW ──────────────────────────────────────────────────────────────
class FallSensorDashboard(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Fall Sensor Monitor")
        self.setMinimumSize(1100, 700)
        self._sample_count = 0
        self._fall_lines = []

        pg.setConfigOption("background", "#0D0D0D")
        pg.setConfigOption("foreground", "#CCCCCC")

        self._build_ui()

        self.timer = QTimer()
        self.timer.timeout.connect(self._update)
        self.timer.start(UI_REFRESH_MS)

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setSpacing(8)
        root.setContentsMargins(12, 12, 12, 12)

        # ── Title bar
        title_row = QHBoxLayout()
        title = QLabel("FALL SENSOR MONITOR")
        title.setFont(QFont("Courier New", 16, QFont.Weight.Bold))
        title.setStyleSheet("color: #00FF88; letter-spacing: 4px;")
        title_row.addWidget(title)
        title_row.addStretch()

        self.clock_label = QLabel("")
        self.clock_label.setFont(QFont("Courier New", 11))
        self.clock_label.setStyleSheet("color: #555555;")
        title_row.addWidget(self.clock_label)
        root.addLayout(title_row)

        # ── Stat bar
        stat_row = QHBoxLayout()
        stat_row.setSpacing(12)

        self.accel_card = self._stat_card("ACCEL MAG", "m/s²", "#00FF88")
        self.gyro_card  = self._stat_card("GYRO MAG",  "°/s",  "#00BFFF")
        self.state_card = self._state_card()
        self.fall_card  = self._fall_card()

        stat_row.addWidget(self.accel_card["frame"])
        stat_row.addWidget(self.gyro_card["frame"])
        stat_row.addWidget(self.state_card["frame"])
        stat_row.addWidget(self.fall_card["frame"])
        root.addLayout(stat_row)

        # ── Graphs
        self.plot_widget = pg.GraphicsLayoutWidget()
        self.plot_widget.setStyleSheet("border: 1px solid #1A1A1A;")
        root.addWidget(self.plot_widget)

        # Accel plot
        self.accel_plot = self.plot_widget.addPlot(row=0, col=0)
        self._style_plot(self.accel_plot, "Acceleration Magnitude", "m/s²", "#00FF88")
        self.accel_curve = self.accel_plot.plot(pen=pg.mkPen("#00FF88", width=2))

        # Threshold lines on accel plot
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

        # Gyro plot
        self.gyro_plot = self.plot_widget.addPlot(row=1, col=0)
        self._style_plot(self.gyro_plot, "Gyro Magnitude", "°/s", "#00BFFF")
        self.gyro_curve = self.gyro_plot.plot(pen=pg.mkPen("#00BFFF", width=2))

        self.gyro_plot.addItem(pg.InfiniteLine(
            pos=25.0, angle=0,
            pen=pg.mkPen("#FF8C00", width=1, style=Qt.PenStyle.DashLine),
            label="Stillness 25.0", labelOpts={"color": "#FF8C00", "position": 0.95}
        ))

        # Link x axes
        self.gyro_plot.setXLink(self.accel_plot)

        # ── Status bar
        self.status_label = QLabel("Waiting for data...")
        self.status_label.setFont(QFont("Courier New", 9))
        self.status_label.setStyleSheet("color: #444444; padding: 2px;")
        root.addWidget(self.status_label)

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
        layout = QVBoxLayout(frame)
        layout.setSpacing(2)

        lbl = QLabel(label)
        lbl.setFont(QFont("Courier New", 8))
        lbl.setStyleSheet(f"color: {color}99; letter-spacing: 2px;")

        val = QLabel("---")
        val.setFont(QFont("Courier New", 22, QFont.Weight.Bold))
        val.setStyleSheet(f"color: {color};")

        unit_lbl = QLabel(unit)
        unit_lbl.setFont(QFont("Courier New", 8))
        unit_lbl.setStyleSheet("color: #444444;")

        layout.addWidget(lbl)
        layout.addWidget(val)
        layout.addWidget(unit_lbl)

        return {"frame": frame, "val": val}

    def _state_card(self):
        frame = QFrame()
        frame.setStyleSheet("""
            QFrame {
                background: #111111;
                border: 1px solid #333333;
                border-radius: 6px;
                padding: 8px;
            }
        """)
        layout = QVBoxLayout(frame)
        layout.setSpacing(2)

        lbl = QLabel("STATE")
        lbl.setFont(QFont("Courier New", 8))
        lbl.setStyleSheet("color: #555555; letter-spacing: 2px;")

        val = QLabel("NORMAL")
        val.setFont(QFont("Courier New", 14, QFont.Weight.Bold))
        val.setStyleSheet("color: #00FF88;")

        layout.addWidget(lbl)
        layout.addWidget(val)

        return {"frame": frame, "val": val}

    def _fall_card(self):
        frame = QFrame()
        frame.setStyleSheet("""
            QFrame {
                background: #111111;
                border: 1px solid #333333;
                border-radius: 6px;
                padding: 8px;
            }
        """)
        layout = QVBoxLayout(frame)
        layout.setSpacing(2)

        lbl = QLabel("LAST FALL")
        lbl.setFont(QFont("Courier New", 8))
        lbl.setStyleSheet("color: #555555; letter-spacing: 2px;")

        val = QLabel("none")
        val.setFont(QFont("Courier New", 11))
        val.setStyleSheet("color: #FF0000;")

        count = QLabel("Total: 0")
        count.setFont(QFont("Courier New", 8))
        count.setStyleSheet("color: #444444;")

        layout.addWidget(lbl)
        layout.addWidget(val)
        layout.addWidget(count)

        return {"frame": frame, "val": val, "count": count}

    def _style_plot(self, plot, title, ylabel, color):
        plot.setTitle(title, color=color, size="10pt")
        plot.setLabel("left", ylabel, color="#666666")
        plot.showGrid(x=True, y=True, alpha=0.15)
        plot.getAxis("bottom").setStyle(tickFont=QFont("Courier New", 8))
        plot.getAxis("left").setStyle(tickFont=QFont("Courier New", 8))

    def _update(self):
        with data_lock:
            accel_data = list(accel_buf)
            gyro_data  = list(gyro_buf)
            acc   = latest["accel"]
            gyr   = latest["gyro"]
            state = latest["state"]
            fell  = latest["fall"]
            fall_ts = latest["fall_ts"]
            markers = list(fall_markers)

            if fell:
                latest["fall"] = False

        x = np.arange(len(accel_data))

        self.accel_curve.setData(x, accel_data)
        self.gyro_curve.setData(x, gyro_data)

        # Update stat cards
        self.accel_card["val"].setText(f"{acc:.2f}")
        self.gyro_card["val"].setText(f"{gyr:.2f}")

        state_name  = STATE_NAMES.get(state, "UNKNOWN")
        state_color = STATE_COLORS.get(state, "#CCCCCC")
        self.state_card["val"].setText(state_name)
        self.state_card["val"].setStyleSheet(f"color: {state_color};")

        # Fall markers on both plots
        if fell and fall_ts is not None:
            ts_str = datetime.now().strftime("%H:%M:%S")
            self.fall_card["val"].setText(ts_str)
            self.fall_card["val"].setStyleSheet("color: #FF0000;")
            self.fall_card["count"].setText(f"Total: {len(markers)}")

            for plot in [self.accel_plot, self.gyro_plot]:
                line = pg.InfiniteLine(
                    pos=WINDOW_SIZE - 1,
                    angle=90,
                    pen=pg.mkPen("#FF0000", width=2, style=Qt.PenStyle.DashLine),
                    label="FALL", labelOpts={"color": "#FF0000", "position": 0.95}
                )
                plot.addItem(line)
                self._fall_lines.append(line)

        self.clock_label.setText(datetime.now().strftime("%H:%M:%S"))
        self.status_label.setText(
            f"acc={acc:.2f} m/s²  |  gyro={gyr:.2f} °/s  |  state={state_name}  |  "
            f"influxdb={'ok' if influx_ok else 'offline'}  |  "
            f"falls={len(fall_markers)}"
        )

        self._sample_count += 1


# ─── ENTRY POINT ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    reader_thread = threading.Thread(target=serial_reader, daemon=True)
    reader_thread.start()

    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    window = FallSensorDashboard()
    window.show()

    sys.exit(app.exec())
