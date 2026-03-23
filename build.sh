#!/bin/bash
set -e  # Exit on first error

# 1. Export ESP-IDF environment variables (ensure correct version and toolchain)
echo "[build.sh] Sourcing ESP-IDF environment..."
source ~/esp-idf/export.sh

# 2. (Optional) remove old build directory for clean build
# idf.py fullclean

# 3. Build the Web Config UI (Vite → single-file index.html for EMBED_TXTFILES)
#    - Requires Node.js >= 18 and dependencies installed once with: npm install
#    - Output: Application/Web_Config_Handler/web/firmware/index.html (all JS+CSS inlined)
#    - This file is picked up by CMake EMBED_TXTFILES and linked into the firmware
echo ""
echo "============================================"
echo "STEP 1: Building Web UI"
echo "============================================"
WEB_DIR="$(dirname "$0")/Application/Web_Config_Handler/web"

if [ ! -d "$WEB_DIR/node_modules" ]; then
    echo "[build.sh] Installing web dependencies (first run)..."
    npm --prefix "$WEB_DIR" install || { echo "ERROR: npm install failed"; exit 1; }
fi

echo "[build.sh] Building web UI with Vite..."
npm --prefix "$WEB_DIR" run build || { echo "ERROR: npm build failed"; exit 1; }

# Output goes to web/firmware/index.html — embedded directly by CMake EMBED_TXTFILES
# web/index.html is the permanent dev source (never overwritten)
if [ -f "$WEB_DIR/firmware/index.html" ]; then
    echo "[build.sh] ✓ Web UI build successful → $WEB_DIR/firmware/index.html"
else
    echo "ERROR: Web UI build failed - firmware/index.html not found"
    exit 1
fi

# 4. Build firmware project (includes configure, CMake, Ninja, dependencies...)
echo ""
echo "============================================"
echo "STEP 2: Building Firmware"
echo "============================================"
idf.py build || { echo "ERROR: Firmware build failed"; exit 1; }

# 5. Copy firmware to flash directory
echo ""
echo "============================================"
echo "STEP 3: Copying Firmware Binaries"
echo "============================================"
mkdir -p ../DATN_config_app/dist/bin
cp -v build/DA2_esp.bin ../DATN_config_app/dist/bin/ || { echo "WARNING: Failed to copy DA2_esp.bin"; }
cp -v build/bootloader/bootloader.bin ../DATN_config_app/dist/bin/ || { echo "WARNING: Failed to copy bootloader.bin"; }
cp -v build/partition_table/partition-table.bin ../DATN_config_app/dist/bin/ || { echo "WARNING: Failed to copy partition-table.bin"; }
echo "[build.sh] ✓ Firmware files copied"

# cp -v build/DA2_esp.bin flash_file
# cp -v build/bootloader/bootloader.bin flash_file
# cp -v build/partition_table/partition-table.bin flash_file

# 6. Build LAN version
echo ""
echo "============================================"
echo "STEP 4: Building LAN Version"
echo "============================================"
cd ../DA2_esp_LAN
./build.sh || { echo "ERROR: LAN firmware build failed"; exit 1; }

echo ""
echo "============================================"
echo "BUILD COMPLETE - All binaries ready!"
echo "============================================"