/**
 * server.js — Server settings tab (Basic & Advanced)
 *
 * Server type selector → MQTT / HTTP / CoAP sub-sections
 * Basic:    minimal fields
 * Advanced: full fields (+ MQTT keepalive/timeout) + command hint footer
 *
 * Config format (matches esp32 api_config.c GET/POST):
 *   config.server_type          — 0=MQTT, 1=CoAP, 2=HTTP
 *   config.mqtt.broker_uri      — mqtt[s]://host:port
 *   config.mqtt.device_token
 *   config.mqtt.subscribe_topic
 *   config.mqtt.publish_topic
 *   config.mqtt.attribute_topic
 *   config.mqtt.keepalive_s     — MQTT keepalive interval (seconds)
 *   config.mqtt.timeout_ms      — MQTT network timeout (ms)
 *   config.http.server_url / auth_token / port / use_tls / verify_server / timeout_ms
 *   config.coap.host / resource_path / device_token / port / use_dtls / ack_timeout_ms / max_retransmit / rpc_poll_interval_ms
 */

import { postConfig } from '../api.js';
import { toast } from '../main.js';

export function renderServer(container, config) {
  container.innerHTML = '';
  const isAdvanced = container.id?.includes('adv');

  // Accept both real ESP32 format (flat) and legacy mock-server nested format
  const type = config?.server_type ?? config?.server?.type ?? 0; // 0=MQTT, 1=CoAP, 2=HTTP

  const mqtt = config?.mqtt || config?.server?.mqtt || {};
  const http = config?.http || config?.server?.http || {};
  const coap = config?.coap || config?.server?.coap || {};

  // Normalise field names (real format uses broker_uri, mock uses broker, etc.)
  const mqBrokerVal   = mqtt.broker_uri    ?? mqtt.broker    ?? '';
  const mqTokenVal    = mqtt.device_token  ?? mqtt.token     ?? '';
  const mqSubVal      = mqtt.subscribe_topic ?? mqtt.sub_topic ?? '';
  const mqPubVal      = mqtt.publish_topic   ?? mqtt.pub_topic ?? '';
  const mqAttrVal     = mqtt.attribute_topic ?? mqtt.attr_topic ?? '';
  const mqKeepalive   = mqtt.keepalive_s ?? 120;
  const mqTimeout     = mqtt.timeout_ms  ?? 10000;

  const hpUrlVal      = http.server_url   ?? http.url        ?? '';
  const hpAuthVal     = http.auth_token                      ?? '';
  const hpPortVal     = http.port         ?? 80;
  const hpTlsVal      = http.use_tls      ?? false;
  const hpVerifyVal   = http.verify_server ?? http.verify    ?? false;
  const hpTimeoutVal  = http.timeout_ms   ?? http.timeout    ?? 10000;

  const cpHostVal     = coap.host                            ?? '';
  const cpResVal      = coap.resource_path ?? coap.resource  ?? '';
  const cpTokenVal    = coap.device_token  ?? coap.token     ?? '';
  const cpPortVal     = coap.port          ?? 5683;
  const cpDtlsVal     = coap.use_dtls      ?? false;
  const cpAckVal      = coap.ack_timeout_ms ?? coap.ack_timeout ?? 2000;
  const cpRtxVal      = coap.max_retransmit ?? coap.max_rtx  ?? 4;
  const cpRpcPollVal  = coap.rpc_poll_interval_ms ?? 1500;

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
            <input type="text" id="mqBroker" value="${esc(mqBrokerVal)}" placeholder="mqtt://demo.thingsboard.io:1883">
            <span class="hint">Format: mqtt[s]://host:port</span>
          </div>
          <div class="form-group full">
            <label>Device Token</label>
            <div style="display:flex;gap:4px">
              <input type="password" id="mqToken" value="${esc(mqTokenVal)}" style="flex:1">
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
            <input type="text" id="mqSub" value="${esc(mqSubVal)}" placeholder="v1/devices/me/rpc/request/+">
          </div>
          <div class="form-group full">
            <label>Publish Topic</label>
            <input type="text" id="mqPub" value="${esc(mqPubVal)}" placeholder="v1/devices/me/telemetry">
          </div>
          <div class="form-group full">
            <label>Attribute Topic</label>
            <input type="text" id="mqAttr" value="${esc(mqAttrVal)}" placeholder="v1/devices/me/attributes">
          </div>
        </div>
      </div>
      <div class="card">
        <div class="card-title">MQTT Session Parameters</div>
        <div class="form-grid">
          <div class="form-group">
            <label>Keepalive (s)</label>
            <input type="number" id="mqKeepalive" value="${mqKeepalive}" min="10" max="3600" step="10">
            <span class="hint">Interval between PINGREQ messages (10–3600 s)</span>
          </div>
          <div class="form-group">
            <label>Network Timeout (ms)</label>
            <input type="number" id="mqTimeout" value="${mqTimeout}" min="1000" max="60000" step="1000">
            <span class="hint">TCP connect/read timeout (1000–60000 ms)</span>
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
            <input type="text" id="hpUrl" value="${esc(hpUrlVal)}" placeholder="http://demo.thingsboard.io/api/v1/{token}/telemetry">
          </div>
          <div class="form-group full">
            <label>Auth Token</label>
            <input type="text" id="hpToken" value="${esc(hpAuthVal)}">
          </div>
          ${isAdvanced ? `
          <div class="form-group">
            <label>Port</label>
            <input type="number" id="hpPort" value="${hpPortVal}" min="1" max="65535">
          </div>
          <div class="form-group">
            <label>Timeout (ms)</label>
            <input type="number" id="hpTimeout" value="${hpTimeoutVal}" min="1000" max="120000" step="1000">
          </div>
          <div class="form-group">
            <label class="checkbox-row"><input type="checkbox" id="hpTls" ${hpTlsVal?'checked':''}> Use TLS (HTTPS)</label>
          </div>
          <div class="form-group">
            <label class="checkbox-row"><input type="checkbox" id="hpVerify" ${hpVerifyVal?'checked':''}> Verify Server Cert</label>
          </div>
          ` : `
          <div class="form-group">
            <label class="checkbox-row"><input type="checkbox" id="hpTls" ${hpTlsVal?'checked':''}> Use TLS (HTTPS)</label>
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
            <input type="text" id="cpHost" value="${esc(cpHostVal)}" placeholder="demo.thingsboard.io">
          </div>
          <div class="form-group full">
            <label>Resource Path</label>
            <input type="text" id="cpResource" value="${esc(cpResVal)}" placeholder="/api/v1/{token}/telemetry">
          </div>
          <div class="form-group full">
            <label>Device Token</label>
            <input type="text" id="cpToken" value="${esc(cpTokenVal)}">
          </div>
          ${isAdvanced ? `
          <div class="form-group">
            <label>Port</label>
            <input type="number" id="cpPort" value="${cpPortVal}" min="1" max="65535">
          </div>
          <div class="form-group">
            <label>ACK Timeout (ms)</label>
            <input type="number" id="cpAck" value="${cpAckVal}" min="100" max="60000" step="100">
          </div>
          <div class="form-group">
            <label>Max Retransmit</label>
            <input type="number" id="cpRtx" value="${cpRtxVal}" min="0" max="20">
          </div>
          <div class="form-group">            <label>RPC Poll Interval (ms)</label>
            <input type="number" id="cpRpcPoll" value="${cpRpcPollVal}" min="500" max="30000" step="500">
            <span class="hint">How often to poll for pending RPC commands (500–30000 ms)</span>
          </div>
          <div class="form-group">            <label class="checkbox-row"><input type="checkbox" id="cpDtls" ${cpDtlsVal?'checked':''}> Use DTLS (CoAPS — port 5684)</label>
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
        MQTT: CFSV:0 + CFMQ:BROKER|TOKEN|SUB|PUB|ATTR|KEEPALIVE_S|TIMEOUT_MS<br>
        HTTP: CFSV:2 + CFHP:URL|TOKEN|PORT|TLS|VERIFY|TIMEOUT<br>
        CoAP: CFSV:1 + CFCP:HOST|PATH|TOKEN|PORT|DTLS|ACK_TO|MAX_RTX|RPC_POLL_MS
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

  // Show/hide MQTT token
  const mqShowTk = container.querySelector('#mqShowTk');
  const mqToken = container.querySelector('#mqToken');
  if (mqShowTk && mqToken) {
    mqShowTk.addEventListener('change', () => { mqToken.type = mqShowTk.checked ? 'text' : 'password'; });
  }

  // Set button — send flat JSON matching api_config.c POST format
  container.querySelector('#svSetBtn').addEventListener('click', async () => {
    const t = Number(typeSel.value);
    const payload = { server_type: t };

    if (t === 0) {
      payload.mqtt = {
        broker_uri:   container.querySelector('#mqBroker').value,
        device_token: mqToken.value,
      };
      if (isAdvanced) {
        payload.mqtt.subscribe_topic  = container.querySelector('#mqSub')?.value  || '';
        payload.mqtt.publish_topic    = container.querySelector('#mqPub')?.value   || '';
        payload.mqtt.attribute_topic  = container.querySelector('#mqAttr')?.value  || '';
        payload.mqtt.keepalive_s      = Number(container.querySelector('#mqKeepalive')?.value || 120);
        payload.mqtt.timeout_ms       = Number(container.querySelector('#mqTimeout')?.value  || 10000);
      }
    } else if (t === 2) {
      payload.http = {
        server_url:    container.querySelector('#hpUrl').value,
        auth_token:    container.querySelector('#hpToken').value,
        use_tls:       container.querySelector('#hpTls')?.checked || false,
      };
      if (isAdvanced) {
        payload.http.port           = Number(container.querySelector('#hpPort')?.value    || 80);
        payload.http.timeout_ms     = Number(container.querySelector('#hpTimeout')?.value || 10000);
        payload.http.verify_server  = container.querySelector('#hpVerify')?.checked       || false;
      }
    } else if (t === 1) {
      payload.coap = {
        host:          container.querySelector('#cpHost').value,
        resource_path: container.querySelector('#cpResource').value,
        device_token:  container.querySelector('#cpToken').value,
      };
      if (isAdvanced) {
        payload.coap.port           = Number(container.querySelector('#cpPort')?.value || 5683);
        payload.coap.ack_timeout_ms = Number(container.querySelector('#cpAck')?.value  || 2000);
        payload.coap.max_retransmit = Number(container.querySelector('#cpRtx')?.value  || 4);
        payload.coap.rpc_poll_interval_ms = Number(container.querySelector('#cpRpcPoll')?.value || 1500);
        payload.coap.use_dtls       = container.querySelector('#cpDtls')?.checked      || false;
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
