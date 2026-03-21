/**
 * api.js — Shared REST client for DA2 Gateway Config SPA
 *
 * Dev:  Vite proxies /api/* → http://localhost:3001 (mock_server)
 * Prod: /api/* → ESP32 httpd handlers (same origin)
 */

async function request(method, path, body) {
  const opts = { method, headers: { 'Content-Type': 'application/json' } };
  if (body !== undefined) opts.body = JSON.stringify(body);
  const res = await fetch(path, opts);
  if (!res.ok) {
    const text = await res.text().catch(() => '');
    throw new Error(`HTTP ${res.status}: ${text}`);
  }
  return res.json();
}

/** GET /api/config — full gateway config */
export const fetchConfig   = () => request('GET', '/api/config');

/** POST /api/config — partial config update */
export const postConfig    = body => request('POST', '/api/config', body);

/** GET /api/status — live status */
export const fetchStatus   = () => request('GET', '/api/status');

/** GET /api/lan_config — LAN module configs */
export const fetchLanConfig = () => request('GET', '/api/lan_config');

/** POST /api/lan_config — update a LAN module */
export const postLanConfig  = (type, data) => request('POST', '/api/lan_config', { type, data });

/** POST /api/reboot */
export const postReboot     = () => request('POST', '/api/reboot');

// ── File helpers ──────────────────────────────────────────────────────────────

/** Download an object as a .json file. */
export function saveJsonFile(obj, filename = 'gateway_config.json') {
  const blob = new Blob([JSON.stringify(obj, null, 2)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = filename;
  a.click();
  URL.revokeObjectURL(a.href);
}

/** Open file picker → parse JSON → return object (or throw). */
export function loadJsonFile() {
  return new Promise((resolve, reject) => {
    const input = document.getElementById('fileInput');
    const handler = () => {
      input.removeEventListener('change', handler);
      const file = input.files[0];
      if (!file) { reject(new Error('No file selected')); return; }
      const reader = new FileReader();
      reader.onload = () => {
        try { resolve(JSON.parse(reader.result)); }
        catch (e) { reject(new Error('Invalid JSON: ' + e.message)); }
      };
      reader.onerror = () => reject(new Error('File read error'));
      reader.readAsText(file);
      input.value = '';
    };
    input.addEventListener('change', handler);
    input.click();
  });
}
