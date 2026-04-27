/**
 * rs485.js — Advanced RS485 GPIO Mode Config tab
 *
 * Renders a dedicated form for RS485 GPIO pin toggling config.
 * Command formats:
 *   Baud rate : CFRS:BR:<baud>
 *   GPIO JSON : CFRS:JSON:<stack_id>:{...json...}
 *
 * JSON structure for GPIO mode config:
 * {
 *   "module_id": "RS485",
 *   "module_type": "RS485",
 *   "stack_id": 0,
 *   "functions": [
 *     { "function_name": "RS485_SEND_MODE",
 *       "gpio_start_control": [{"pin":"03","state":"HIGH"},{"pin":"02","state":"HIGH"}],
 *       "delay_start": 1, "gpio_end_control": [], "delay_end": 0 },
 *     { "function_name": "RS485_RECEIVE_MODE",
 *       "gpio_start_control": [{"pin":"03","state":"LOW"},{"pin":"02","state":"LOW"}],
 *       "delay_start": 1, "gpio_end_control": [], "delay_end": 0 }
 *   ]
 * }
 */

import { postLanConfig } from '../api.js';

function toast(msg, type = 'ok') {
  const el = document.getElementById('toast');
  if (!el) return;
  el.textContent = msg;
  el.className = `show ${type}`;
  setTimeout(() => { el.className = ''; }, 3000);
}

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

const PIN_OPTS = [
  '01','02','03','04','05','06','07','08','09',
  '11','12','13','14','15','16','17','18','19',
];

const STATE_OPTS = ['HIGH', 'LOW'];

// ── GPIO pin action list ──────────────────────────────────────────────────────

/**
 * Creates an editable list of GPIO pin actions ({ pin, state }).
 * Returns an object with:
 *   .element   — DOM element to append
 *   .getValue  — () => [{ pin, state }, ...]
 *   .getDelay  — () => number
 */
function createGpioPinList(label, defaults, defaultDelay = 1) {
  const wrap = el('div', { className: 'card' }, [
    el('div', { className: 'card-title', textContent: label }),
  ]);

  // Pin rows container
  const rows = el('div', { style: 'display:flex;flex-direction:column;gap:4px;margin-bottom:6px' });
  wrap.appendChild(rows);
  const pinData = [];   // [{ pinSel, stateSel, rowEl }]

  function addRow(pin = '03', state = 'HIGH') {
    const pinSel = el('select', { style: 'width:70px' });
    PIN_OPTS.forEach(p => {
      pinSel.appendChild(el('option', { value: p, textContent: p }));
    });
    pinSel.value = pin;

    const stateSel = el('select', { style: 'width:70px' });
    STATE_OPTS.forEach(s => {
      stateSel.appendChild(el('option', { value: s, textContent: s }));
    });
    stateSel.value = state;

    const removeBtn = el('button', {
      className: 'btn',
      textContent: '✕',
      style: 'padding:2px 8px;font-size:12px',
    });

    const rowEl = el('div', {
      style: 'display:flex;gap:6px;align-items:center',
    }, [
      el('span', { textContent: 'Pin:', style: 'font-size:12px;min-width:28px' }),
      pinSel,
      el('span', { textContent: 'State:', style: 'font-size:12px;min-width:36px' }),
      stateSel,
      removeBtn,
    ]);

    const entry = { pinSel, stateSel, rowEl };
    removeBtn.addEventListener('click', () => {
      const idx = pinData.indexOf(entry);
      if (idx !== -1) { pinData.splice(idx, 1); rows.removeChild(rowEl); }
    });

    pinData.push(entry);
    rows.appendChild(rowEl);
  }

  defaults.forEach(d => addRow(d.pin, d.state));

  wrap.appendChild(el('button', {
    className: 'btn',
    textContent: '+ Add Pin',
    style: 'font-size:12px;padding:2px 10px',
    onClick: () => addRow(),
  }));

  // Delay
  const delayInput = el('input', {
    type: 'number', min: '0', value: String(defaultDelay),
    style: 'width:70px',
  });
  const delayRow = el('div', { style: 'display:flex;align-items:center;gap:6px;margin-top:6px' }, [
    el('span', { textContent: 'Delay after (ms):', style: 'font-size:12px' }),
    delayInput,
  ]);
  wrap.appendChild(delayRow);

  return {
    element: wrap,
    getValue: () => pinData.map(e => ({ pin: e.pinSel.value, state: e.stateSel.value })),
    getDelay: () => { const v = parseInt(delayInput.value, 10); return isNaN(v) ? 0 : v; },
  };
}

// ── Main export ───────────────────────────────────────────────────────────────

export function renderRs485(container, config) {
  container.innerHTML = '';

  // ── Baud Rate card ───────────────────────────────────────────────────────
  const baudCard = el('div', { className: 'card' }, [
    el('div', { className: 'card-title', textContent: '⚡ RS485 Baud Rate' }),
  ]);

  const baudRow = el('div', { className: 'form-grid' });
  const baudSel = el('select', { style: 'width:130px' });
  ['9600','19200','38400','57600','115200'].forEach(b => {
    baudSel.appendChild(el('option', { value: b, textContent: b }));
  });
  baudSel.value = '115200';

  baudRow.append(
    el('div', { className: 'form-group' }, [
      el('label', { textContent: 'Baud Rate' }),
      baudSel,
    ])
  );
  baudCard.appendChild(baudRow);

  let baudStatus = el('div', { className: 'status-panel', textContent: 'Ready' });
  baudCard.appendChild(baudStatus);

  baudCard.appendChild(el('button', {
    className: 'btn btn-set',
    textContent: '📤 Set Baud Rate',
    style: 'margin-top:8px',
    onClick: async () => {
      try {
        const res = await postLanConfig('rs485_baud', baudSel.value);
        baudStatus.textContent = `✓ ${res?.message || `Baud rate set to ${baudSel.value}`}`;
        toast(res?.message || `RS485 baud → ${baudSel.value}`);
      } catch (e) {
        baudStatus.textContent = '✗ Error: ' + e.message;
        toast(e.message, 'err');
      }
    },
  }));

  // ── GPIO Config card ─────────────────────────────────────────────────────
  const gpioCard = el('div', { className: 'card' }, [
    el('div', { className: 'card-title', textContent: '🔌 RS485 GPIO Mode Config (JSON)' }),
    el('div', {
      className: 'hint',
      innerHTML: 'Define which GPIO pins toggle for <b>SEND</b> and <b>RECEIVE</b> modes.<br>'
               + 'Pin format: <code>XY</code> — X=stack port (0/1), Y=pin 1-9.<br>'
               + 'Default: pin <code>03</code>=DE, pin <code>02</code>=RE (Stack 0).',
    }),
  ]);

  // Stack ID
  const stackRow = el('div', { className: 'form-group', style: 'margin:8px 0' }, [
    el('label', { textContent: 'Stack ID' }),
  ]);
  const stackSel = el('select', { style: 'width:80px;margin-top:4px' });
  [{ value: '0', label: 'Slot 1 (S0)' }, { value: '1', label: 'Slot 2 (S1)' }].forEach(o => {
    stackSel.appendChild(el('option', { value: o.value, textContent: o.label }));
  });
  stackRow.appendChild(stackSel);
  gpioCard.appendChild(stackRow);

  // SEND mode pin list
  const sendList = createGpioPinList(
    '🔴 SEND Mode — DE/RE pins to assert',
    [{ pin: '03', state: 'HIGH' }, { pin: '02', state: 'HIGH' }],
    1
  );
  gpioCard.appendChild(sendList.element);

  // RECEIVE mode pin list
  const recvList = createGpioPinList(
    '🟢 RECEIVE Mode — DE/RE pins to de-assert',
    [{ pin: '03', state: 'LOW' }, { pin: '02', state: 'LOW' }],
    1
  );
  gpioCard.appendChild(recvList.element);

  // JSON preview
  const previewArea = el('textarea', {
    className: 'json-preview',
    rows: '14',
    style: 'margin-top:10px',
  });

  function buildJson() {
    const stackId = parseInt(stackSel.value, 10);
    return {
      module_id: 'RS485',
      module_type: 'RS485',
      stack_id: stackId,
      functions: [
        {
          function_name: 'RS485_SEND_MODE',
          gpio_start_control: sendList.getValue(),
          delay_start: sendList.getDelay(),
          gpio_end_control: [],
          delay_end: 0,
        },
        {
          function_name: 'RS485_RECEIVE_MODE',
          gpio_start_control: recvList.getValue(),
          delay_start: recvList.getDelay(),
          gpio_end_control: [],
          delay_end: 0,
        },
      ],
    };
  }

  function refreshPreview() {
    previewArea.value = JSON.stringify(buildJson(), null, 2);
  }

  // Auto-refresh preview when stack changes
  stackSel.addEventListener('change', refreshPreview);

  gpioCard.appendChild(el('button', {
    className: 'btn',
    textContent: '🔄 Preview JSON',
    style: 'margin-top:8px',
    onClick: refreshPreview,
  }));
  gpioCard.appendChild(previewArea);

  let gpioStatus = el('div', { className: 'status-panel', textContent: 'Ready', style: 'margin-top:6px' });
  gpioCard.appendChild(gpioStatus);

  const gpioActRow = el('div', { className: 'btn-row', style: 'margin-top:8px' });

  gpioActRow.appendChild(el('button', {
    className: 'btn',
    textContent: '💾 Save JSON',
    onClick: () => {
      const cfg = buildJson();
      const blob = new Blob([JSON.stringify(cfg, null, 2)], { type: 'application/json' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = 'rs485_config.json';
      a.click();
      URL.revokeObjectURL(a.href);
    },
  }));

  gpioActRow.appendChild(el('button', {
    className: 'btn btn-set',
    textContent: '📤 Send GPIO Config',
    onClick: async () => {
      try {
        const cfg = buildJson();
        const jsonStr = JSON.stringify(cfg);
        const slot = stackSel.value;
        /* Format: "<slot>:<minified_json>" → api builds ML:CFRS:JSON:<slot>:<json> */
        const res = await postLanConfig('rs485_json', `${slot}:${jsonStr}`);
        gpioStatus.textContent = `✓ ${res?.message || `GPIO config sent (${jsonStr.length} bytes)`}`;
        toast(res?.message || 'RS485 GPIO config sent');
        refreshPreview();
      } catch (e) {
        gpioStatus.textContent = '✗ Error: ' + e.message;
        toast(e.message, 'err');
      }
    },
  }));

  gpioCard.appendChild(gpioActRow);

  // ── Assemble ─────────────────────────────────────────────────────────────
  container.append(baudCard, gpioCard);

  // Initial preview
  refreshPreview();
}

