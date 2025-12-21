@echo off
setlocal enabledelayedexpansion

REM ========================================
REM ESP32-S3 Flash Script
REM ========================================

set PORT=%1
set TARGET=%2

REM Check port parameter
if "%PORT%"=="" (
    echo Error: COM port not specified
    echo.
    echo Usage:
    echo   flash.bat COMX -WAN    ^(Flash WAN only^)
    echo   flash.bat COMX -LAN    ^(Flash LAN only^)
    echo   flash.bat COMX         ^(Flash both^)
    echo.
    exit /b 1
)

REM Validate target parameter
set FLASH_WAN=0
set FLASH_LAN=0

if "%TARGET%"=="-WAN" (
    set FLASH_WAN=1
    echo Target: WAN only
) else if "%TARGET%"=="-LAN" (
    set FLASH_LAN=1
    echo Target: LAN only
) else if "%TARGET%"=="" (
    set FLASH_WAN=1
    set FLASH_LAN=1
    echo Target: Both WAN and LAN
) else (
    echo Error: Invalid target '%TARGET%'
    echo Valid targets: -WAN, -LAN, or none ^(for both^)
    exit /b 1
)

echo ========================================
echo Port: %PORT%
echo ========================================
echo.

REM ========================================
REM WAN Flash
REM ========================================
if %FLASH_WAN%==1 (
    echo.
    echo ===== WAN Flash =====
    echo.
    
    echo [WAN 1/4] Erasing NVS...
    esptool --port %PORT% erase_region 0x9000 0x6000
    if errorlevel 1 goto error_wan
    
    echo [WAN 2/4] Bootloader...
    esptool --chip esp32s3 --port %PORT% --baud 115200 write_flash 0x0 bootloader.bin
    if errorlevel 1 goto error_wan
    
    echo [WAN 3/4] Partition table...
    esptool --chip esp32s3 --port %PORT% --baud 115200 write_flash 0x8000 partition-table.bin
    if errorlevel 1 goto error_wan
    
    echo [WAN 4/4] Application...
    esptool --chip esp32s3 --port %PORT% --baud 115200 write_flash 0x20000 DA2_esp.bin
    if errorlevel 1 goto error_wan
    
    echo.
    echo ===== WAN Flash OK =====
    echo.
)

REM ========================================
REM LAN Flash
REM ========================================
if %FLASH_LAN%==1 (
    echo.
    echo ===== LAN Flash =====
    echo.
    
    if %FLASH_WAN%==1 (
        echo Waiting 2 seconds...
        timeout /t 2 /nobreak >nul 2>&1
    )
    
    echo [LAN 1/4] Erasing NVS...
    esptool --port %PORT% --before no_reset --after no_reset erase_region 0x9000 0x100000
    if errorlevel 1 goto error_lan
    
    echo [LAN 2/4] Bootloader...
    esptool --chip esp32s3 --port %PORT% --baud 115200 --before no_reset --after no_reset write_flash 0x0 bootloader_LAN.bin
    if errorlevel 1 goto error_lan
    
    echo [LAN 3/4] Partition table...
    esptool --chip esp32s3 --port %PORT% --baud 115200 --before no_reset --after no_reset write_flash 0x8000 partition-table_LAN.bin
    if errorlevel 1 goto error_lan
    
    echo [LAN 4/4] Application...
    esptool --chip esp32s3 --port %PORT% --baud 115200 --before no_reset --after no_reset write_flash 0x110000 DA2_esp_LAN.bin
    if errorlevel 1 goto error_lan
    
    echo.
    echo ===== LAN Flash OK =====
    echo.
)

echo.
echo ========================================
echo SUCCESS: All operations completed
echo ========================================
endlocal
exit /b 0

:error_wan
echo.
echo ERROR: WAN flash failed
endlocal
exit /b 1

:error_lan
echo.
echo ERROR: LAN flash failed
endlocal
exit /b 1