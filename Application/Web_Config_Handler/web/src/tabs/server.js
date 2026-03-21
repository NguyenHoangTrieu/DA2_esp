/**
 * server.js — Server settings tab (Basic & Advanced)
 *
 * Server type selector → MQTT / HTTP / CoAP sub-sections
 * Basic:    minimal fields
 * Advanced: full fields + command hint footer
 */

import { postConfig } from '../api.js';
import { toast } from '../main.js';

export function renderServer(container, config) {
  container.innerHTML = '';
  const srv = config?.server || {};
  const isAdvanced = container.id?.includes('adv');
  const type = srv.type ?? 0; // 0=MQTT, 1=CoAP, 2=HTTP

  const mqtt = srv.mqtt || {};
  const http = srv.http || {};
  const coap = srv.coap || {};

  container.innerHTML = `
    <div class="card">
      <div class="card-title">Server Type</div>
      <div class="form-group" style="max-width:250px">
        <select id="svType">
          <option value="0" ${type===0?'selected':''}>MQTT</option>
          <option value="1" ${type===1?'selected':''}>CoAP</option>
          <option value="2" ${type===2?'selected':''}>HTTP / HTTPS</option>
        </select>
      </div>
    </div>

    <!-- MQTT section -->
    <div id="svMqtt" class="${type===0?'':'hidden'}">
      <div class="card">
        <div class="card-title">MQTT Settings</div>
        <div class="form-grid">
          <div class="form-group full">
            <label>Broker URI</label>
            <input type="text" id="mqBroker" value="${esc(mqtt.broker)}" placeholder="mqtt://demo.thingsboard.io:1883">
            <span class="hint">Format: mqtt[s]://host:port</span>
          </div>
          <div class="form-group full">
            <label>Device Token</label>
            <div style="display:flex;gap:4px">
              <input type="password" id="mqToken" value="${esc(mqtt.token)}" style="flex:1">
              <label class="checkbox-row" style="white-space:nowrap"><input type="checkbox" id="mqShowTk"> Show</label>
            </div>
          </div>
        </div>
      </div>

      ${isAdvanced ? `
      <div class="card">
        <div class="card-title">MQTT Topics</div>
        <div class="form-grid">
          <div class="form-group full">
            <label>Subscribe Topic</label>
            <input type="text" id="mqSub" value="${esc(mqtt.sub_topic)}" placeholder="v1/devices/me/rpc/request/+">
          </div>
          <div class="form-group full">
            <label>Publish Topic</label>
            <input type="text" id="mqPub" value="${esc(mqtt.pub_topic)}" placeholder="v1/devices/me/telemetry">
          </div>
          <div class="form-group full">
            <label>Attribute Topic</label>
            <input type="text" id="mqAttr" value="${esc(mqtt.attr_topic)}" placeholder="v1/devices/me/attributes">
          </div>
        </div>
      </div>
      ` : ''}
    </div>

    <!-- HTTP section -->
    <div id="svHttp" class="${type===2?'':'hidden'}">
      <div class="card">
        <div class="card-title">HTTP / HTTPS Settings</div>
        <div class="form-grid">
          <div class="form-group full">
            <label>Server URL</label>
            <input type="text" id="hpUrl" value="${esc(http.url)}" placeholder="http://demo.thingsboard.io:8080/api/v1/{token}/telemetry">
          </div>
          <div class="form-group full">
            <label>Auth Token</label>
            <input type="text" id="hpToken" value="${esc(http.auth_token)}">
          </div>
          ${isAdvanced ? `
          <div class="form-group">
            <label>Port</label>
            <input type="number" id="hpPort" value="${http.port||8080}" min="1" max="65535">
          </div>
          <div class="form-group">
            <label>Timeout (ms)</label>
            <input type="number" id="hpTimeout" value="${http.timeout||10000}" min="1000" max="120000" step="1000">
          </div>
          <div class="form-group">
            <label class="checkbox-row"><input type="checkbox" id="hpTls" ${http.use_tls?'checked':''}> Use TLS (HTTPS)</label>
          </div>
          <div class="form-group">
            <label class="checkbox-row"><input type="checkbox" id="hpVerify" ${http.verify?'checked':''}> Verify Server Cert</label>
          </div>
          ` : `
          <div class="form-group">
            <label class="checkbox-row"><input type="checkbox" id="hpTls" ${http.use_tls?'checked':''}> Use TLS (HTTPS)</label>
          </div>
          `}
          <div class="form-group full">
            <span class="hint">💡 Use {token} in URL to inject the auth token</span>
          </div>
        </div>
      </div>
    </div>

    <!-- CoAP section -->
    <div id="svCoap" class="${type===1?'':'hidden'}">
      <div class="card">
        <div class="card-title">CoAP Settings</div>
        <div class="form-grid">
          <div class="form-group full">
            <label>Host</label>
            <input type="text" id="cpHost" value="${esc(coap.host)}" placeholder="demo.thingsboard.io">
          </div>
          <div class="form-group full">
            <label>Resource Path</label>
            <input type="text" id="cpResource" value="${esc(coap.resource)}" placeholder="/api/v1/{token}/telemetry">
          </div>
          <div class="form-group full">
            <label>Device Token</label>
            <input type="text" id="cpToken" value="${esc(coap.token)}">
          </div>
          ${isAdvanced ? `
          <div class="form-group">
            <label>Port</label>
            <input type="number" id="cpPort" value="${coap.port||5683}" min="1" max="65535">
          </div>
          <div class="form-group">
            <label>ACK Timeout (ms)</label>
            <input type="number" id="cpAck" value="${coap.ack_timeout||2000}" min="100" max="60000" step="100">
          </div>
          <div class="form-group">
            <label>Max Retransmit</label>
            <input type="number" id="cpRtx" value="${coap.max_rtx||4}" min="0" max="20">
          </div>
          <div class="form-group">
            <label class="checkbox-row"><input type="checkbox" id="cpDtls" ${coap.use_dtls?'checked':''}> Use DTLS (CoAPS — port 5684)</label>
          </div>
          ` : ''}
          <div class="form-group full">
            <span class="hint">💡 Use {token} in Resource Path to inject the device token</span>
          </div>
        </div>
      </div>
    </div>

    <div class="btn-row">
      <button class="btn btn-set" id="svSetBtn">✅ Set Server Config</button>
    </div>

    ${isAdvanced ? `
    <div class="card" style="margin-top:8px;padding:8px">
      <span class="hint">
        MQTT: CFSV:0 + CFMQ:BROKER|TOKEN|SUB|PUB|ATTR<br>
        HTTP: CFSV:2 + CFHP:URL|TOKEN|PORT|TLS|VERIFY|TIMEOUT<br>
        CoAP: CFSV:1 + CFCP:HOST|PATH|TOKEN|PORT|DTLS|ACK_TO|MAX_RTX
      </span>
    </div>
    ` : ''}
  `;

  // Server type switching
  const typeSel = container.querySelector('#svType');
  const sections = {
    '0': container.querySelector('#svMqtt'),
    '1': container.querySelector('#svCoap'),
    '2': container.querySelector('#svHttp'),
  };
  typeSel.addEventListener('change', () => {
    Object.values(sections).forEach(s => s.classList.add('hidden'));
    sections[typeSel.value]?.classList.remove('hidden');
  });

  // Show token
  const mqShowTk = container.querySelector('#mqShowTk');
  const mqToken = container.querySelector('#mqToken');
  if (mqShowTk && mqToken) {
    mqShowTk.addEventListener('change', () => { mqToken.type = mqShowTk.checked ? 'text' : 'password'; });
  }

  // Set button
  container.querySelector('#svSetBtn').addEventListener('click', async () => {
    const t = Number(typeSel.value);
    const payload = { server: { type: t } };

    if (t === 0) {
      payload.server.mqtt = {
        broker: container.querySelector('#mqBroker').value,
        token: mqToken.value,
      };
      if (isAdvanced) {
        payload.server.mqtt.sub_topic = container.querySelector('#mqSub')?.value || '';
        payload.server.mqtt.pub_topic = container.querySelector('#mqPub')?.value || '';
        payload.server.mqtt.attr_topic = container.querySelector('#mqAttr')?.value || '';
      }
    } else if (t === 2) {
      payload.server.http = {
        url: container.querySelector('#hpUrl').value,
        auth_token: container.querySelector('#hpToken').value,
        use_tls: container.querySelector('#hpTls')?.checked || false,
      };
      if (isAdvanced) {
        payload.server.http.port = Number(container.querySelector('#hpPort')?.value || 8080);
        payload.server.http.timeout = Number(container.querySelector('#hpTimeout')?.value || 10000);
        payload.server.http.verify = container.querySelector('#hpVerify')?.checked || false;
      }
    } else if (t === 1) {
      payload.server.coap = {
        host: container.querySelector('#cpHost').value,
        resource: container.querySelector('#cpResource').value,
        token: container.querySelector('#cpToken').value,
      };
      if (isAdvanced) {
        payload.server.coap.port = Number(container.querySelector('#cpPort')?.value || 5683);
        payload.server.coap.ack_timeout = Number(container.querySelector('#cpAck')?.value || 2000);
        payload.server.coap.max_rtx = Number(container.querySelector('#cpRtx')?.value || 4);
        payload.server.coap.use_dtls = container.querySelector('#cpDtls')?.checked || false;
      }
    }

    try {
      await postConfig(payload);
      toast('Server config set');
    } catch (e) {
      toast('Server set failed: ' + e.message, 'err');
    }
  });
}

function esc(s) { return (s || '').replace(/"/g, '&quot;'); }
