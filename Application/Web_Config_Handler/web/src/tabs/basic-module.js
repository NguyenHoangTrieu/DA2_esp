/**
 * basic-module.js — Basic mode dynamic module tab
 *
 * Shown for each detected LAN module stack.
 * Layout: JSON Config panel, Quick Controls, Connection section, Response area
 */

import { postConfig, loadJsonFile } from '../api.js';
import { toast } from '../main.js';
import { LAN_STACK_MAP, BLE_QUICK_COMMANDS, getDefaultConfig } from '../stack-data.js';

/**
 * @param {HTMLElement} container
 * @param {object} config - gateway config
 * @param {number} slot - 0 or 1
 * @param {string} stackId - e.g. '002'
 */
export function renderBasicModule(container, config, slot, stackId) {
  container.innerHTML = '';
  const info = LAN_STACK_MAP[stackId] || { type: 'NONE', label: 'Unknown' };
  const moduleType = info.type;
  const cmdPrefix = info.cmd_prefix || 'CFBL';

  let loadedJson = null;
  let loadedName = '(none)';

  container.innerHTML = `
    <div class="card">
      <div class="card-title">📤 JSON Config</div>
      <div id="bmJsonInfo" style="font-size:12px;color:var(--muted);margin-bottom:8px">No JSON loaded</div>
      <div class="btn-row" style="margin-top:0">
        <button class="btn" id="bmDefault">📋 Default</button>
        <button class="btn" id="bmCustom">📂 Custom</button>
        <button class="btn btn-set" id="bmSend" disabled>📤 Send</button>
      </div>
    </div>

    <div class="card">
      <div class="card-title">⚡ Quick Controls</div>
      <div class="quick-grid" id="bmQuickGrid"></div>
    </div>

    <div class="card">
      <div class="card-title">🔗 Connection</div>
      <div class="conn-row">
        <label>Connect:</label>
        <input type="text" id="bmConnAddr" placeholder="MAC or address" style="flex:1">
        <button class="btn" id="bmConnBtn">Send</button>
      </div>
      <div class="conn-row">
        <label>Disconnect:</label>
        <input type="text" id="bmDiscAddr" placeholder="MAC or address" style="flex:1">
        <button class="btn" id="bmDiscBtn">Send</button>
      </div>
    </div>

    <div class="card">
      <div class="card-title">📋 Response</div>
      <div class="response-area" id="bmResponse"></div>
      <div class="btn-row">
        <button class="btn" id="bmClear">🗑 Clear</button>
      </div>
    </div>
  `;

  const jsonInfo = container.querySelector('#bmJsonInfo');
  const sendBtn = container.querySelector('#bmSend');
  const respArea = container.querySelector('#bmResponse');

  function appendResp(text, cls = 'info') {
    const d = document.createElement('div');
    d.className = cls;
    d.textContent = text;
    respArea.appendChild(d);
    respArea.scrollTop = respArea.scrollHeight;
  }

  // Default JSON
  container.querySelector('#bmDefault').addEventListener('click', () => {
    const cfg = getDefaultConfig(moduleType, 0);
    if (cfg) {
      loadedJson = cfg;
      loadedName = `Default ${moduleType}`;
      jsonInfo.textContent = `Loaded: ${loadedName} (${JSON.stringify(cfg).length} bytes, ${cfg.functions?.length || 0} functions)`;
      sendBtn.disabled = false;
    }
  });

  // Custom JSON
  container.querySelector('#bmCustom').addEventListener('click', async () => {
    try {
      const data = await loadJsonFile();
      loadedJson = data;
      loadedName = 'Custom file';
      jsonInfo.textContent = `Loaded: ${loadedName} (${JSON.stringify(data).length} bytes)`;
      sendBtn.disabled = false;
    } catch (e) {
      toast('Load failed: ' + e.message, 'err');
    }
  });

  // Send JSON
  sendBtn.addEventListener('click', async () => {
    if (!loadedJson) return;
    try {
      const key = moduleType.toLowerCase() + '_json';
      const jsonStr = JSON.stringify(loadedJson);
      appendResp(`→ Sending ${moduleType} JSON config (${jsonStr.length} bytes)…`, 'info');
      await postConfig({ [key]: jsonStr, slot: String(slot) });
      appendResp(`✓ JSON config sent successfully`, 'ok');
      toast('JSON config sent');
    } catch (e) {
      appendResp(`✗ Send failed: ${e.message}`, 'error');
      toast('Send failed', 'err');
    }
  });

  // Quick Controls
  const quickGrid = container.querySelector('#bmQuickGrid');
  const commands = moduleType === 'BLE' ? BLE_QUICK_COMMANDS :
    [{ label: 'SW Reset', fn: 'MODULE_SW_RESET' }, { label: 'Get Info', fn: 'MODULE_GET_INFO' }];

  commands.forEach(cmd => {
    const btn = document.createElement('button');
    btn.className = 'btn';
    btn.textContent = cmd.label;
    btn.addEventListener('click', async () => {
      try {
        const key = moduleType.toLowerCase() + '_cmd';
        appendResp(`→ ${cmd.fn}`, 'info');
        await postConfig({ [key]: { slot: String(slot), function: cmd.fn } });
        appendResp(`✓ ${cmd.fn} sent`, 'ok');
      } catch (e) {
        appendResp(`✗ ${cmd.fn} failed: ${e.message}`, 'error');
      }
    });
    quickGrid.appendChild(btn);
  });

  // Connection
  container.querySelector('#bmConnBtn').addEventListener('click', async () => {
    const addr = container.querySelector('#bmConnAddr').value.trim();
    if (!addr) { toast('Enter an address', 'warn'); return; }
    try {
      appendResp(`→ Connect: ${addr}`, 'info');
      const key = moduleType.toLowerCase() + '_cmd';
      await postConfig({ [key]: { slot: String(slot), function: 'MODULE_CONNECT', param: addr } });
      appendResp(`✓ Connect command sent`, 'ok');
    } catch (e) {
      appendResp(`✗ Connect failed: ${e.message}`, 'error');
    }
  });

  container.querySelector('#bmDiscBtn').addEventListener('click', async () => {
    const addr = container.querySelector('#bmDiscAddr').value.trim();
    if (!addr) { toast('Enter an address', 'warn'); return; }
    try {
      appendResp(`→ Disconnect: ${addr}`, 'info');
      const key = moduleType.toLowerCase() + '_cmd';
      await postConfig({ [key]: { slot: String(slot), function: 'MODULE_DISCONNECT', param: addr } });
      appendResp(`✓ Disconnect command sent`, 'ok');
    } catch (e) {
      appendResp(`✗ Disconnect failed: ${e.message}`, 'error');
    }
  });

  // Clear
  container.querySelector('#bmClear').addEventListener('click', () => {
    respArea.innerHTML = '';
  });
}
