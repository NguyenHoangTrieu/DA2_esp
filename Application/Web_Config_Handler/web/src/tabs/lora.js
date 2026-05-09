/**
 * lora.js — Advanced LoRa tab
 *
 * Renders the full two-pane JSON Config Builder for LoRa modules
 * using the shared ConfigForm component.
 */

import { renderConfigForm } from '../config-form.js';

export function renderLora(container, config) {
  renderConfigForm(container, config, 'LORA');
}
