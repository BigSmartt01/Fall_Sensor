#pragma once
#include <Arduino.h>

const char dashboard_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Fall Sensor</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.1/dist/chart.umd.min.js"></script>
  <style>
    body { font-family: system-ui, sans-serif; background:#0d1117; color:#0f0; margin:0; padding:15px; text-align:center; }
    h1 { color:#58a6ff; margin:10px 0 5px; }
    #status { font-size:1.9em; padding:18px; margin:12px; border-radius:12px; background:#21262d; min-height:1.3em; }
    #alert { font-size:2.6em; color:#ff5555; display:none; animation: pulse 1s infinite; font-weight:bold; }
    canvas { max-width:100%; margin:20px auto; background:#010409; border-radius:10px; }
    @keyframes pulse { 0%,100% {opacity:1;} 50% {opacity:0.3;} }
  </style>
</head>
<body>
  <h1>🔴 Fall Sensor Live</h1>
  <div id="status">Connecting to device...</div>
  <div id="alert">🚨 FALL DETECTED — GET HELP IMMEDIATELY! 🚨</div>
  <canvas id="chart" height="240"></canvas>

  <script>
    const ws = new WebSocket(`ws://${location.hostname}:81`);
    let points = [];
    let chart;

    ws.onopen = () => document.getElementById("status").textContent = "✅ Connected";
    
    ws.onmessage = e => {
      const line = e.data.trim();
      if (line.startsWith("DATA,")) {
        const p = line.split(",");
        if (p.length < 5) return;

        const states = ["NORMAL","FREEFALL","IMPACT","POST-STILL","CONFIRMED","ALERTED"];
        const state = parseInt(p[4]);
        const accel = parseFloat(p[2]);

        document.getElementById("status").innerHTML = `<strong>${states[state] || "UNKNOWN"}</strong>`;

        if (state >= 4) document.getElementById("alert").style.display = "block";
        else document.getElementById("alert").style.display = "none";

        points.push(accel);
        if (points.length > 150) points.shift();

        if (!chart) {
          chart = new Chart(document.getElementById("chart"), {
            type: 'line',
            data: { labels: points.map((_,i)=>""), datasets: [{ label: 'Accel (m/s²)', data: points, borderColor: '#39ff14', tension: 0.4 }] },
            options: { animation: { duration: 0 }, scales: { y: { beginAtZero: true }}}
          });
        } else {
          chart.data.datasets[0].data = points;
          chart.update('none');
        }
      } else if (line.startsWith("FALL,")) {
        document.getElementById("alert").style.display = "block";
      }
    };

    ws.onclose = () => setTimeout(() => location.reload(), 2000);
  </script>
</body>
</html>
)rawliteral";