#!/usr/bin/env bash
set -euo pipefail

# ========================================
# ESP32-S3 Flash Script (Linux)
# ========================================
# Usage:
#   ./flash_WAN.sh /dev/ttyUSB0 -WAN   # Flash WAN only
#   ./flash_WAN.sh /dev/ttyUSB0 -LAN   # Flash LAN only
#   ./flash_WAN.sh /dev/ttyUSB0        # Flash both WAN and LAN
# ========================================

PORT=${1:-}
TARGET=${2:-}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
# Workspace root is three levels up from dist/bin
WS_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VENV_BIN="$WS_ROOT/.venv/bin"

# Resolve esptool command (prefer 'esptool', then venv fallback)
resolve_esptool() {
  # Prefer workspace venv 'esptool' command
  if [[ -x "$VENV_BIN/esptool" ]]; then
    echo "$VENV_BIN/esptool"
    return
  fi
  # Then look in PATH for 'esptool' (not deprecated .py version)
  if command -v esptool >/dev/null 2>&1; then
    echo "esptool"
    return
  fi
  # Fallback to .py if needed (deprecated but may work)
  if [[ -x "$VENV_BIN/esptool.py" ]]; then
    echo "$VENV_BIN/esptool.py"
    return
  fi
  echo ""  # none
}

ESPN=esp32s3
BAUD=115200
ESP_CMD=$(resolve_esptool)

if [[ -z "$PORT" ]]; then
  echo "Error: serial port not specified"
  echo
  echo "Usage:"
  echo "  ./flash_WAN.sh /dev/ttyUSB0 -WAN   (Flash WAN only)"
  echo "  ./flash_WAN.sh /dev/ttyUSB0 -LAN   (Flash LAN only)"
  echo "  ./flash_WAN.sh /dev/ttyUSB0        (Flash both)"
  exit 1
fi

# Check port accessibility
if [[ ! -e "$PORT" ]]; then
  echo "Error: serial port '$PORT' not found"
  echo "Available ports:"
  ls -la /dev/tty* 2>/dev/null | grep -E 'ttyUSB|ttyACM' || echo "  (no USB serial devices found)"
  exit 1
fi

if [[ ! -r "$PORT" ]] || [[ ! -w "$PORT" ]]; then
  echo "Error: serial port '$PORT' not readable/writable (permission denied)"
  echo
  echo "Fix with one of:"
  echo "  1. Add user to dialout group: sudo usermod -aG dialout \$USER"
  echo "  2. Run with sudo: sudo ./flash_WAN.sh $PORT ${TARGET:-}"
  echo "  3. Fix port permissions: sudo chmod 666 $PORT"
  exit 1
fi

# Warn about port being busy
echo "⚠️  If you get 'port is busy' error, close any:"
echo "   - Serial monitor / IDE debug console"
echo "   - Previous flash attempt still holding port"
echo "   - Other terminals/tools using the port"
echo

if [[ -z "$ESP_CMD" ]]; then
  echo "Error: esptool not found"
  echo "Install with: pip install esptool"
  exit 1
fi

FLASH_WAN=0
FLASH_LAN=0
case "${TARGET}" in
  -WAN) FLASH_WAN=1 ; echo "Target: WAN only" ;;
  -LAN) FLASH_LAN=1 ; echo "Target: LAN only" ;;
  "") FLASH_WAN=1 ; FLASH_LAN=1 ; echo "Target: Both WAN and LAN" ;;
  *) echo "Error: Invalid target '${TARGET}'" ; echo "Valid targets: -WAN, -LAN, or none (for both)" ; exit 1 ;;
 esac

# Helper to run esptool with common flags
run_esptool() {
  set +e
  "$ESP_CMD" --chip "$ESPN" --port "$PORT" --baud "$BAUD" "$@"
  local rc=$?
  set -e
  return $rc
}

# Files must exist in current directory
require_file() {
  local f="$1"
  if [[ ! -f "$f" ]]; then
    echo "Error: required file not found: $f"
    exit 1
  fi
}

# Validate required artifacts exist before starting
if [[ "$FLASH_WAN" -eq 1 ]]; then
  require_file "bootloader.bin"
  require_file "partition-table.bin"
  require_file "DA2_esp.bin"
fi
if [[ "$FLASH_LAN" -eq 1 ]]; then
  require_file "bootloader_LAN.bin"
  require_file "partition-table_LAN.bin"
  require_file "DA2_esp_LAN.bin"
fi

echo "========================================"
echo "Port: $PORT"
echo "========================================"
echo

# ========================================
# WAN Flash
# ========================================
if [[ "$FLASH_WAN" -eq 1 ]]; then
  echo
  echo "===== WAN Flash ====="
  echo

  echo "[WAN 1/4] Erasing NVS..."
  run_esptool erase-flash || { echo "ERROR: WAN erase failed"; exit 1; }

  echo "[WAN 2/4] Bootloader..."
  run_esptool write-flash 0x0 bootloader.bin || { echo "ERROR: WAN bootloader flash failed"; exit 1; }

  echo "[WAN 3/4] Partition table..."
  run_esptool write-flash 0x8000 partition-table.bin || { echo "ERROR: WAN partition table flash failed"; exit 1; }

  echo "[WAN 4/4] Application..."
  run_esptool write-flash 0x20000 DA2_esp.bin || { echo "ERROR: WAN application flash failed"; exit 1; }

  echo
  echo "===== WAN Flash OK ====="
  echo
fi

# ========================================
# LAN Flash
# ========================================
if [[ "$FLASH_LAN" -eq 1 ]]; then
  echo
  echo "===== LAN Flash ====="
  echo

  if [[ "$FLASH_WAN" -eq 1 ]]; then
    echo "Waiting 2 seconds..."
    sleep 2
  fi

  echo "[LAN 1/4] Erasing NVS..."
  run_esptool --before no_reset --after no_reset erase-flash || { echo "ERROR: LAN erase failed"; exit 1; }

  echo "[LAN 2/4] Bootloader..."
  run_esptool --before no_reset --after no_reset write-flash 0x0 bootloader_LAN.bin || { echo "ERROR: LAN bootloader flash failed"; exit 1; }

  echo "[LAN 3/4] Partition table..."
  run_esptool --before no_reset --after no_reset write-flash 0x8000 partition-table_LAN.bin || { echo "ERROR: LAN partition table flash failed"; exit 1; }

  echo "[LAN 4/4] Application..."
  run_esptool --before no_reset --after no_reset write-flash 0x110000 DA2_esp_LAN.bin || { echo "ERROR: LAN application flash failed"; exit 1; }

  echo
  echo "===== LAN Flash OK ====="
  echo
fi

echo
echo "========================================"
echo "SUCCESS: All operations completed"
echo "========================================"
