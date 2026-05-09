/**
 * ble.js — Advanced BLE tab
 *
 * Renders the full two-pane JSON Config Builder for BLE modules
 * using the shared ConfigForm component.
 */

import { renderConfigForm } from '../config-form.js';

export function renderBle(container, config) {
  renderConfigForm(container, config, 'BLE');
}
