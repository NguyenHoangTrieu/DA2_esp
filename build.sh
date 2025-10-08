#!/bin/bash

# 1. Export ESP-IDF environment variables (ensure correct version and toolchain)
source ~/esp-idf/export.sh

# 2. (Optional) remove old build directory for clean build
idf.py fullclean

# 3. Build project (includes configure, CMake, Ninja, dependencies...)
idf.py build

#4. copy firmware to flash directory
# cp -r build/esp_usb_otg.bin /mnt/c/embedded/esp_flash_folder/esp_usb_otg

# 4. (Optional) flash firmware to device
# idf.py -p /dev/ttyUSB0 flash
