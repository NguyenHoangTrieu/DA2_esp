// web/mock_server.js
// Run with: node mock_server.js
// Simulates the ESP32 REST API on localhost:3001 — no MCU needed for UI development.

import express from 'express';
const app = express();
app.use(express.json());

// ── In-memory state ───────────────────────────────────────────────────────────

let state = {
  wifi: {
    ssid: 'TestNetwork',
    password: 'secret123',
    auth_mode: 'PERSONAL',
    username: '',
  },
  wan: {
    stack_wan_id: '101',
  },
  lte: {
    modem: 'A7600C1',
    comm_type: 'USB',
    apn: 'v-internet',
    username: '',
    password: '',
    auto_reconnect: true,
    timeout: 30000,
    max_retry: 0,
    pwr_pin: 'WK',
    rst_pin: 'PE',
  },
  lan: {
    stack1_id: '002',
    stack2_id: '003',
  },
  server: {
    type: 0,
    mqtt: {
      broker: 'mqtt://demo.thingsboard.io:1883',
      token: '',
      sub_topic: 'v1/devices/me/rpc/request/+',
      pub_topic: 'v1/devices/me/telemetry',
      attr_topic: 'v1/devices/me/attributes',
    },
    http: {
      url: '',
      auth_token: '',
      port: 8080,
      timeout: 10000,
      use_tls: false,
      verify: false,
    },
    coap: {
      host: 'demo.thingsboard.io',
      resource: '/api/v1/{token}/telemetry',
      token: '',
      port: 5683,
      ack_timeout: 2000,
      max_rtx: 4,
      use_dtls: false,
    },
  },
};

let startTime = Date.now();

// ── GET /api/config ─────────────────────────────────────────────────────────

app.get('/api/config', (_req, res) => {
  console.log('[GET] /api/config');
  res.json(state);
});

// ── POST /api/config ────────────────────────────────────────────────────────

app.post('/api/config', (req, res) => {
  const body = req.body;
  console.log('[POST] /api/config', JSON.stringify(body, null, 2));

  // Merge known sections
  for (const key of ['wifi', 'lte', 'wan', 'lan', 'server']) {
    if (body[key]) {
      if (typeof state[key] === 'object') Object.assign(state[key], body[key]);
      else state[key] = body[key];
    }
  }

  // Handle BLE/LoRa/Zigbee JSON and CMD proxying
  for (const prefix of ['ble', 'lora', 'zigbee']) {
    if (body[prefix + '_json']) {
      console.log(`  → LAN ${prefix.toUpperCase()} JSON config (${body[prefix + '_json'].length} bytes)`);
    }
    if (body[prefix + '_cmd']) {
      console.log(`  → LAN ${prefix.toUpperCase()} CMD:`, body[prefix + '_cmd']);
    }
  }

  // Handle OTA
  if (body.wan_fota) console.log(`  → WAN OTA: ${body.wan_fota}`);
  if (body.lan_fota) console.log(`  → LAN OTA: ${body.lan_fota}`);

  res.json({ ok: true });
});

// ── GET /api/lan_config ─────────────────────────────────────────────────────

app.get('/api/lan_config', (_req, res) => {
  console.log('[GET] /api/lan_config');
  res.json({ ok: true, data: {} });
});

// ── POST /api/lan_config ────────────────────────────────────────────────────

app.post('/api/lan_config', (req, res) => {
  console.log('[POST] /api/lan_config', JSON.stringify(req.body, null, 2));
  res.json({ ok: true });
});

// ── GET /api/status ─────────────────────────────────────────────────────────

app.get('/api/status', (_req, res) => {
  const uptime = Math.floor((Date.now() - startTime) / 1000);
  res.json({
    firmware_version: '1.0.0-mock',
    wifi_connected: true,
    wifi_ssid: state.wifi.ssid,
    wifi_rssi: -62,
    ip_address: '192.168.1.200',
    internet_ok: true,
    server_ok: true,
    free_heap: 198420,
    uptime_s: uptime,
  });
});

// ── POST /api/reboot ────────────────────────────────────────────────────────

app.post('/api/reboot', (_req, res) => {
  console.log('[POST] /api/reboot — simulating reboot');
  startTime = Date.now();
  res.json({ ok: true, message: 'Rebooting in 500ms' });
});

app.listen(3001, () => {
  console.log('Mock API server running at http://localhost:3001');
  console.log('Start Vite dev server: npm run dev');
});
