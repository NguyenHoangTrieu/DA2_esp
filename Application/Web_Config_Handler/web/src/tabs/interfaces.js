/**
 * interfaces.js — Basic mode Interfaces tab
 *
 * Read-only informational tab showing which module stacks were detected
 * on LAN slots, based on GET /api/config response.
 */

import { LAN_STACK_MAP, WAN_STACK_MAP } from '../stack-data.js';

export function renderInterfaces(container, config) {
  container.innerHTML = '';
  const lan = config?.lan || {};
  const wan = config?.wan || {};

  const s1 = lan.stack1_id || '000';
  const s2 = lan.stack2_id || '000';
  const wId = wan.stack_wan_id || '100';

  const s1Info = LAN_STACK_MAP[s1] || { type: 'NONE', label: 'Unknown' };
  const s2Info = LAN_STACK_MAP[s2] || { type: 'NONE', label: 'Unknown' };
  const wInfo  = WAN_STACK_MAP[wId] || { type: 'NONE', label: 'Unknown' };

  container.innerHTML = `
    <div class="card">
      <div class="card-title">Detected WAN Adapter</div>
      <div class="form-grid">
        <div class="form-group">
          <label>WAN Stack ID</label>
          <span class="info-label">${esc(wId)}</span>
        </div>
        <div class="form-group">
          <label>Adapter</label>
          <span class="info-label">${esc(wInfo.label)}</span>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Detected LAN Stacks</div>
      <div class="form-grid">
        <div class="form-group">
          <label>Stack 1 (S1)</label>
          <span class="info-label">${esc(s1)} — ${esc(s1Info.label)}</span>
        </div>
        <div class="form-group">
          <label>Type</label>
          <span class="info-label">${esc(s1Info.type)}</span>
        </div>
        <div class="form-group">
          <label>Stack 2 (S2)</label>
          <span class="info-label">${esc(s2)} — ${esc(s2Info.label)}</span>
        </div>
        <div class="form-group">
          <label>Type</label>
          <span class="info-label">${esc(s2Info.type)}</span>
        </div>
      </div>
    </div>

    <div class="card" style="border-color:var(--accent)">
      <span class="hint">💡 Module config tabs appear automatically when a module stack is detected. Use Advanced Mode for full JSON config editing.</span>
    </div>
  `;
}

function esc(s) { return (s || '').replace(/</g, '&lt;'); }
