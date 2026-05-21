import serial
import re
from influxdb_client import InfluxDBClient, Point, WritePrecision
from influxdb_client.client.write_api import SYNCHRONOUS
from datetime import datetime, timezone

# --- Config ---
SERIAL_PORT = "COM3"       # Change to your actual port
BAUD_RATE = 115200
INFLUX_URL = "http://localhost:8086"
INFLUX_TOKEN = "rd0KRAFD_U49DEXyHtVr4hSoshABJB65JfuII5LjNuOM_DvflAp_G9lwBjPsRBpGTQf6pMdPNiPoLhwkHAfaNQ=="   # From InfluxDB UI
INFLUX_ORG = "smartlab"
INFLUX_BUCKET = "fallsensor"

STATE_NAMES = {
    0: "NORMAL",
    1: "FREEFALL",
    2: "IMPACT",
    3: "POST_IMPACT_STILLNESS",
    4: "FALL_CONFIRMED"
}

# --- InfluxDB setup ---
client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
write_api = client.write_api(write_options=SYNCHRONOUS)

# --- Serial setup ---
ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
print(f"Listening on {SERIAL_PORT} at {BAUD_RATE} baud...")

def write_sensor_data(timestamp_ms, accel_mag, gyro_mag, state):
    state_name = STATE_NAMES.get(state, "UNKNOWN")
    point = (
        Point("sensor_data")
        .field("accel_mag", accel_mag)
        .field("gyro_mag", gyro_mag)
        .field("state", state)
        .tag("state_name", state_name)
        .time(datetime.now(timezone.utc), WritePrecision.MS)
    )
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)

def write_fall_event(timestamp_ms):
    point = (
        Point("fall_events")
        .field("fall_detected", 1)
        .field("device_timestamp_ms", timestamp_ms)
        .time(datetime.now(timezone.utc), WritePrecision.MS)
    )
    write_api.write(bucket=INFLUX_BUCKET, org=INFLUX_ORG, record=point)
    print(f"[FALL EVENT WRITTEN] device_ts={timestamp_ms}ms")

try:
    while True:
        raw = ser.readline()
        try:
            line = raw.decode("utf-8").strip()
        except UnicodeDecodeError:
            continue

        if not line:
            continue

        # DATA line
        if line.startswith("DATA,"):
            parts = line.split(",")
            if len(parts) == 5:
                try:
                    ts  = int(parts[1])
                    acc = float(parts[2])
                    gyr = float(parts[3])
                    st  = int(parts[4]) if len(parts) > 4 else 0
                except ValueError:
                    continue
                write_sensor_data(ts, acc, gyr, st)
                print(f"[DATA] ts={ts}ms acc={acc} gyr={gyr} state={STATE_NAMES.get(st, st)}")

        # FALL event line
        elif line.startswith("FALL,"):
            parts = line.split(",")
            if len(parts) == 2:
                try:
                    ts = int(parts[1])
                    write_fall_event(ts)
                except ValueError:
                    continue

        # Log other stage messages for visibility
        elif any(kw in line for kw in ["STAGE", "ALERTING", "Orientation", "Accel variance"]):
            print(f"[EVENT] {line}")

except KeyboardInterrupt:
    print("Stopping bridge...")
finally:
    ser.close()
    client.close()