/**
 * lte.js — LTE settings tab
 *
 * Basic mode:  APN, Username, Password only
 * Advanced mode: Full 11 fields (modem, comm, APN, user, pass, auto, timeout, retry, pwr, rst)
 * Command: CFLT:MODEM:APN:USER:PASS:COMM:AUTO:TIMEOUT:RETRY:PWR:RST  then CFIN:LTE
 */

import { postConfig } from '../api.js';
import { toast } from '../main.js';
import { WAN_STACK_MAP, PIN_OPTIONS } from '../stack-data.js';

export function renderLte(container, config) {
  container.innerHTML = '';
  const lte = config?.lte || {};
  const wan = config?.wan || {};
  const wanId = wan.stack_wan_id || '100';
  const wanInfo = WAN_STACK_MAP[wanId] || {};

  // Detect if we're in advanced panel (parent has 'adv' in id)
  const isAdvanced = container.id?.includes('adv');

  let html = '';

  if (isAdvanced) {
    html = `
      <div class="card">
        <div class="card-title">Modem Identity</div>
        <div class="form-grid">
          <div class="form-group">
            <label>WAN Stack ID</label>
            <span class="info-label">${esc(wanId)} — ${esc(wanInfo.label || 'Unknown')}</span>
          </div>
          <div class="form-group">
            <label>Modem Name</label>
            <input type="text" id="ltModem" value="${esc(lte.modem || wanInfo.modem || 'A7600C1')}">
          </div>
          <div class="form-group">
            <label>Comm Type</label>
            <select id="ltComm">
              <option value="USB" ${(lte.comm_type||wanInfo.comm_type)==='UART'?'':'selected'}>USB</option>
              <option value="UART" ${(lte.comm_type||wanInfo.comm_type)==='UART'?'selected':''}>UART</option>
            </select>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-title">APN Settings</div>
        <div class="form-grid">
          <div class="form-group">
            <label>APN</label>
            <input type="text" id="ltAPN" value="${esc(lte.apn)}" placeholder="v-internet">
          </div>
          <div class="form-group">
            <label>Username</label>
            <input type="text" id="ltUser" value="${esc(lte.username)}">
          </div>
          <div class="form-group">
            <label>Password</label>
            <div style="display:flex;gap:4px">
              <input type="password" id="ltPass" value="${esc(lte.password)}" style="flex:1">
              <label class="checkbox-row" style="white-space:nowrap"><input type="checkbox" id="ltShowPw"> Show</label>
            </div>
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-title">Connection Settings</div>
        <div class="form-grid">
          <div class="form-group">
            <label class="checkbox-row"><input type="checkbox" id="ltAuto" ${lte.auto_reconnect !== false ? 'checked' : ''}> Auto Reconnect</label>
          </div>
          <div class="form-group">
            <label>Reconnect Timeout (ms)</label>
            <input type="number" id="ltTimeout" value="${lte.timeout || 30000}" min="5000" max="300000" step="5000">
          </div>
          <div class="form-group">
            <label>Max Retry (0=unlimited)</label>
            <input type="number" id="ltRetry" value="${lte.max_retry || 0}" min="0" max="100">
          </div>
        </div>
      </div>

      <div class="card">
        <div class="card-title">Module GPIO Pins</div>
        <div class="form-grid">
          <div class="form-group">
            <label>PWR Pin</label>
            <select id="ltPwr">${PIN_OPTIONS.map(p => `<option value="${p}" ${(lte.pwr_pin||wanInfo.pwr_pin||'05')===p?'selected':''}>${p}</option>`).join('')}</select>
          </div>
          <div class="form-group">
            <label>RST Pin</label>
            <select id="ltRst">${PIN_OPTIONS.map(p => `<option value="${p}" ${(lte.rst_pin||wanInfo.rst_pin||'06')===p?'selected':''}>${p}</option>`).join('')}</select>
          </div>
        </div>
      </div>

      <div class="btn-row">
        <button class="btn btn-set" id="ltSetBtn">&#x2705; Set LTE Config</button>
      </div>
      <div class="card">
        <div class="form-group">
          <label class="checkbox-row"><input type="checkbox" id="ltFallback" ${wan.internet_fallback ? 'checked' : ''}> Enable WiFi fallback when LTE fails</label>
        </div>
      </div>
    `;
  } else {
    // Basic mode — simple APN only
    html = `
      <div class="card">
        <div class="card-title">LTE Module: ${esc(wanInfo.modem || 'Unknown')}</div>
        <p class="hint">${esc(wanInfo.label || '')}</p>
      </div>

      <div class="card">
        <div class="card-title">LTE Settings</div>
        <div class="form-grid">
          <div class="form-group">
            <label>APN</label>
            <input type="text" id="ltAPN" value="${esc(lte.apn)}" placeholder="internet">
          </div>
          <div class="form-group">
            <label>Username</label>
            <input type="text" id="ltUser" value="${esc(lte.username)}">
          </div>
          <div class="form-group">
            <label>Password</label>
            <div style="display:flex;gap:4px">
              <input type="password" id="ltPass" value="${esc(lte.password)}" style="flex:1">
              <label class="checkbox-row" style="white-space:nowrap"><input type="checkbox" id="ltShowPw"> Show</label>
            </div>
          </div>
        </div>
      </div>

      <div class="btn-row">
        <button class="btn btn-set" id="ltSetBtn">&#x2705; Set LTE Config</button>
      </div>
      <div class="card">
        <div class="form-group">
          <label class="checkbox-row"><input type="checkbox" id="ltFallback" ${wan.internet_fallback ? 'checked' : ''}> Enable WiFi fallback when LTE fails</label>
        </div>
      </div>
    `;
  }

  container.innerHTML = html;

  // Show/hide password
  const showCb = container.querySelector('#ltShowPw');
  const passIn = container.querySelector('#ltPass');
  if (showCb && passIn) {
    showCb.addEventListener('change', () => { passIn.type = showCb.checked ? 'text' : 'password'; });
  }

  // Set button
  container.querySelector('#ltSetBtn').addEventListener('click', async () => {
    let apn = container.querySelector('#ltAPN').value.trim();
    if (!apn) { 
      apn = 'v-internet';
      container.querySelector('#ltAPN').value = apn;
      toast('Using default APN: ' + apn, 'info');
    }

    const payload = {
      lte: {
        apn,
        username: container.querySelector('#ltUser').value,
        password: passIn.value,
      },
      wan: {
        internet_type: 'LTE',
        internet_fallback: container.querySelector('#ltFallback')?.checked ? 1 : 0,
        internet_fallback_type: 'WIFI',
      }
    };

    if (isAdvanced) {
      payload.lte.modem = container.querySelector('#ltModem').value;
      payload.lte.comm_type = container.querySelector('#ltComm').value;
      payload.lte.auto_reconnect = container.querySelector('#ltAuto').checked;
      payload.lte.timeout = Number(container.querySelector('#ltTimeout').value);
      payload.lte.max_retry = Number(container.querySelector('#ltRetry').value);
      payload.lte.pwr_pin = container.querySelector('#ltPwr').value;
      payload.lte.rst_pin = container.querySelector('#ltRst').value;
    }

    try {
      await postConfig(payload);
      toast('LTE config set');
    } catch (e) {
      toast('LTE set failed: ' + e.message, 'err');
    }
  });
}

function esc(s) { return (s || '').replace(/"/g, '&quot;'); }
