# HMI App Build and Flash Guide

This guide describes the practical flow to build the TJC HMI app used by DA2 firmware and flash it to the TJC3224K024_011 display.

## 1. Scope and source files

Current HMI assets in the workspace:

- Main firmware integration: `DA2_esp/Application/HMI_Task/`, `DA2_esp/Middleware/HMI_Display/`, `DA2_esp/BSP/hmi_handler/`
- HMI editor project folder: `HMI_Project/`
- Known project files in `HMI_Project/`:
  - `DA2_gateway.HMI`  - project name referenced by existing design docs
  - `xxx.HMI`          - newer working copy currently being edited in the TJC editor screenshot
- Font/resource files: `font_ascii_16.zi`, `arial_ascii.zi`, `xxxx.zi`

Important: the repo currently contains more than one `.HMI` file. Before releasing a `.tft`, decide which file is the canonical source and keep the flashed binary tied to that exact source file.

## 2. Firmware-side contract that the HMI app must match

The firmware currently expects the following contract:

- Display model: `TJC3224K024_011`
- Orientation: `Landscape (90 degrees)`
- UART baud rate: `115200`
- Pages:
  - `home`
  - `pgWifi`
  - `pgLTE`
  - `pgKB`
- Touch component IDs:
  - `home.b_wifi_cfg` must be component ID `1`
  - `home.b_lte_cfg` must be component ID `2`
  - `pgWifi.b_back` must be component ID `1`
  - `pgLTE.b_back` must be component ID `1`
- Home page components required by firmware:
  - `home.b_wifi_cfg`
  - `home.b_lte_cfg`

The rest of the home page is drawn by firmware via `xstr`, `fill`, and `line` commands.
If any page name or button component ID changes, the firmware in `hmi_display.c` and `hmi_display.h` must be updated together.

## 3. Pre-build checklist in TJC USART HMI Editor

1. Open the intended project file in `HMI_Project/`.
2. Confirm the project settings:
   - Device series: `K-series`
   - Device model: `TJC3224K024_011`
   - Orientation: `Landscape`
   - Baud rate: `115200`
3. Ensure at least one ASCII font resource is imported before compiling text components:
   - preferred: `font_ascii_16.zi`
4. Keep the battery bar area empty:
   - `x = 88..161`, `y = 6..17`
5. Re-check component IDs after adding or deleting buttons. TJC assigns IDs by creation order.
6. Do not add the status text fields on `home`; this panel can exceed the per-page task limit if too many components are placed on one page.

## 4. Build the HMI app into a `.tft` image

Recommended workflow in TJC Editor:

1. Open the chosen `.HMI` file.
2. Save once before compiling so the output path follows the active project.
3. Run `File -> Compile` or press `Ctrl+B`.
4. Wait for the compile to complete without missing-font or invalid-component errors.
5. Locate the generated `.tft` output in the project output directory used by the editor.

Recommended naming convention for the exported binary:

- `DA2_gateway_YYYYMMDD.tft` for baseline releases
- `xxx_YYYYMMDD_test.tft` for temporary test builds

That makes it easier to match the binary on the SD card with the exact editor project used to build it.

## 5. Flash the `.tft` image to the display

### Recommended method: microSD update

1. Format a microSD card as `FAT32`.
2. Copy exactly one `.tft` file to the root of the card.
3. Remove power from the HMI display.
4. Insert the microSD card into the display module.
5. Power the display from a stable `5V` supply.
6. Wait for the display update cycle to finish and reboot.
7. Remove the microSD card after the update completes.

Notes:

- Do not leave multiple `.tft` files on the card root.
- Do not power the TJC module from a weak `3.3V` GPIO rail during flashing.
- If the update does not start, rebuild the `.tft`, reformat the card, and retry with only one file present.

### Optional method: direct serial download from the editor

Use this only when the display is connected to a suitable USB-UART path and powered correctly. For this repo, microSD is the safer default because the gateway UART path is shared through the hardware switch.

## 6. Verify against DA2 firmware after flashing

After the screen has been flashed, boot the gateway firmware and check that the HMI enters normal mode correctly.

Expected boot log pattern from the ESP32 side:

```text
I HMI_BSP: UART2 BSP init OK (TX=41 RX=42 115200 baud)
I HMI_TASK: Display init commands sent (bkcmd=0, recmod=0)
I HMI_TASK: HMI RX task created in PSRAM
I HMI_TASK: HMI mode active
I HMI_DISP: goto_page 0 (home)
```

Functional checks on the display:

1. Home page shows battery, WiFi, LTE, and ETH fields without `invalid component` behavior.
2. Pressing `WiFi` opens `pgWifi`.
3. Pressing `LTE` opens `pgLTE`.
4. Pressing `Back` returns to `home` from both detail pages.
5. Status updates continue to refresh while touch navigation still works.

## 7. Common mismatch cases

### Compile succeeds but firmware cannot drive the UI

Typical causes:

- Wrong page name
- Wrong component name
- Button IDs changed after reordering or re-creating components
- Text components left at `sta = 0`
- Wrong orientation or wrong model selected in the editor

### Touch works but the wrong page opens

Most likely cause: component IDs no longer match `hmi_display.h`.

### Firmware logs HMI active but screen stays stale

Check these first:

- UART still set to `115200`
- Display was flashed with the same project version you just edited
- The flashed project still contains the named components expected by firmware

## 8. Next cleanup recommended for this repo

The repo should eventually converge to one canonical source project file under `HMI_Project/`. Right now `DA2_gateway.HMI` and `xxx.HMI` can easily cause the wrong screen binary to be compiled and flashed.