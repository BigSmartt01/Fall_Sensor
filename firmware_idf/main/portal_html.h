#pragma once

const char portal_html[] = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>FallSensor Setup</title>
<style>
  * {
    margin: 0;
    padding: 0;
    box-sizing: border-box;
  }
  body {
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    min-height: 100vh;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 20px;
  }
  .card {
    background: white;
    border-radius: 24px;
    padding: 40px 30px;
    max-width: 400px;
    width: 100%;
    box-shadow: 0 25px 50px -12px rgba(0,0,0,0.25);
    text-align: center;
  }
  .icon {
    font-size: 64px;
    margin-bottom: 10px;
  }
  h1 {
    font-size: 26px;
    font-weight: 700;
    color: #2d3748;
    margin-bottom: 8px;
  }
  p {
    color: #718096;
    font-size: 15px;
    margin-bottom: 30px;
  }
  .input-group {
    margin-bottom: 20px;
    text-align: left;
  }
  label {
    display: block;
    font-size: 14px;
    font-weight: 600;
    color: #4a5568;
    margin-bottom: 6px;
  }
  input {
    width: 100%;
    padding: 14px 16px;
    font-size: 16px;
    border: 2px solid #e2e8f0;
    border-radius: 14px;
    outline: none;
    transition: border 0.2s, box-shadow 0.2s;
    background: #f7fafc;
  }
  input:focus {
    border-color: #667eea;
    box-shadow: 0 0 0 3px rgba(102,126,234,0.25);
    background: white;
  }
  button {
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    color: white;
    border: none;
    padding: 16px 24px;
    width: 100%;
    font-size: 18px;
    font-weight: 700;
    border-radius: 14px;
    cursor: pointer;
    transition: transform 0.1s, box-shadow 0.2s;
    margin-top: 10px;
  }
  button:hover {
    transform: translateY(-2px);
    box-shadow: 0 10px 20px -5px rgba(102,126,234,0.4);
  }
  button:active {
    transform: translateY(0);
  }
  .footer {
    margin-top: 24px;
    font-size: 12px;
    color: #a0aec0;
  }
</style>
</head>
<body>
  <div class="card">
    <div class="icon">🛡️</div>
    <h1>FallSensor Setup</h1>
    <p>Connect your sensor to Wi‑Fi to start monitoring.</p>
    <form action="/save" method="POST">
      <div class="input-group">
        <label for="ssid">Wi‑Fi Network (SSID)</label>
        <input type="text" id="ssid" name="ssid" placeholder="Your network name" required>
      </div>
      <div class="input-group">
        <label for="pass">Password</label>
        <input type="password" id="pass" name="pass" placeholder="Network password" required>
      </div>
      <button type="submit">Save &amp; Connect</button>
    </form>
    <div class="footer">
      Device will reboot after saving.
    </div>
  </div>
</body>
</html>
)rawliteral";