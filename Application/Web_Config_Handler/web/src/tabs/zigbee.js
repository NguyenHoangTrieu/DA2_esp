/**
 * zigbee.js — Advanced Zigbee tab
 *
 * Renders the full two-pane JSON Config Builder for Zigbee modules
 * using the shared ConfigForm component. Zigbee gets extra fields
 * (cmd_type, cmd_code, resp_format, async badge) handled inside config-form.js.
 */

import { renderConfigForm } from '../config-form.js';

export function renderZigbee(container, config) {
  renderConfigForm(container, config, 'ZIGBEE');
}
