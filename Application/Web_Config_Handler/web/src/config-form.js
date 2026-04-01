/**
 * config-form.js — Shared two-pane JSON Config Builder
 *
 * Used by BLE, LoRa, and Zigbee advanced tabs.
 * Left pane:  Header + Communication + Functions accordion
 * Right pane: JSON preview (editable) + Actions + Status
 */

import { FUNCTION_GROUPS, PRESETS, PIN_OPTIONS, getDefaultConfig } from './stack-data.js';
import { postLanConfig, saveJsonFile, loadJsonFile } from './api.js';
import { toast } from './main.js';

// ── Helpers ───────────────────────────────────────────────────────────────────

function el(tag, attrs = {}, children = []) {
  const e = document.createElement(tag);
  Object.entries(attrs).forEach(([k, v]) => {
    if (k === 'className') e.className = v;
    else if (k === 'textContent') e.textContent = v;
    else if (k === 'innerHTML') e.innerHTML = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2).toLowerCase(), v);
    else e.setAttribute(k, v);
  });
  children.forEach(c => { if (c) e.appendChild(typeof c === 'string' ? document.createTextNode(c) : c); });
  return e;
}

function select(options, value, onChange, attrs = {}) {
  const s = el('select', attrs);
  options.forEach(o => {
    const opt = el('option', { value: typeof o === 'object' ? o.value : o, textContent: typeof o === 'object' ? o.label : o });
    s.appendChild(opt);
  });
  s.value = value;
  if (onChange) s.addEventListener('change', onChange);
  return s;
}

// ── Main export ───────────────────────────────────────────────────────────────

/**
 * Render a full two-pane config form into `container`.
 * @param {HTMLElement} container
 * @param {object} config - Current gateway config
 * @param {string} moduleType - 'BLE'|'LORA'|'ZIGBEE'
 */
export function renderConfigForm(container, config, moduleType) {
  container.innerHTML = '';

  const presets = PRESETS[moduleType] || [];
  const groups  = FUNCTION_GROUPS[moduleType] || [];
  const isZigbee = moduleType === 'ZIGBEE';

  // State
  let presetIdx = 0;
  let cfgData = getDefaultConfig(moduleType, 0) || {};

  // Restore last-saved config from localStorage (remembered across page reloads)
  const storageKey = moduleType.toLowerCase() + '_config';
  const saved = localStorage.getItem(storageKey);
  if (saved) {
    try { cfgData = JSON.parse(saved); } catch {}
  }

  // Merge from server config if available (overrides localStorage)
  const lanKey = moduleType.toLowerCase() + '_config';
  if (config?.[lanKey]) {
    try { cfgData = structuredClone(config[lanKey]); } catch {}
  }

  // Always ensure module_type is set to the correct type
  if (!cfgData.module_type) cfgData.module_type = moduleType;

  // ── Header ────────────────────────────────────────────────────────────────
  const headerCard = el('div', { className: 'card' });

  const hGrid = el('div', { className: 'form-grid' });

  // Stack Slot
  const slotGroup = el('div', { className: 'form-group' }, [
    el('label', { textContent: 'Stack Slot' }),
    select([{ value: '0', label: 'S1' }, { value: '1', label: 'S2' }], '0', null, { id: `slot-${moduleType}` }),
  ]);

  // Preset
  const presetSel = select(presets.map((p, i) => ({ value: String(i), label: p.label })), '0', (e) => {
    presetIdx = Number(e.target.value);
    const p = presets[presetIdx];
    modIdInput.value = p.module_id;
    modNameInput.value = p.module_name;
    cfgData = getDefaultConfig(moduleType, presetIdx) || cfgData;
    rebuildFunctions();
    updatePreview();
  });
  const presetGroup = el('div', { className: 'form-group' }, [
    el('label', { textContent: 'Preset' }),
    presetSel,
  ]);

  const reloadBtn = el('button', { className: 'btn', textContent: '🔄 Reload', onClick: () => {
    cfgData = getDefaultConfig(moduleType, presetIdx) || cfgData;
    modIdInput.value = cfgData.module_id;
    modNameInput.value = cfgData.module_name;
    rebuildComm();
    rebuildFunctions();
    updatePreview();
    toast('Form reloaded from preset');
  }});

  // Module ID / Name
  const modIdInput = el('input', { type: 'text', value: cfgData.module_id || '' });
  modIdInput.addEventListener('input', () => { cfgData.module_id = modIdInput.value; updatePreview(); });
  const modIdGroup = el('div', { className: 'form-group' }, [el('label', { textContent: 'Module ID' }), modIdInput]);

  const modNameInput = el('input', { type: 'text', value: cfgData.module_name || '' });
  modNameInput.addEventListener('input', () => { cfgData.module_name = modNameInput.value; updatePreview(); });
  const modNameGroup = el('div', { className: 'form-group' }, [el('label', { textContent: 'Module Name' }), modNameInput]);

  hGrid.append(slotGroup, presetGroup, modIdGroup, modNameGroup);
  headerCard.append(hGrid, el('div', { className: 'btn-row' }, [reloadBtn]));

  // ── Communication Section ─────────────────────────────────────────────────
  const commCard = el('div', { className: 'card' });
  const commTitle = el('div', { className: 'card-title', textContent: '🔌 Communication' });
  const commBody = el('div');
  commCard.append(commTitle, commBody);

  function rebuildComm() {
    commBody.innerHTML = '';
    const comm = cfgData.module_communication || { port_type: 'uart', parameters: {} };
    const params = comm.parameters || {};

    const grid = el('div', { className: 'form-grid' });

    const portSel = select(['uart','usb','spi','i2c'], comm.port_type, (e) => {
      comm.port_type = e.target.value;
      comm.parameters = {};
      cfgData.module_communication = comm;
      rebuildComm();
      updatePreview();
    });
    grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Port Type' }), portSel]));

    const pt = comm.port_type;
    if (pt === 'uart' || pt === 'usb') {
      const brSel = select(['9600','38400','57600','115200','230400'], String(params.baudrate || 115200), (e) => {
        params.baudrate = Number(e.target.value); comm.parameters = params; updatePreview();
      });
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Baudrate' }), brSel]));
    }
    if (pt === 'uart') {
      const parSel = select(['none','odd','even'], params.parity || 'none', (e) => {
        params.parity = e.target.value; comm.parameters = params; updatePreview();
      });
      const sbSel = select(['1','2'], String(params.stopbit || 1), (e) => {
        params.stopbit = Number(e.target.value); comm.parameters = params; updatePreview();
      });
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Parity' }), parSel]));
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Stop Bit' }), sbSel]));
    }
    if (pt === 'spi') {
      grid.appendChild(el('div', { className: 'form-group' }, [
        el('label', { textContent: 'SPI Mode' }),
        select(['0','1','2','3'], String(params.spi_mode || 0), (e) => { params.spi_mode = Number(e.target.value); updatePreview(); }),
      ]));
      const clkIn = el('input', { type: 'number', value: String(params.clock_hz || 1000000), min: '100000', max: '40000000', step: '100000' });
      clkIn.addEventListener('input', () => { params.clock_hz = Number(clkIn.value); updatePreview(); });
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Clock Hz' }), clkIn]));
      const csIn = el('input', { type: 'text', value: params.cs_pin || '05' });
      csIn.addEventListener('input', () => { params.cs_pin = csIn.value; updatePreview(); });
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'CS Pin' }), csIn]));
    }
    if (pt === 'i2c') {
      const addrIn = el('input', { type: 'text', value: params.i2c_addr || '0x60' });
      addrIn.addEventListener('input', () => { params.i2c_addr = addrIn.value; updatePreview(); });
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'I2C Address' }), addrIn]));
      const clkIn = el('input', { type: 'number', value: String(params.clock_hz || 400000), min: '10000', max: '1000000', step: '10000' });
      clkIn.addEventListener('input', () => { params.clock_hz = Number(clkIn.value); updatePreview(); });
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Clock Hz' }), clkIn]));
    }

    cfgData.module_communication = comm;
    commBody.appendChild(grid);
  }

  // ── Functions accordion ───────────────────────────────────────────────────
  const fnCard = el('div', { className: 'card' });
  const fnTitle = el('div', { className: 'card-title', textContent: '⚙️ Functions' });
  const fnBody = el('div');
  fnCard.append(fnTitle, fnBody);

  function rebuildFunctions() {
    fnBody.innerHTML = '';
    const fns = cfgData.functions || [];

    groups.forEach(group => {
      const gDiv = el('div', { className: 'accordion-group' });
      const gHeader = el('div', { className: 'accordion-header' }, [
        el('span', { className: 'arrow', textContent: '▼' }),
        document.createTextNode(` ${group.emoji} ${group.title} (${group.functions.length})`),
      ]);
      const gBody = el('div', { className: 'accordion-body' });

      gHeader.addEventListener('click', () => {
        gHeader.classList.toggle('collapsed');
        gBody.classList.toggle('collapsed');
      });

      group.functions.forEach(fnName => {
        let fn = fns.find(f => f.function_name === fnName);
        if (!fn) {
          fn = { function_name: fnName, command: '', is_prefix: false, gpio_start_control: [], delay_start: 0, expect_response: '', timeout: 0, gpio_end_control: [], delay_end: 0 };
          fns.push(fn);
        }
        gBody.appendChild(buildFnItem(fn, isZigbee));
      });

      gDiv.append(gHeader, gBody);
      fnBody.appendChild(gDiv);
    });

    cfgData.functions = fns;
  }

  function buildFnItem(fn, zigbee) {
    const item = el('div', { className: 'fn-item' });
    const arrow = el('span', { className: 'fn-arrow', textContent: '▸' });
    const header = el('div', { className: 'fn-header' }, [
      arrow,
      el('span', { className: 'fn-name', textContent: fn.function_name }),
    ]);
    if (fn.is_async_event) {
      header.appendChild(el('span', { className: 'fn-async-badge', textContent: '⚡ async' }));
    }
    const body = el('div', { className: 'fn-body collapsed' });

    header.addEventListener('click', () => {
      const open = body.classList.toggle('collapsed');
      arrow.classList.toggle('open', !open);
      arrow.textContent = open ? '▸' : '▾';
    });

    // Build detail fields
    const grid = el('div', { className: 'form-grid' });

    // Command
    const cmdIn = el('input', { type: 'text', value: fn.command || '' });
    cmdIn.addEventListener('input', () => { fn.command = cmdIn.value; updatePreview(); });
    grid.appendChild(el('div', { className: 'form-group full' }, [el('label', { textContent: 'Command' }), cmdIn]));

    // Is prefix
    const pfxCb = el('input', { type: 'checkbox' });
    pfxCb.checked = !!fn.is_prefix;
    pfxCb.addEventListener('change', () => { fn.is_prefix = pfxCb.checked; updatePreview(); });
    grid.appendChild(el('div', { className: 'form-group' }, [
      el('div', { className: 'checkbox-row' }, [pfxCb, el('label', { textContent: 'Is Prefix' })]),
    ]));

    // Is hex
    const hexCb = el('input', { type: 'checkbox' });
    hexCb.checked = !!fn.is_hex;
    hexCb.addEventListener('change', () => { fn.is_hex = hexCb.checked; updatePreview(); });
    grid.appendChild(el('div', { className: 'form-group' }, [
      el('div', { className: 'checkbox-row' }, [hexCb, el('label', { textContent: 'Is Hex' })]),
    ]));

    // Delay start
    const dsIn = el('input', { type: 'number', value: String(fn.delay_start || 0), min: '0', step: '50' });
    dsIn.addEventListener('input', () => { fn.delay_start = Number(dsIn.value); updatePreview(); });
    grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Delay Start (ms)' }), dsIn]));

    // Expect response
    const erIn = el('input', { type: 'text', value: fn.expect_response || '' });
    erIn.addEventListener('input', () => { fn.expect_response = erIn.value; updatePreview(); });
    grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Expect Response' }), erIn]));

    // Timeout
    const toIn = el('input', { type: 'number', value: String(fn.timeout || 0), min: '0', step: '100' });
    toIn.addEventListener('input', () => { fn.timeout = Number(toIn.value); updatePreview(); });
    if (fn.is_async_event) toIn.disabled = true;
    grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Timeout (ms)' }), toIn]));

    // Delay end
    const deIn = el('input', { type: 'number', value: String(fn.delay_end || 0), min: '0', step: '50' });
    deIn.addEventListener('input', () => { fn.delay_end = Number(deIn.value); updatePreview(); });
    grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Delay End (ms)' }), deIn]));

    // Zigbee extras
    if (zigbee) {
      const ctIn = el('input', { type: 'text', value: fn.cmd_type || '' });
      ctIn.addEventListener('input', () => { fn.cmd_type = ctIn.value; updatePreview(); });
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'CMD Type (hex)' }), ctIn]));

      const ccIn = el('input', { type: 'text', value: fn.cmd_code || '' });
      ccIn.addEventListener('input', () => { fn.cmd_code = ccIn.value; updatePreview(); });
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'CMD Code (hex)' }), ccIn]));

      const rfSel = select(['ascii','hex'], fn.resp_format || 'ascii', (e) => { fn.resp_format = e.target.value; updatePreview(); });
      grid.appendChild(el('div', { className: 'form-group' }, [el('label', { textContent: 'Resp Format' }), rfSel]));
    }

    body.appendChild(grid);

    // GPIO Start
    body.appendChild(buildGpioSection('GPIO Start', fn.gpio_start_control, (arr) => { fn.gpio_start_control = arr; updatePreview(); }));
    // GPIO End
    body.appendChild(buildGpioSection('GPIO End', fn.gpio_end_control, (arr) => { fn.gpio_end_control = arr; updatePreview(); }));

    item.append(header, body);
    return item;
  }

  function buildGpioSection(label, gpioArr, onChange) {
    const wrap = el('div', { style: 'margin: 6px 0' });
    const title = el('label', { textContent: label, style: 'font-weight:600;display:block;margin-bottom:4px' });
    const list = el('div');
    const addBtn = el('button', { className: 'btn', textContent: '+ Add GPIO', style: 'font-size:11px;padding:2px 8px;margin-top:4px' });

    function rebuild() {
      list.innerHTML = '';
      gpioArr.forEach((g, i) => {
        const row = el('div', { className: 'gpio-row' });
        const pinSel = select(PIN_OPTIONS, g.pin || '01', (e) => { g.pin = e.target.value; onChange(gpioArr); });
        const stateSel = select(['LOW','HIGH'], g.state || 'LOW', (e) => { g.state = e.target.value; onChange(gpioArr); });
        const rmBtn = el('button', { className: 'btn btn-danger', textContent: '✕', style: 'font-size:10px;padding:1px 6px',
          onClick: () => { gpioArr.splice(i, 1); onChange(gpioArr); rebuild(); } });
        row.append(el('span', { textContent: 'Pin:' }), pinSel, el('span', { textContent: 'State:' }), stateSel, rmBtn);
        list.appendChild(row);
      });
    }

    addBtn.addEventListener('click', () => { gpioArr.push({ pin: '01', state: 'LOW' }); onChange(gpioArr); rebuild(); });

    rebuild();
    wrap.append(title, list, addBtn);
    return wrap;
  }

  // ── JSON Preview (right pane) ─────────────────────────────────────────────
  const previewCard = el('div', { className: 'card' });
  previewCard.appendChild(el('div', { className: 'card-title', textContent: '📄 Generated JSON' }));
  const previewArea = el('textarea', { className: 'json-preview', rows: '20' });
  previewCard.appendChild(previewArea);

  previewArea.addEventListener('input', () => {
    try {
      const parsed = JSON.parse(previewArea.value);
      cfgData = parsed;
      previewArea.classList.remove('error');
    } catch {
      previewArea.classList.add('error');
    }
  });

  function updatePreview() {
    const clean = structuredClone(cfgData);
    previewArea.value = JSON.stringify(clean, null, 2);
    previewArea.classList.remove('error');
  }

  // ── Actions panel ─────────────────────────────────────────────────────────
  const actionsCard = el('div', { className: 'card' });
  actionsCard.appendChild(el('div', { className: 'card-title', textContent: '🚀 Actions' }));

  const actRow1 = el('div', { className: 'btn-row', style: 'margin-top:0' });
  actRow1.appendChild(el('button', { className: 'btn', textContent: '📋 Generate', onClick: () => {
    updatePreview();
    toast('JSON regenerated');
  }}));
  actRow1.appendChild(el('button', { className: 'btn', textContent: '💾 Save JSON', onClick: () => {
    const name = `${moduleType.toLowerCase()}_config.json`;
    saveJsonFile(cfgData, name);
    setStatus(`Saved to ${name}`, false);
  }}));
  actRow1.appendChild(el('button', { className: 'btn', textContent: '📂 Load JSON', onClick: async () => {
    try {
      const data = await loadJsonFile();
      if (!data.module_type) data.module_type = moduleType;
      cfgData = data;
      modIdInput.value = data.module_id || '';
      modNameInput.value = data.module_name || '';
      rebuildComm();
      rebuildFunctions();
      updatePreview();
      setStatus('JSON loaded from file', false);
    } catch (e) { toast('Load failed: ' + e.message, 'err'); }
  }}));

  const actRow2 = el('div', { className: 'btn-row' });
  actRow2.appendChild(el('button', { className: 'btn btn-set', textContent: '📤 Send JSON Config', onClick: async () => {
    try {
      const slot = container.querySelector(`#slot-${moduleType}`)?.value || '0';
      if (!cfgData.module_type) cfgData.module_type = moduleType;
      const jsonStr = JSON.stringify(cfgData);
      const key = moduleType.toLowerCase() + '_json';
      localStorage.setItem(storageKey, jsonStr);
      await postLanConfig(key, `${slot}:${jsonStr}`);
      setStatus(`JSON sent (${jsonStr.length} bytes)`, false);
      toast('JSON config sent');
    } catch (e) {
      setStatus('Send failed: ' + e.message, true);
      toast('Send failed: ' + e.message, 'err');
    }
  }}));

  actionsCard.append(actRow1, actRow2);

  // ── Status panel ──────────────────────────────────────────────────────────
  const statusCard = el('div', { className: 'card' });
  statusCard.appendChild(el('div', { className: 'card-title', textContent: '📊 Status' }));
  const statusEl = el('div', { className: 'status-panel', textContent: 'Ready' });
  statusCard.appendChild(statusEl);

  function setStatus(msg, isError) {
    const time = new Date().toLocaleTimeString();
    statusEl.innerHTML = '';
    statusEl.appendChild(el('div', { className: isError ? 'last-error' : 'last-action', textContent: msg }));
    statusEl.appendChild(el('div', { textContent: time, style: 'font-size:11px;color:var(--muted)' }));
  }

  // ── Assemble two-pane layout ──────────────────────────────────────────────
  const twoPane = el('div', { className: 'two-pane' });
  const left  = el('div', { className: 'pane-left' }, [headerCard, commCard, fnCard]);
  const right = el('div', { className: 'pane-right' }, [previewCard, actionsCard, statusCard]);
  twoPane.append(left, right);

  container.appendChild(twoPane);

  // Initial render
  rebuildComm();
  rebuildFunctions();
  updatePreview();
}
