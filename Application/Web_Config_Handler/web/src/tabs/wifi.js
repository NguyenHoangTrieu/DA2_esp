/**
 * wifi.js — WiFi settings tab (Basic & Advanced use same layout)
 *
 * Fields: SSID, Password (+show toggle), Auth Mode, Username (Enterprise only)
 * Command: CFWF:SSID:PASSWORD:AUTH_MODE  then CFIN:WIFI
 */

import { postConfig } from '../api.js';
import { toast } from '../main.js';

export function renderWifi(container, config) {
  container.innerHTML = '';
  const w = config?.wifi || {};
  const wan = config?.wan || {};

  container.innerHTML = `
    <div class="card">
      <div class="card-title">WiFi Network Settings</div>
      <div class="form-grid">
        <div class="form-group">
          <label>SSID</label>
          <input type="text" id="wfSSID" value="${esc(w.ssid)}" placeholder="Enter WiFi SSID">
        </div>
        <div class="form-group">
          <label>Password</label>
          <div style="display:flex;gap:4px">
            <input type="password" id="wfPass" value="${esc(w.password)}" style="flex:1">
            <label class="checkbox-row" style="white-space:nowrap"><input type="checkbox" id="wfShowPw"> Show</label>
          </div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Authentication</div>
      <div class="form-grid">
        <div class="form-group">
          <label>Auth Mode</label>
          <select id="wfAuth">
            <option value="PERSONAL" ${w.auth_mode === 'ENTERPRISE' ? '' : 'selected'}>PERSONAL</option>
            <option value="ENTERPRISE" ${w.auth_mode === 'ENTERPRISE' ? 'selected' : ''}>ENTERPRISE</option>
          </select>
        </div>
        <div class="form-group ${w.auth_mode === 'ENTERPRISE' ? '' : 'hidden'}" id="wfUserGroup">
          <label>Username</label>
          <input type="text" id="wfUser" value="${esc(w.username)}" placeholder="Enterprise username">
        </div>
      </div>
    </div>

    <div class="btn-row">
      <button class="btn btn-set" id="wfSetBtn">&#x2705; Set WiFi Config</button>
    </div>
    <div class="card">
      <div class="form-group">
        <label class="checkbox-row"><input type="checkbox" id="wfFallback" ${wan.internet_fallback ? 'checked' : ''}> Enable LTE/Ethernet fallback when WiFi fails</label>
      </div>
    </div>
  `;

  // Show/hide password
  const showCb = container.querySelector('#wfShowPw');
  const passIn = container.querySelector('#wfPass');
  showCb.addEventListener('change', () => { passIn.type = showCb.checked ? 'text' : 'password'; });

  // Show/hide enterprise username
  const authSel = container.querySelector('#wfAuth');
  const userGrp = container.querySelector('#wfUserGroup');
  authSel.addEventListener('change', () => {
    userGrp.classList.toggle('hidden', authSel.value !== 'ENTERPRISE');
  });

  // Set button
  container.querySelector('#wfSetBtn').addEventListener('click', async () => {
    const ssid = container.querySelector('#wfSSID').value.trim();
    if (!ssid) { toast('SSID is required', 'err'); return; }
    try {
      await postConfig({
        wifi: {
          ssid,
          password: passIn.value,
          auth_mode: authSel.value,
          username: container.querySelector('#wfUser').value,
        },
        wan: {
          internet_type: 'WIFI',
          internet_fallback: container.querySelector('#wfFallback')?.checked ? 1 : 0,
        }
      });
      toast('WiFi config set');
    } catch (e) {
      toast('WiFi set failed: ' + e.message, 'err');
    }
  });
}

function esc(s) { return (s || '').replace(/"/g, '&quot;'); }
