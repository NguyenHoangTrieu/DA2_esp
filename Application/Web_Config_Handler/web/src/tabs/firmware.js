/**
 * firmware.js — Advanced Firmware tab (OTA)
 *
 * One button → POST /api/config { fota: true }
 * API enqueues ONE command: raw_data="ML:CFFW", type=CONFIG_TYPE_MCU_LAN
 * Firmware handles FOTA for both WAN and LAN MCUs.
 * URLs are hardcoded in firmware (configured during build).
 */

import { postConfig } from '../api.js';
import { toast } from '../main.js';

export function renderFirmware(container, config) {
  container.innerHTML = '';

  container.innerHTML = `
    <div class="card">
      <div class="card-title">Firmware OTA Update</div>
      <p style="font-size:12px; margin-bottom:16px">Updates both WAN and LAN MCUs simultaneously. Firmware URLs are pre-configured.</p>
      <p class="hint">⚠️ Do not power off the gateway during the update process.</p>
      <div class="btn-row">
        <button class="btn btn-accent" id="fwStartBtn">🔄 Start Firmware OTA Update</button>
      </div>
    </div>
  `;

  container.querySelector('#fwStartBtn').addEventListener('click', async () => {
    try {
      await postConfig({ fota: true });
      toast('Firmware OTA started for both MCUs', 'success');
    } catch (e) {
      toast('Firmware OTA failed: ' + e.message, 'err');
    }
  });
}
