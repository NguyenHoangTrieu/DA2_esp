#!/bin/bash

# 1. Export ESP-IDF environment variables (ensure correct version and toolchain)
source ~/esp-idf/export.sh

# 2. (Optional) remove old build directory for clean build
# idf.py fullclean

# 3. Build project (includes configure, CMake, Ninja, dependencies...)
idf.py build

#4. copy firmware to flash directory
cp -r build/DA2_esp.bin config_app/dist/bin
cp -r build/bootloader/bootloader.bin config_app/dist/bin
cp -r build/partition_table/partition-table.bin config_app/dist/bin

# cp -r build/DA2_esp.bin flash_file
# cp -r build/bootloader/bootloader.bin flash_file
# cp -r build/partition_table/partition-table.bin flash_file

#5. build LAN version
cd ../DA2_esp_LAN
./build.sh