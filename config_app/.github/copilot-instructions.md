# ESP32 Gateway Configuration Tool - AI Coding Instructions

## Project Overview
Desktop GUI application (Python/Tkinter) for configuring ESP32-based IoT gateways via serial UART. Communicates using a custom text-based protocol (CFSC commands) to read/write device configurations.

## Architecture

### Core Components
- **`main.py`** - Application entry point, `GatewayConfigApp` orchestrates all components
- **`src/serial/manager.py`** - Thread-safe serial communication with callback-based data flow
- **`src/config/protocol.py`** - Protocol definitions, dataclasses (`GatewayConfig`, `WanConfig`, `LanConfig`), and `ConfigParser`
- **`src/ui/`** - UI components split into `basic/` (4 tabs) and `advanced/` (7 tabs) modes

### Data Flow Pattern
```
User Input → UI Panel → SerialManager.send_command() → ESP32 Gateway
                                    ↓
UI Update ← ConfigParser.parse_response() ← SerialManager (read thread)
```

### Serial Protocol (CFSC)
- **Read config**: Send `CFSC` → Receive `CFSC_RESP:START`...sections...`CFSC_RESP:END`
- **Write config**: Send `CF:key=value\r\n` per parameter
- Response sections: `[GATEWAY_INFO]`, `[WAN_CONFIG]`, `[LAN_CONFIG]`
- Passwords use `***HIDDEN***` placeholder when reading

## Key Patterns

### UI Tab Structure
All config tabs follow this pattern (see [lora_tab.py](src/ui/advanced/lora_tab.py)):
```python
class SomeTab(ttk.Frame):
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def set_config(self, config: SomeConfig):
        """Populate UI from config dataclass"""
    
    def _set_config(self):
        """Send CF commands to device"""
```

### Serial Commands (Thread-Safe)
```python
# Read with timeout
response = self.serial_manager.send_command("CFSC", timeout=5.0)

# Write single parameter
self.serial_manager.send(f"CF:wifi_ssid={ssid}")
```

### Config Dataclasses
Located in [protocol.py](src/config/protocol.py). Use nested dataclasses:
- `GatewayConfig` contains `GatewayInfo`, `WanConfig`, `LanConfig`
- `LanConfig` contains `LoraConfig`, `CanConfig`, `Rs485Config`, `StackConfig`

### Validation
Use validators from [validators.py](src/config/validators.py):
```python
from src.config.validators import Validators, format_hex_byte, parse_hex
valid, error_msg = Validators.validate_hex_byte(value)
```

### Hex Value Formatting
- Display: `format_hex_byte(value)` → `"0x1A"`, `format_hex_word(value)` → `"0x0001"`
- Parse: `parse_hex("0x1A")` → `26`

## Development Commands

### Run Application
```bash
python main.py
```

### Build Executable (PyInstaller)
```bash
pyinstaller Gateway_Config_Tool_v4.spec
# Output: dist/Gateway_Config_Tool_v4.exe
```

### Dependencies
```bash
pip install -r requirements.txt
# Core: pyserial, ttkthemes, pillow
```

## Important Conventions

1. **Logging**: Use `self.log(message, level)` where level is `INFO|SUCCESS|WARNING|ERROR|DEBUG`
2. **Thread Safety**: UI updates from serial thread must use `self.root.after(0, callback)`
3. **UI Sizing**: Use `fill=tk.X, anchor="nw"` for panels (avoid whitespace expansion)
4. **Config Keys**: LAN config keys are prefixed (`lora_`, `can_`, `rs485_`, `stack_`)
5. **Password Handling**: Skip sending if value equals `PASSWORD_HIDDEN` constant

## Adding New Configuration Parameters

1. Add field to appropriate dataclass in [protocol.py](src/config/protocol.py)
2. Update `ConfigParser._set_config_value()` for parsing
3. Update `ConfigParser.build_*_command()` for serialization
4. Add UI widget in relevant tab file
5. Wire up in `set_config()` and `_set_config()` methods
