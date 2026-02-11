# Gateway Config Tool - Build Instructions

## Quick Build

```bash
# 1. Install dependencies
cd c:\embedded\DATN_Workspace\DA2_esp\config_app
pip install -r requirements.txt
pip install pyinstaller

# 2. Build executable
pyinstaller Gateway_Config_Tool_v4.spec

# 3. Output location
# dist\Gateway_Config_Tool_v4.exe
```

## Clean Build

```bash
rmdir /s /q build dist
pyinstaller Gateway_Config_Tool_v4.spec
```
