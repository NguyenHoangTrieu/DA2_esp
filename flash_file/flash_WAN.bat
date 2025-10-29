@echo off
REM Cách dùng: flash.bat COMX (VD: flash.bat COM5)

set PORT=%1

if "%PORT%"=="" (
    echo Usage: flash.bat COMX
    exit /b 1
)

REM -- Erase flash --
esptool --port %PORT% erase_flash

REM -- Flash bootloader --
esptool --chip esp32s3 --port %PORT% --baud 115200 write_flash 0x0 bootloader.bin

REM -- Flash partition table --
esptool --chip esp32s3 --port %PORT% --baud 115200 write_flash 0x8000 partition-table.bin

REM -- Flash application --
esptool --chip esp32s3 --port %PORT% --baud 115200 write_flash 0x20000 DA2_esp.bin
