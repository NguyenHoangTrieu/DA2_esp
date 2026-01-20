#!/bin/bash

# ========================================
# ESP32-S3 Flash Script
# ========================================

PORT=$1
TARGET=$2

# Check port parameter
if [ -z "$PORT" ]; then
    echo "Error: Serial port not specified"
    echo ""
    echo "Usage:"
    echo "  ./flash.sh /dev/ttyUSBx -WAN    (Flash WAN only)"
    echo "  ./flash.sh /dev/ttyUSBx -LAN    (Flash LAN only)"
    echo "  ./flash.sh /dev/ttyUSBx         (Flash both)"
    echo ""
    exit 1
fi

# Validate target parameter
FLASH_WAN=0
FLASH_LAN=0

if [ "$TARGET" == "-WAN" ]; then
    FLASH_WAN=1
    echo "Target: WAN only"
elif [ "$TARGET" == "-LAN" ]; then
    FLASH_LAN=1
    echo "Target: LAN only"
elif [ -z "$TARGET" ]; then
    FLASH_WAN=1
    FLASH_LAN=1
    echo "Target: Both WAN and LAN"
else
    echo "Error: Invalid target '$TARGET'"
    echo "Valid targets: -WAN, -LAN, or none (for both)"
    exit 1
fi

echo "========================================"
echo "Port: $PORT"
echo "========================================"
echo ""

# Function to handle errors
check_error_wan() {
    if [ $? -ne 0 ]; then
        echo ""
        echo "ERROR: WAN flash failed"
        exit 1
    fi
}

check_error_lan() {
    if [ $? -ne 0 ]; then
        echo ""
        echo "ERROR: LAN flash failed"
        exit 1
    fi
}

# ========================================
# WAN Flash
# ========================================
if [ $FLASH_WAN -eq 1 ]; then
    echo ""
    echo "===== WAN Flash ====="
    echo ""
    
    echo "[WAN 1/4] Erasing NVS..."
    esptool --port "$PORT" erase_flash
    check_error_wan
    
    echo "[WAN 2/4] Bootloader..."
    esptool --chip esp32s3 --port "$PORT" --baud 115200 write-flash 0x0 bootloader.bin
    check_error_wan
    
    echo "[WAN 3/4] Partition table..."
    esptool --chip esp32s3 --port "$PORT" --baud 115200 write-flash 0x8000 partition-table.bin
    check_error_wan
    
    echo "[WAN 4/4] Application..."
    esptool --chip esp32s3 --port "$PORT" --baud 115200 write-flash 0x20000 DA2_esp.bin
    check_error_wan
    
    echo ""
    echo "===== WAN Flash OK ====="
    echo ""
fi

# ========================================
# LAN Flash
# ========================================
if [ $FLASH_LAN -eq 1 ]; then
    echo ""
    echo "===== LAN Flash ====="
    echo ""
    
    if [ $FLASH_WAN -eq 1 ]; then
        echo "Waiting 2 seconds..."
        sleep 2
    fi
    
    echo "[LAN 1/4] Erasing NVS..."
    esptool --port "$PORT" --before no_reset --after no_reset erase_flash
    check_error_lan
    
    echo "[LAN 2/4] Bootloader..."
    esptool --chip esp32s3 --port "$PORT" --baud 115200 --before no_reset --after no_reset write-flash 0x0 bootloader_LAN.bin
    check_error_lan
    
    echo "[LAN 3/4] Partition table..."
    esptool --chip esp32s3 --port "$PORT" --baud 115200 --before no_reset --after no_reset write-flash 0x8000 partition-table_LAN.bin
    check_error_lan
    
    echo "[LAN 4/4] Application..."
    esptool --chip esp32s3 --port "$PORT" --baud 115200 --before no_reset --after no_reset write-flash 0x110000 DA2_esp_LAN.bin
    check_error_lan
    
    echo ""
    echo "===== LAN Flash OK ====="
    echo ""
fi

echo ""
echo "========================================"
echo "SUCCESS: All operations completed"
echo "========================================"
exit 0