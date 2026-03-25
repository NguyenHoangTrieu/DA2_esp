/**
 * ble_native.js — BLE Native tab (GATT Central + BLE Mesh Provisioner)
 *
 * Native ESP32 BLE handlers — no slot, no AT module, no stack config.
 * Shows form fields; JSON is built and POSTed on click (no preview panel).
 *
 * CFBG: JSON  → postLanConfig('BLE_GATT', payload)
 * CFBN: JSON  → postLanConfig('BLE_MESH', payload)
 */

import { postLanConfig } from '../api.js';
import { toast }         from '../main.js';

// ── Utilities ─────────────────────────────────────────────────────────────────

function esc(v) { return String(v ?? '').replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;'); }
function q(id)  { return document.getElementById(id); }

// ── GATT Central helpers ──────────────────────────────────────────────────────

function buildGattPayload() {
  return {
    ble_gatt: {
      scan: {
        interval: parseInt(q('bn-g-interval').value, 10),
        window:   parseInt(q('bn-g-window').value,   10),
        active:   q('bn-g-active').checked,
      },
      connection: {
        interval_min:        parseInt(q('bn-g-imin').value,    10),
        interval_max:        parseInt(q('bn-g-imax').value,    10),
        latency:             parseInt(q('bn-g-latency').value, 10),
        supervision_timeout: parseInt(q('bn-g-timeout').value, 10),
      },
    },
  };
}

// ── BLE Mesh helpers ──────────────────────────────────────────────────────────

let _cmdRows = [];   // [{name, model_id, opcode, param_schema} row elements]

function buildMeshPayload() {
  const commands = _cmdRows
    .filter(r => r && r.closest('body'))   // skip removed rows
    .map(r => ({
      name:         r.querySelector('[data-f="name"]').value.trim(),
      model_id:     r.querySelector('[data-f="model_id"]').value.trim(),
      opcode:       r.querySelector('[data-f="opcode"]').value.trim(),
      param_schema: r.querySelector('[data-f="param_schema"]').value.trim(),
    }))
    .filter(c => c.name);

  return {
    ble_native: {
      mesh: {
        provisioner_name:     q('bn-m-prov').value.trim(),
        net_key:              q('bn-m-netkey').value.trim(),
        app_key:              q('bn-m-appkey').value.trim(),
        primary_unicast_addr: parseInt(q('bn-m-unicast').value, 10),
        ttl:                  parseInt(q('bn-m-ttl').value,     10),
      },
      commands,
    },
  };
}

function addCmdRow(container, defaults = {}) {
  const row = document.createElement('div');
  row.className = 'bn-cmd-row';
  row.innerHTML = `
    <input data-f="name"         value="${esc(defaults.name         ?? '')}"  placeholder="ONOFF"        style="width:110px">
    <input data-f="model_id"     value="${esc(defaults.model_id     ?? '0x1000')}" placeholder="0x1000" style="width:80px">
    <input data-f="opcode"       value="${esc(defaults.opcode       ?? '0x8202')}" placeholder="0x8202" style="width:80px">
    <input data-f="param_schema" value="${esc(defaults.param_schema ?? '')}"  placeholder="key:type"     style="width:160px">
    <button class="btn btn-sm btn-del" title="Remove row">✕</button>
  `;
  row.querySelector('.btn-del').addEventListener('click', () => {
    row.remove();
    _cmdRows = _cmdRows.filter(r => r !== row);
  });
  container.appendChild(row);
  _cmdRows.push(row);
}

// ── Render ────────────────────────────────────────────────────────────────────

export function renderBleNative(container, _config) {
  _cmdRows = [];
  container.innerHTML = `
    <!-- ── Sub-tab bar ─────────────────────────────────────────────────────── -->
    <div class="bn-tabbar">
      <button class="bn-tab active" data-sub="gatt">🔷 GATT Central (CFBG:)</button>
      <button class="bn-tab"        data-sub="mesh">🔶 BLE Mesh (CFBN:)</button>
    </div>

    <!-- ════════════════════════════════════════════════════════════════════════
         GATT Central pane
    ═══════════════════════════════════════════════════════════════════════════ -->
    <div id="bn-pane-gatt" class="bn-pane">
      <p class="hint">Native ESP32 GATT Central — no slot &nbsp;|&nbsp; CFBG:JSON:<em>&lt;json&gt;</em></p>

      <div class="card">
        <div class="card-title">Scan Parameters</div>
        <div class="form-grid">
          <div class="form-group">
            <label>Interval <span class="hint">(×0.625 ms)</span></label>
            <input type="number" id="bn-g-interval" value="160" min="4" max="16384">
          </div>
          <div class="form-group">
            <label>Window <span class="hint">(×0.625 ms)</span></label>
            <input type="number" id="bn-g-window" value="80" min="4" max="16384">
          </div>
          <div class="form-group" style="align-self:end">
            <label class="checkbox-row">
              <input type="checkbox" id="bn-g-active" checked> Active Scan
            </label>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-title">Connection Parameters</div>
        <div class="form-grid">
          <div class="form-group">
            <label>Interval Min <span class="hint">(×1.25 ms)</span></label>
            <input type="number" id="bn-g-imin" value="16" min="6" max="3200">
          </div>
          <div class="form-group">
            <label>Interval Max <span class="hint">(×1.25 ms)</span></label>
            <input type="number" id="bn-g-imax" value="32" min="6" max="3200">
          </div>
          <div class="form-group">
            <label>Slave Latency</label>
            <input type="number" id="bn-g-latency" value="0" min="0" max="500">
          </div>
          <div class="form-group">
            <label>Supervision Timeout <span class="hint">(×10 ms)</span></label>
            <input type="number" id="bn-g-timeout" value="500" min="10" max="3200">
          </div>
        </div>
      </div>

      <div class="btn-row">
        <button class="btn btn-set" id="bn-g-send">✅ Send GATT Config</button>
      </div>
    </div>

    <!-- ════════════════════════════════════════════════════════════════════════
         BLE Mesh pane
    ═══════════════════════════════════════════════════════════════════════════ -->
    <div id="bn-pane-mesh" class="bn-pane hidden">
      <p class="hint">Native ESP32 BLE Mesh Provisioner — no slot &nbsp;|&nbsp; CFBN:JSON:<em>&lt;json&gt;</em></p>

      <div class="card">
        <div class="card-title">Mesh Keys &amp; Settings</div>
        <div class="form-grid">
          <div class="form-group">
            <label>Provisioner Name</label>
            <input type="text" id="bn-m-prov" value="DA2_GW" maxlength="32">
          </div>
          <div class="form-group" style="grid-column: span 2">
            <label>Network Key <span class="hint">(32 hex chars)</span></label>
            <input type="text" id="bn-m-netkey" value="A1B2C3D4E5F6A7B8C9DAEBFCAD1E2F30"
                   maxlength="32" style="font-family:var(--mono)">
          </div>
          <div class="form-group" style="grid-column: span 2">
            <label>App Key <span class="hint">(32 hex chars)</span></label>
            <input type="text" id="bn-m-appkey" value="0102030405060708090A0B0C0D0E0F10"
                   maxlength="32" style="font-family:var(--mono)">
          </div>
          <div class="form-group">
            <label>Primary Unicast Addr</label>
            <input type="number" id="bn-m-unicast" value="1" min="1" max="32767">
          </div>
          <div class="form-group">
            <label>TTL</label>
            <input type="number" id="bn-m-ttl" value="7" min="1" max="127">
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-title">Control Commands</div>
        <div class="bn-cmd-header">
          <span style="width:110px">Name</span>
          <span style="width:80px">Model ID</span>
          <span style="width:80px">Opcode</span>
          <span style="width:160px">Param Schema</span>
        </div>
        <div id="bn-cmd-list"></div>
        <button class="btn btn-sm" id="bn-m-addrow" style="margin-top:8px">+ Add Row</button>
      </div>

      <div class="btn-row">
        <button class="btn btn-set" id="bn-m-send">✅ Send Mesh Config</button>
      </div>
    </div>
  `;

  // ── Sub-tab switching ────────────────────────────────────────────────────
  container.querySelectorAll('.bn-tab').forEach(btn => {
    btn.addEventListener('click', () => {
      container.querySelectorAll('.bn-tab').forEach(b => b.classList.remove('active'));
      container.querySelectorAll('.bn-pane').forEach(p => p.classList.add('hidden'));
      btn.classList.add('active');
      q(`bn-pane-${btn.dataset.sub}`).classList.remove('hidden');
    });
  });

  // ── Populate default mesh command rows ───────────────────────────────────
  const cmdList = q('bn-cmd-list');
  [
    { name: 'ONOFF',     model_id: '0x1000', opcode: '0x8202', param_schema: 'value:uint8'        },
    { name: 'LIGHTNESS', model_id: '0x1300', opcode: '0x824C', param_schema: 'lightness:uint16'   },
    { name: 'CTL',       model_id: '0x1303', opcode: '0x8265', param_schema: 'lightness:uint16,temperature:uint16,delta_uv:int16' },
  ].forEach(d => addCmdRow(cmdList, d));

  q('bn-m-addrow').addEventListener('click', () => addCmdRow(cmdList));

  // ── Send: GATT ───────────────────────────────────────────────────────────
  q('bn-g-send').addEventListener('click', async () => {
    try {
      await postLanConfig('BLE_GATT', buildGattPayload());
      toast('GATT Central config sent ✓');
    } catch (e) {
      toast('GATT send failed: ' + e.message, 'err');
    }
  });

  // ── Send: Mesh ───────────────────────────────────────────────────────────
  q('bn-m-send').addEventListener('click', async () => {
    try {
      await postLanConfig('BLE_MESH', buildMeshPayload());
      toast('BLE Mesh config sent ✓');
    } catch (e) {
      toast('Mesh send failed: ' + e.message, 'err');
    }
  });
}
