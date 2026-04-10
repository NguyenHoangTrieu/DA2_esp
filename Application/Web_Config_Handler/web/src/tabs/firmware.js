/**
 * firmware.js - Firmware OTA URL configuration
 *
 * Saves firmware download URLs for both LAN and WAN MCUs to NVS.
 * LAN URL: POST /api/config { fota: { lan_fw_url } }  -> ML:CFFU:<url> -> LAN NVS
 * WAN URL: POST /api/config { fota: { wan_fw_url } }  -> WAN NVS (direct)
 *
 * OTA must be triggered separately (Python app or UART command).
 */

import { postConfig, fetchConfig } from '../api.js';
import { toast } from '../main.js';

export function renderFirmware(container, config) {
  container.innerHTML = '';

  const savedLanUrl = config?.fota?.lan_fw_url ?? '';
  const savedWanUrl = config?.fota?.wan_fw_url ?? '';

  container.innerHTML = `
    <div class="card">
      <div class="card-title">LAN MCU Firmware URL</div>
      <p style="font-size:12px; margin-bottom:8px">
        Set the firmware download URL for the LAN MCU. Saved to NVS on both MCUs.<br>
        To trigger OTA, use the Python config app or send <code>CFML:CFFW</code> over UART.
      </p>
      <div class="form-row">
        <label class="form-label" for="fotaLanUrl">LAN Firmware URL</label>
        <input class="form-input" id="fotaLanUrl" type="text"
               placeholder="http://192.168.1.100:8080/api/v1/TOKEN/firmware?title=DA2_esp_LAN&version=1.1.2"
               value="${escHtml(savedLanUrl)}" />
        <div class="form-hint">
          Format: <code>http://&lt;host&gt;:&lt;port&gt;/api/v1/&lt;token&gt;/firmware?title=&lt;title&gt;&amp;version=&lt;ver&gt;</code>
        </div>
      </div>
      <div class="btn-row" style="margin-top:12px">
        <button class="btn btn-accent" id="fotaLanSaveBtn">Save LAN URL</button>
      </div>
    </div>

    <div class="card" style="margin-top:16px">
      <div class="card-title">WAN MCU Firmware URL</div>
      <p style="font-size:12px; margin-bottom:8px">
        Set the firmware download URL for the WAN MCU. Saved to NVS and used on next OTA trigger.<br>
        To trigger WAN OTA, use the Python config app or send <code>FW</code> over UART.
      </p>
      <div class="form-row">
        <label class="form-label" for="fotaWanUrl">WAN Firmware URL</label>
        <input class="form-input" id="fotaWanUrl" type="text"
               placeholder="http://192.168.1.100:8080/api/v1/TOKEN/firmware?title=DA2_esp&version=1.1.2"
               value="${escHtml(savedWanUrl)}" />
        <div class="form-hint">
          Format: <code>http://&lt;host&gt;:&lt;port&gt;/api/v1/&lt;token&gt;/firmware?title=&lt;title&gt;&amp;version=&lt;ver&gt;</code>
        </div>
      </div>
      <div class="btn-row" style="margin-top:12px">
        <button class="btn btn-accent" id="fotaWanSaveBtn">Save WAN URL</button>
      </div>
    </div>
  `;

  container.querySelector('#fotaLanSaveBtn').addEventListener('click', async () => {
    const url = container.querySelector('#fotaLanUrl').value.trim();
    if (!url) { toast('Please enter a LAN firmware URL', 'err'); return; }
    try {
      await postConfig({ fota: { lan_fw_url: url } });
      toast('LAN firmware URL saved to NVS', 'success');
    } catch (e) { toast('Save failed: ' + e.message, 'err'); }
  });

  container.querySelector('#fotaWanSaveBtn').addEventListener('click', async () => {
    const url = container.querySelector('#fotaWanUrl').value.trim();
    if (!url) { toast('Please enter a WAN firmware URL', 'err'); return; }
    try {
      await postConfig({ fota: { wan_fw_url: url } });
      toast('WAN firmware URL saved to NVS', 'success');
    } catch (e) { toast('Save failed: ' + e.message, 'err'); }
  });
}

function escHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}