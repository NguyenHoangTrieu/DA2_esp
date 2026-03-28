/**
 * stack-data.js — Bundled stack ID maps + default JSON configs + function groups.
 *
 * In the Python app these come from separate JSON files.  For the SPA we bundle
 * them inline so there are zero extra fetch requests once the page is loaded.
 */

// ── Stack ID Map ──────────────────────────────────────────────────────────────

export const LAN_STACK_MAP = {
  '000': { type: 'NONE',   label: 'Empty' },
  '001': { type: 'ZIGBEE', label: 'Zigbee (E180-ZG120B)', cmd_prefix: 'CFZB' },
  '002': { type: 'BLE',    label: 'BLE (STM32WB55)',      cmd_prefix: 'CFBL' },
  '003': { type: 'LORA',   label: 'LoRa (RAK3172)',       cmd_prefix: 'CFLR' },
  '004': { type: 'BLE',    label: 'BLE (Custom)',          cmd_prefix: 'CFBL' },
  '005': { type: 'ZIGBEE', label: 'Zigbee (STM32WB55)',   cmd_prefix: 'CFZB' },
  '006': { type: 'LORA',   label: 'LoRa (Wio-E5 mini)',   cmd_prefix: 'CFLR' },
  '007': { type: 'RS485',  label: 'RS485 Module',         cmd_prefix: 'CFRS' },
};

export const WAN_STACK_MAP = {
  '001': { type: 'QUECTEL', label: 'Quectel (AC76001) / LTE', modem: 'AC76001', comm_type: 'UART', pwr_pin: 'WK', rst_pin: 'PE' },
  '100': { type: 'NONE',    label: 'No WAN adapter',   modem: '',        comm_type: 'USB', pwr_pin: 'WK', rst_pin: 'PE' },
  '101': { type: 'QUECTEL', label: 'Quectel (A7600)',   modem: 'A7600C1', comm_type: 'USB', pwr_pin: 'WK', rst_pin: 'PE' },
  '102': { type: 'SIMCOM',  label: 'SIMCom (SIM7600)',  modem: 'SIM7600', comm_type: 'USB', pwr_pin: 'WK', rst_pin: 'PE' },
};

export const PIN_OPTIONS = ['WK', 'PE', '01', '02', '03', '04', '05', '06', '07', '08', '09', '10', '11'];

// ── Function Groups (per module type — same as Python config_form.py) ─────

export const FUNCTION_GROUPS = {
  BLE: [
    { emoji: '🔄', title: 'System', functions: ['MODULE_HW_RESET','MODULE_SW_RESET','MODULE_FACTORY_RESET','MODULE_ENTER_CMD_MODE','MODULE_ENTER_SLEEP','MODULE_WAKEUP','MODULE_START_BROADCAST'] },
    { emoji: 'ℹ️', title: 'Info', functions: ['MODULE_GET_INFO','MODULE_GET_CONNECTION_STATUS','MODULE_GET_DIAGNOSTICS'] },
    { emoji: '⚙️', title: 'Config', functions: ['MODULE_SET_NAME','MODULE_SET_COMM_CONFIG','MODULE_SET_RF_PARAMS'] },
    { emoji: '🔍', title: 'Discovery', functions: ['MODULE_START_DISCOVERY','MODULE_DISCOVER_SERVICES','MODULE_DISCOVER_CHARACTERISTICS'] },
    { emoji: '🔗', title: 'Connection', functions: ['MODULE_CONNECT','MODULE_DISCONNECT','MODULE_ENTER_DATA_MODE'] },
    { emoji: '📨', title: 'Data', functions: ['MODULE_SEND_DATA'] },
  ],
  LORA: [
    { emoji: '🔄', title: 'System', functions: ['MODULE_HW_RESET','MODULE_SW_RESET','MODULE_GET_INFO','MODULE_FACTORY_RESET'] },
    { emoji: '🌍', title: 'Region & Class', functions: ['MODULE_SET_REGION','MODULE_SET_CLASS'] },
    { emoji: '🔑', title: 'OTAA Provisioning', functions: ['MODULE_SET_JOIN_MODE','MODULE_SET_DEVEUI','MODULE_GET_DEVEUI','MODULE_SET_APPEUI','MODULE_SET_APPKEY','MODULE_JOIN','MODULE_GET_JOIN_STATUS'] },
    { emoji: '🔒', title: 'ABP Provisioning', functions: ['MODULE_SET_DEVADDR','MODULE_SET_NWKSKEY','MODULE_SET_APPSKEY'] },
    { emoji: '📶', title: 'MAC & RF Settings', functions: ['MODULE_SET_DR','MODULE_SET_ADR','MODULE_SET_TXP','MODULE_SET_CHANNEL','MODULE_SET_CONFIRM','MODULE_SET_PUBLIC_NET'] },
    { emoji: '📨', title: 'Data', functions: ['MODULE_SEND_UNCONFIRMED','MODULE_SEND_CONFIRMED','MODULE_READ_RECV'] },
  ],
  ZIGBEE: [
    { emoji: '🔄', title: 'Lifecycle', functions: ['MODULE_HW_RESET','MODULE_SW_RESET','MODULE_FACTORY_RESET','MODULE_GET_INFO','MODULE_ENTER_HEX_MODE','MODULE_ENTER_AT_MODE','MODULE_ENTER_BOOTLOADER','MODULE_SET_COMM_CONFIG'] },
    { emoji: '🌐', title: 'Network Management', functions: ['MODULE_START_NETWORK','MODULE_STOP_NETWORK','MODULE_LEAVE_NETWORK','MODULE_GET_NET_STATUS','MODULE_SET_CHANNEL','MODULE_SET_PANID','MODULE_SET_TX_POWER','MODULE_SET_PERMIT_JOIN','MODULE_SET_DEVICE_TYPE'] },
    { emoji: '🔍', title: 'Node Discovery', functions: ['MODULE_NODE_JOIN_NOTIFY','MODULE_NODE_LEAVE_NOTIFY','MODULE_NODE_ANNOUNCE_NOTIFY','MODULE_QUERY_SHORT_ADDR','MODULE_QUERY_NODE_PORT_INFO','MODULE_QUERY_IEEE_ADDR','MODULE_DELETE_NODE','MODULE_AUTO_FIND_TARGET'] },
    { emoji: '⚡', title: 'ZCL Control', functions: ['MODULE_ZCL_READ_ATTR','MODULE_ZCL_WRITE_ATTR','MODULE_ZCL_SEND_CONTROL_CMD','MODULE_ZCL_RECV_CONTROL_CMD','MODULE_ZCL_RECV_ATTR_REPORT','MODULE_ZCL_SET_REPORT_RULE','MODULE_ZCL_DISCOVER_ATTR','MODULE_ZCL_IDENTIFY','MODULE_ZCL_BIND','MODULE_ZCL_UNBIND','MODULE_ZCL_GET_BIND_TABLE'] },
    { emoji: '📨', title: 'Data Transfer', functions: ['MODULE_SEND_UNICAST','MODULE_SEND_BROADCAST','MODULE_SEND_MULTICAST','MODULE_ENTER_TRANSPARENT_MODE','MODULE_SET_DEST_ADDR','MODULE_SET_DEST_EP'] },
    { emoji: '💤', title: 'Power Management', functions: ['MODULE_SET_LP_LEVEL','MODULE_ENTER_SLEEP','MODULE_WAKEUP'] },
  ],
  RS485: [
    { emoji: '🔌', title: 'GPIO Mode', functions: ['RS485_SEND_MODE', 'RS485_RECEIVE_MODE'] },
  ],
};

// ── Preset Definitions ────────────────────────────────────────────────────────

export const PRESETS = {
  BLE: [
    { label: 'BLE (STM32WB55)', module_id: '002', module_name: 'STM32WB_BLE_Gateway' },
    { label: 'BLE (Custom)',     module_id: '004', module_name: 'Custom_BLE_Module' },
  ],
  LORA: [
    { label: 'LoRa (RAK3172)',    module_id: '003', module_name: 'RAK3172_LoRa' },
    { label: 'LoRa (Wio-E5 mini)', module_id: '006', module_name: 'WioE5_LoRa' },
  ],
  ZIGBEE: [
    { label: 'Zigbee (E180-ZG120B)', module_id: '001', module_name: 'E180_Zigbee_Gateway' },
    { label: 'Zigbee (STM32WB55)',   module_id: '005', module_name: 'STM32WB_Zigbee_Gateway' },
  ],
  RS485: [
    { label: 'RS485 Module', module_id: '007', module_name: 'RS485_GPIO_Module' },
  ],
};

// ── Default Config Templates ──────────────────────────────────────────────────

function defaultFn(name) {
  return {
    function_name: name, command: '', is_prefix: false,
    gpio_start_control: [], delay_start: 0,
    expect_response: '', timeout: 0,
    gpio_end_control: [], delay_end: 0,
  };
}

function defaultCfg(id, type, name, fns) {
  return {
    module_id: id, module_type: type, module_name: name,
    module_communication: { port_type: 'uart', parameters: { baudrate: 115200, parity: 'none', stopbit: 1 } },
    functions: fns.map(defaultFn),
  };
}

/** Return a deep-cloned default config for a given stack ID / module type. */
export function getDefaultConfig(moduleType, presetIndex = 0) {
  const preset = (PRESETS[moduleType] || [])[presetIndex];
  if (!preset) return null;

  // RS485 uses GPIO-only functions (no AT command fields, no module_communication)
  if (moduleType === 'RS485') {
    return structuredClone({
      module_id: preset.module_id,
      module_type: 'RS485',
      stack_id: 0,
      functions: [
        {
          function_name: 'RS485_SEND_MODE',
          gpio_start_control: [{ pin: '03', state: 'HIGH' }, { pin: '02', state: 'HIGH' }],
          delay_start: 1,
          gpio_end_control: [],
          delay_end: 0,
        },
        {
          function_name: 'RS485_RECEIVE_MODE',
          gpio_start_control: [{ pin: '03', state: 'LOW' }, { pin: '02', state: 'LOW' }],
          delay_start: 1,
          gpio_end_control: [],
          delay_end: 0,
        },
      ],
    });
  }

  const allFns = (FUNCTION_GROUPS[moduleType] || []).flatMap(g => g.functions);
  return structuredClone(defaultCfg(preset.module_id, moduleType, preset.module_name, allFns));
}

// ── BLE Quick-control commands (basic tab) ────────────────────────────────────

export const BLE_QUICK_COMMANDS = [
  { label: 'SW Reset',    fn: 'MODULE_SW_RESET' },
  { label: 'Get Info',    fn: 'MODULE_GET_INFO' },
  { label: 'Get Status',  fn: 'MODULE_GET_CONNECTION_STATUS' },
  { label: 'Scan',        fn: 'MODULE_START_DISCOVERY' },
  { label: 'Stop Scan',   fn: 'MODULE_FACTORY_RESET' },
];
