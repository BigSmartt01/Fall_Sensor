#pragma once
#include <Arduino.h>

const char dashboard_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
  <title>Fall Sensor Live</title>
  <style>
    * { margin:0; padding:0; box-sizing:border-box; }
    body {
      background: #0d1117; color: #c9d1d9; font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
      display: flex; flex-direction: column; align-items: center; min-height: 100vh; padding: 12px;
    }
    .container { max-width: 900px; width: 100%; }
    h1 { color: #00FF88; font-size: 1.8em; text-align: center; margin: 8px 0 4px; letter-spacing: 2px; }
    .status-row { display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; margin: 8px 0; }
    .connection { font-size: 0.9em; padding: 4px 12px; border-radius: 12px; background: #21262d; }
    .connection.ok { color: #00FF88; }
    .connection.error { color: #ff5555; }
    .stat-cards { display: flex; gap: 10px; flex-wrap: wrap; margin: 10px 0; }
    .card {
      flex: 1 1 150px; background: #161b22; border: 1px solid #30363d; border-radius: 12px;
      padding: 12px; text-align: center; min-width: 120px;
    }
    .card .label { font-size: 0.7em; color: #8b949e; text-transform: uppercase; letter-spacing: 1px; }
    .card .value { font-size: 1.6em; font-weight: 700; margin: 6px 0; }
    .card .unit { font-size: 0.7em; color: #484f58; }
    .state-badge { display: inline-block; padding: 4px 14px; border-radius: 20px; font-weight: bold; font-size: 0.9em; }
    .fall-alert {
      display: none; background: #ff5555; color: white; text-align: center; padding: 14px;
      border-radius: 12px; font-size: 1.2em; font-weight: bold; margin: 10px 0;
      animation: pulse 1.5s infinite;
    }
    .fall-alert.show { display: block; }
    @keyframes pulse { 0%,100%{opacity:1;} 50%{opacity:0.4;} }
    .chart-container { background: #010409; border: 1px solid #30363d; border-radius: 12px; padding: 8px; margin: 10px 0; }
    canvas { width: 100%; height: auto; display: block; }
    .legend { display: flex; gap: 20px; justify-content: center; margin: 6px 0; font-size: 0.8em; }
    .legend span { display: flex; align-items: center; gap: 4px; }
    .legend .color { width: 16px; height: 4px; border-radius: 2px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>🛡️ Fall Sensor Live</h1>
    <div class="status-row">
      <span id="connection" class="connection error">⚫ Disconnected</span>
      <span id="clock" style="color:#8b949e;"></span>
    </div>

    <div class="stat-cards">
      <div class="card">
        <div class="label">Accel Mag</div>
        <div class="value" id="accelVal" style="color:#00FF88;">---</div>
        <div class="unit">m/s²</div>
      </div>
      <div class="card">
        <div class="label">Gyro Mag</div>
        <div class="value" id="gyroVal" style="color:#00BFFF;">---</div>
        <div class="unit">°/s</div>
      </div>
      <div class="card">
        <div class="label">State</div>
        <div class="value"><span id="stateBadge" class="state-badge" style="background:#00FF8833; color:#00FF88;">NORMAL</span></div>
      </div>
      <div class="card">
        <div class="label">Total Falls</div>
        <div class="value" id="totalFalls" style="color:#ff5555;">0</div>
      </div>
    </div>

    <div id="fallAlert" class="fall-alert">🚨 FALL DETECTED — GET HELP IMMEDIATELY! 🚨</div>

    <div class="chart-container">
      <canvas id="chartCanvas" width="800" height="300"></canvas>
    </div>
    <div class="legend">
      <span><span class="color" style="background:#00FF88;"></span> Accel (m/s²)</span>
      <span><span class="color" style="background:#00BFFF;"></span> Gyro (°/s)</span>
      <span style="color:#ff5555;">━ Fall Marker</span>
    </div>
  </div>

  <script>
    (function() {
      const WS_PORT = 81;
      const MAX_POINTS = 150;
      let ws;
      let accelData = [];
      let gyroData = [];
      let fallMarkers = [];   // relative positions (0..MAX_POINTS-1) where falls occurred
      let state = 0;
      let totalFalls = 0;
      let reconnectTimer = null;

      const states = ["NORMAL","FREEFALL","IMPACT","POST-STILL","CONFIRMED","ALERTED"];
      const stateColors = ["#00FF88","#FFD700","#FF6B35","#FF8C00","#FF0000","#FF4444"];

      // UI elements
      const connectionEl = document.getElementById('connection');
      const clockEl = document.getElementById('clock');
      const accelValEl = document.getElementById('accelVal');
      const gyroValEl = document.getElementById('gyroVal');
      const stateBadge = document.getElementById('stateBadge');
      const totalFallsEl = document.getElementById('totalFalls');
      const fallAlertEl = document.getElementById('fallAlert');
      const canvas = document.getElementById('chartCanvas');
      const ctx = canvas.getContext('2d');

      // Chart thresholds
      const ACCEL_THRESHOLDS = [3.0, 20.0];
      const GYRO_THRESHOLD = 25.0;

      function connectWebSocket() {
        if (ws) { ws.close(); }
        ws = new WebSocket(`ws://${location.hostname}:${WS_PORT}`);
        ws.onopen = () => {
          connectionEl.textContent = '🟢 Connected';
          connectionEl.className = 'connection ok';
          if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
        };
        ws.onclose = () => {
          connectionEl.textContent = '🔴 Disconnected';
          connectionEl.className = 'connection error';
          // Reconnect after 2s
          if (!reconnectTimer) reconnectTimer = setTimeout(connectWebSocket, 2000);
        };
        ws.onerror = () => {
          ws.close();
        };
        ws.onmessage = (e) => {
          const line = e.data.trim();
          if (line.startsWith('DATA,')) {
            const parts = line.split(',');
            if (parts.length < 5) return;
            const accel = parseFloat(parts[2]);
            const gyro = parseFloat(parts[3]);
            state = parseInt(parts[4]);

            // Update data buffers
            accelData.push(accel);
            gyroData.push(gyro);
            if (accelData.length > MAX_POINTS) {
              accelData.shift();
              gyroData.shift();
              // Adjust fall marker positions (they shift left)
              fallMarkers = fallMarkers.filter(m => m > 0).map(m => m - 1);
            }

            // Update UI values
            accelValEl.textContent = accel.toFixed(2);
            gyroValEl.textContent = gyro.toFixed(2);

            // State badge
            const stateName = states[state] || 'UNKNOWN';
            const color = stateColors[state] || '#CCCCCC';
            stateBadge.textContent = stateName;
            stateBadge.style.background = color + '33';
            stateBadge.style.color = color;

            // Show/hide fall alert based on state
            if (state >= 4) {
              fallAlertEl.classList.add('show');
            } else {
              fallAlertEl.classList.remove('show');
            }
          }
          else if (line.startsWith('FALL,')) {
            totalFalls++;
            totalFallsEl.textContent = totalFalls;
            // Add fall marker at the most recent data point (if any)
            if (accelData.length > 0) {
              fallMarkers.push(accelData.length - 1);
            }
            // Flash alert even if state later clears quickly
            fallAlertEl.classList.add('show');
            setTimeout(() => {
              if (state < 4) fallAlertEl.classList.remove('show');
            }, 5000);
          }
        };
      }

      // Draw chart
      function drawChart() {
        const W = canvas.width;
        const H = canvas.height;
        ctx.clearRect(0, 0, W, H);

        // Background
        ctx.fillStyle = '#010409';
        ctx.fillRect(0, 0, W, H);

        // Grid lines
        ctx.strokeStyle = '#1c2128';
        ctx.lineWidth = 0.5;
        // horizontal grid lines (5 divisions)
        for (let i = 1; i < 5; i++) {
          const y = (H / 5) * i;
          ctx.beginPath();
          ctx.moveTo(0, y);
          ctx.lineTo(W, y);
          ctx.stroke();
        }
        // vertical grid lines (every 25 points)
        for (let i = 25; i < MAX_POINTS; i += 25) {
          const x = (i / MAX_POINTS) * W;
          ctx.beginPath();
          ctx.moveTo(x, 0);
          ctx.lineTo(x, H);
          ctx.stroke();
        }

        // Draw threshold lines (accel)
        const accelRange = 25; // max Y for accel (0-25)
        const gyroRange = 50;  // max Y for gyro (0-50)
        ctx.save();
        ctx.strokeStyle = '#FFD70088';
        ctx.lineWidth = 1;
        ctx.setLineDash([4, 4]);
        // Freefall threshold (3.0)
        let y = H - (3.0 / accelRange * H);
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
        // Impact threshold (20.0)
        y = H - (20.0 / accelRange * H);
        ctx.strokeStyle = '#FF6B3588';
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
        // Gyro stillness threshold (25.0)
        y = H - (25.0 / gyroRange * H);
        ctx.strokeStyle = '#FF8C0088';
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
        ctx.restore();

        // Draw data lines
        if (accelData.length < 2) return;

        // Helper to plot a line
        function plotLine(data, color, yRange) {
          ctx.beginPath();
          ctx.strokeStyle = color;
          ctx.lineWidth = 2;
          const len = data.length;
          for (let i = 0; i < len; i++) {
            const x = (i / (MAX_POINTS - 1)) * W;
            const y = H - (data[i] / yRange * H);
            if (i === 0) ctx.moveTo(x, y);
            else ctx.lineTo(x, y);
          }
          ctx.stroke();
        }

        plotLine(accelData, '#00FF88', accelRange);
        plotLine(gyroData, '#00BFFF', gyroRange);

        // Draw fall markers
        ctx.strokeStyle = '#ff5555';
        ctx.lineWidth = 2;
        ctx.setLineDash([3, 3]);
        for (const relPos of fallMarkers) {
          if (relPos >= 0 && relPos < MAX_POINTS) {
            const x = (relPos / (MAX_POINTS - 1)) * W;
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, H);
            ctx.stroke();
          }
        }
        ctx.setLineDash([]); // reset
      }

      // Animation loop for canvas
      function animationLoop() {
        drawChart();
        // Update clock
        const now = new Date();
        clockEl.textContent = now.toLocaleTimeString();
        requestAnimationFrame(animationLoop);
      }

      // Start everything
      connectWebSocket();
      animationLoop();

      // Handle page unload
      window.addEventListener('beforeunload', () => {
        if (ws) ws.close();
      });
    })();
  </script>
</body>
</html>
)rawliteral";