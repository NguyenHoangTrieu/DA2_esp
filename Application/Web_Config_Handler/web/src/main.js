/**
 * main.js — DA2 Gateway Config SPA entry point
 *
 * Basic mode:  WiFi, (LTE conditional), Server, Interfaces
 * Advanced mode: WiFi, LTE, Server, BLE, LoRa, Zigbee, Firmware (7 fixed tabs)
 */

import './style.css';
import { fetchConfig, fetchStatus, postConfig, saveJsonFile, loadJsonFile } from './api.js';
import { WAN_STACK_MAP } from './stack-data.js';

import { renderWifi }       from './tabs/wifi.js';
import { renderLte }        from './tabs/lte.js';
import { renderServer }     from './tabs/server.js';
import { renderInterfaces } from './tabs/interfaces.js';
import { renderBle }        from './tabs/ble.js';
import { renderLora }       from './tabs/lora.js';
import { renderZigbee }     from './tabs/zigbee.js';
import { renderFirmware }   from './tabs/firmware.js';

// ── Toast helper (exported for use by tabs) ───────────────────────────────────
let _toastTimer = null;
export function toast(msg, type = 'ok') {
  const el = document.getElementById('toast');
  if (!el) return;
  el.textContent = msg;
  el.className = `show ${type}`;
  if (_toastTimer) clearTimeout(_toastTimer);
  _toastTimer = setTimeout(() => { el.className = ''; }, 3000);
}

// ── State ─────────────────────────────────────────────────────────────────────
let currentConfig = {};   // last fetched gateway config
let advancedMode = false;

// ── DOM refs ──────────────────────────────────────────────────────────────────
const advToggle      = document.getElementById('advancedToggle');
const basicPanel     = document.getElementById('basicPanel');
const advancedPanel  = document.getElementById('advancedPanel');
const basicTabBar    = document.getElementById('basicTabBar');
const basicContent   = document.getElementById('basicContent');
const advancedTabBar = document.getElementById('advancedTabBar');
const advancedContent= document.getElementById('advancedContent');
const statusDot      = document.getElementById('statusDot');
const statusHost     = document.getElementById('statusHost');
const statusText     = document.getElementById('statusText');

// ── Tab definitions (per mode) ────────────────────────────────────────────────

const BASIC_FIXED_TABS = [
  { id: 'wifi',       icon: '📶', label: 'WiFi',       render: renderWifi },
  // LTE inserted dynamically if WAN adapter detected
  { id: 'server',     icon: '🌐', label: 'Server',     render: renderServer },
  { id: 'interfaces', icon: '🔌', label: 'Interfaces', render: renderInterfaces },
];

const ADVANCED_TABS = [
  { id: 'wifi',     icon: '📶', label: 'WiFi',     render: renderWifi },
  { id: 'lte',      icon: '📱', label: 'LTE',      render: renderLte },
  { id: 'server',   icon: '☁️', label: 'Server',    render: renderServer },
  { id: 'ble',      icon: '🔷', label: 'BLE',      render: renderBle },
  { id: 'lora',     icon: '🟩', label: 'LoRa',     render: renderLora },
  { id: 'zigbee',   icon: '🔶', label: 'Zigbee',   render: renderZigbee },
  { id: 'firmware', icon: '🔄', label: 'FW',       render: renderFirmware },
];

// ── Tab management ────────────────────────────────────────────────────────────

function buildTabBar(bar, tabs) {
  bar.innerHTML = '';
  tabs.forEach((t, i) => {
    const btn = document.createElement('button');
    btn.className = 'tab-btn' + (i === 0 ? ' active' : '');
    btn.dataset.tab = t.id;
    btn.textContent = `${t.icon} ${t.label}`;
    bar.appendChild(btn);
  });
}

function renderTabs(container, tabs, config) {
  container.innerHTML = '';
  tabs.forEach((t, i) => {
    const pane = document.createElement('div');
    pane.className = 'tab-pane' + (i === 0 ? '' : ' hidden');
    pane.id = `pane-${advancedMode ? 'adv' : 'bas'}-${t.id}`;
    container.appendChild(pane);
    t.render(pane, config);
  });
}

function switchTab(bar, container, tabId) {
  bar.querySelectorAll('.tab-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === tabId));
  container.querySelectorAll('.tab-pane').forEach(p => p.classList.toggle('hidden', !p.id.endsWith('-' + tabId)));
}

function wireTabBar(bar, container) {
  bar.addEventListener('click', e => {
    const btn = e.target.closest('.tab-btn');
    if (btn && btn.dataset.tab) switchTab(bar, container, btn.dataset.tab);
  });
}

// ── Build basic tabs ─────────────────────────────────────────────────────────

function buildBasicTabs(config) {
  const tabs = [...BASIC_FIXED_TABS];

  // Conditionally add LTE tab only when a LTE-capable WAN adapter is present.
  // stack_wan_id '100' = no WAN (NONE type) → no LTE tab.
  // stack_wan_id '101'/'102' = Quectel/SIMCom LTE modems → show LTE tab.
  const wanId = config?.wan?.stack_wan_id || '100';
  const wanInfo = WAN_STACK_MAP[wanId];
  if (wanInfo && wanInfo.type !== 'NONE') {
    tabs.splice(1, 0, { id: 'lte', icon: '📱', label: 'LTE', render: renderLte });
  }

  return tabs;
}

// ── Mode toggle ───────────────────────────────────────────────────────────────

function applyMode(refresh = false) {
  advancedMode = advToggle.checked;
  localStorage.setItem('advMode', advancedMode ? '1' : '0');
  basicPanel.classList.toggle('hidden', advancedMode);
  advancedPanel.classList.toggle('hidden', !advancedMode);
  if (refresh) rebuildUI();
}

advToggle.checked = localStorage.getItem('advMode') === '1';
advToggle.addEventListener('change', () => applyMode(true));

// ── Rebuild UI (after config load or mode change) ─────────────────────────────

function rebuildUI() {
  // Basic
  const bTabs = buildBasicTabs(currentConfig);
  buildTabBar(basicTabBar, bTabs);
  renderTabs(basicContent, bTabs, currentConfig);

  // Advanced
  buildTabBar(advancedTabBar, ADVANCED_TABS);
  renderTabs(advancedContent, ADVANCED_TABS, currentConfig);

  applyMode(false);
}

// ── Top bar actions ───────────────────────────────────────────────────────────

document.getElementById('btnReadConfig').addEventListener('click', async () => {
  statusText.textContent = 'Reading config…';
  try {
    currentConfig = await fetchConfig();
    rebuildUI();
    toast('Config loaded');
    statusText.textContent = 'Config loaded ✓';
  } catch (e) {
    toast('Read failed: ' + e.message, 'err');
    statusText.textContent = 'Read failed';
  }
});

document.getElementById('btnSaveFile').addEventListener('click', () => {
  try {
    // Collect current form state into an object
    const state = collectFormState();
    saveJsonFile(state);
    toast('Config file saved');
  } catch (e) {
    toast('Save failed: ' + e.message, 'err');
  }
});

document.getElementById('btnLoadFile').addEventListener('click', async () => {
  try {
    const data = await loadJsonFile();
    currentConfig = data;
    rebuildUI();
    toast('Config loaded from file');
    statusText.textContent = 'Loaded from file ✓';
  } catch (e) {
    toast('Load failed: ' + e.message, 'err');
  }
});

/** Collect current form state by reading from DOM inputs. */
function collectFormState() {
  // The config object is always kept in sync by the tab modules
  return structuredClone(currentConfig);
}

// ── Status polling ────────────────────────────────────────────────────────────

async function pollStatus() {
  try {
    const s = await fetchStatus();
    statusDot.classList.remove('offline');
    statusHost.textContent = window.location.hostname || 'gateway.local';

    const parts = [];
    if (s.wifi_connected) parts.push(`WiFi: ${s.wifi_ssid}`);
    if (s.internet_ok) parts.push('Internet: OK');
    if (s.server_ok) parts.push('Server: OK');
    if (s.uptime_s != null) {
      const h = Math.floor(s.uptime_s / 3600);
      const m = Math.floor((s.uptime_s % 3600) / 60);
      parts.push(`Up: ${h}h${m}m`);
    }
    statusText.textContent = parts.join(' | ') || 'Connected';
  } catch {
    statusDot.classList.add('offline');
    statusText.textContent = 'Offline';
  }
}

// ── Init ──────────────────────────────────────────────────────────────────────

async function init() {
  // Wire up tab bars
  wireTabBar(basicTabBar, basicContent);
  wireTabBar(advancedTabBar, advancedContent);

  // Try to load config
  try {
    currentConfig = await fetchConfig();
  } catch {
    currentConfig = {};
  }

  rebuildUI();

  // Status polling
  pollStatus();
  setInterval(pollStatus, 5000);
}

init();
