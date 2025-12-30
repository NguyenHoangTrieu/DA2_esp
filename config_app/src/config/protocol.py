"""
Protocol definitions for ESP32 Gateway Configuration Tool
Handles CFSC command/response protocol
"""

from dataclasses import dataclass, field
from typing import Dict, Optional, List
from enum import Enum


class InternetType(Enum):
    WIFI = "WIFI"
    LTE = "LTE"
    UNKNOWN = "UNKNOWN"


class ServerType(Enum):
    MQTT = "MQTT"
    HTTP = "HTTP"
    UNKNOWN = "UNKNOWN"


class CanMode(Enum):
    NORMAL = "NORMAL"
    LOOPBACK = "LOOPBACK"
    SILENT = "SILENT"


class LoraRole(Enum):
    GATEWAY = "GATEWAY"
    NODE = "NODE"


class StackType(Enum):
    NONE = "NONE"
    LORA = "LORA"
    RS485 = "RS485"
    ZIGBEE = "ZIGBEE"
    CAN = "CAN"


# Protocol constants
CFSC_COMMAND = "CFSC"
CFSC_RESP_START = "CFSC_RESP:START"
CFSC_RESP_END = "CFSC_RESP:END"

SECTION_GATEWAY_INFO = "[GATEWAY_INFO]"
SECTION_WAN_CONFIG = "[WAN_CONFIG]"
SECTION_LAN_CONFIG = "[LAN_CONFIG]"

PASSWORD_HIDDEN = "***HIDDEN***"
UNAVAILABLE = "UNAVAILABLE"


@dataclass
class GatewayInfo:
    """Read-only gateway information"""
    model: str = ""
    firmware: str = ""
    hardware: str = ""
    serial: str = ""
    internet_status: str = ""
    rtc_time: str = ""


@dataclass
class WanConfig:
    """WAN/Internet configuration"""
    internet_type: str = "WIFI"
    wifi_ssid: str = ""
    wifi_password: str = ""
    wifi_username: str = ""
    wifi_auth_mode: int = 0  # 0=PERSONAL, 1=ENTERPRISE
    lte_apn: str = ""
    lte_username: str = ""
    lte_password: str = ""
    lte_comm_type: str = "UART"
    lte_max_retries: int = 5
    lte_timeout_ms: int = 30000
    lte_auto_reconnect: str = "false"
    server_type: str = "MQTT"
    mqtt_broker: str = ""
    mqtt_device_token: str = ""
    mqtt_pub_topic: str = ""
    mqtt_sub_topic: str = ""
    mqtt_attribute_topic: str = ""


@dataclass
class LoraConfig:
    """LoRa configuration"""
    e32_addh: int = 0x00
    e32_addl: int = 0x00
    e32_sped: int = 0x1A
    e32_chan: int = 23
    e32_option: int = 0x44
    e32_baud: int = 9600  # Read-only
    e32_header: int = 0xC0  # Read-only
    role: str = "GATEWAY"
    node_id: int = 0x0001
    gateway_id: int = 0x0001
    num_slots: int = 8
    my_slot: int = 0
    slot_duration_ms: int = 200
    crypto_key_len: int = 0


@dataclass
class CanConfig:
    """CAN Bus configuration"""
    baud_rate: int = 500000
    mode: str = "NORMAL"
    whitelist_count: int = 0  # Read-only
    whitelist: str = ""  # Read-only, comma-separated


@dataclass
class Rs485Config:
    """RS485 configuration"""
    baud_rate: int = 115200


@dataclass
class StackConfig:
    """Stack configuration"""
    stack_1_type: str = "NONE"
    stack_2_type: str = "NONE"


@dataclass
class LanConfig:
    """LAN configuration - contains all LAN-side configs"""
    lora: LoraConfig = field(default_factory=LoraConfig)
    can: CanConfig = field(default_factory=CanConfig)
    rs485: Rs485Config = field(default_factory=Rs485Config)
    stack: StackConfig = field(default_factory=StackConfig)


@dataclass
class GatewayConfig:
    """Complete gateway configuration"""
    gateway_info: GatewayInfo = field(default_factory=GatewayInfo)
    wan: WanConfig = field(default_factory=WanConfig)
    lan: LanConfig = field(default_factory=LanConfig)


class ConfigParser:
    """Parser for CFSC response data"""
    
    @staticmethod
    def parse_response(data: str) -> Optional[GatewayConfig]:
        """Parse CFSC response string into GatewayConfig object"""
        if CFSC_RESP_START not in data or CFSC_RESP_END not in data:
            return None
        
        config = GatewayConfig()
        current_section = None
        
        lines = data.split('\n')
        for line in lines:
            line = line.strip()
            if not line:
                continue
            
            # Check for section headers
            if line == SECTION_GATEWAY_INFO:
                current_section = "gateway_info"
                continue
            elif line == SECTION_WAN_CONFIG:
                current_section = "wan"
                continue
            elif line == SECTION_LAN_CONFIG:
                current_section = "lan"
                continue
            elif line in [CFSC_RESP_START, CFSC_RESP_END]:
                continue
            
            # Parse key=value pairs
            if '=' in line:
                key, value = line.split('=', 1)
                key = key.strip()
                value = value.strip()
                
                ConfigParser._set_config_value(config, current_section, key, value)
        
        return config
    
    @staticmethod
    def _set_config_value(config: GatewayConfig, section: str, key: str, value: str):
        """Set a config value based on section and key"""
        try:
            if section == "gateway_info":
                if hasattr(config.gateway_info, key):
                    setattr(config.gateway_info, key, value)
            
            elif section == "wan":
                if key == "wifi_auth_mode":
                    config.wan.wifi_auth_mode = int(value)
                elif key == "lte_max_retries":
                    config.wan.lte_max_retries = int(value)
                elif key == "lte_timeout_ms":
                    config.wan.lte_timeout_ms = int(value)
                elif hasattr(config.wan, key):
                    setattr(config.wan, key, value)
            
            elif section == "lan":
                # LoRa configs
                if key.startswith("lora_e32_"):
                    attr = key.replace("lora_e32_", "e32_")
                    if attr in ["e32_addh", "e32_addl", "e32_sped", "e32_option", "e32_header"]:
                        setattr(config.lan.lora, attr, int(value, 16) if value.startswith("0x") else int(value))
                    elif attr in ["e32_chan", "e32_baud"]:
                        setattr(config.lan.lora, attr, int(value))
                elif key.startswith("lora_"):
                    attr = key.replace("lora_", "")
                    if attr in ["node_id", "gateway_id"]:
                        setattr(config.lan.lora, attr, int(value, 16) if value.startswith("0x") else int(value))
                    elif attr in ["num_slots", "my_slot", "slot_duration_ms", "crypto_key_len"]:
                        setattr(config.lan.lora, attr, int(value))
                    elif hasattr(config.lan.lora, attr):
                        setattr(config.lan.lora, attr, value)
                
                # CAN configs
                elif key.startswith("can_"):
                    attr = key.replace("can_", "")
                    if attr == "baud_rate":
                        config.lan.can.baud_rate = int(value)
                    elif attr == "whitelist_count":
                        config.lan.can.whitelist_count = int(value)
                    elif hasattr(config.lan.can, attr):
                        setattr(config.lan.can, attr, value)
                
                # RS485 configs
                elif key.startswith("rs485_"):
                    attr = key.replace("rs485_", "")
                    if attr == "baud_rate":
                        config.lan.rs485.baud_rate = int(value)
                
                # Stack configs
                elif key.startswith("stack_"):
                    if hasattr(config.lan.stack, key):
                        setattr(config.lan.stack, key, value)
        except (ValueError, AttributeError) as e:
            print(f"Warning: Failed to parse {key}={value}: {e}")
    
    @staticmethod
    def build_wan_config_command(wan: WanConfig) -> str:
        """Build CF command for WAN config"""
        lines = []
        lines.append(f"CF:internet_type={wan.internet_type}")
        lines.append(f"CF:wifi_ssid={wan.wifi_ssid}")
        if wan.wifi_password and wan.wifi_password != PASSWORD_HIDDEN:
            lines.append(f"CF:wifi_password={wan.wifi_password}")
        lines.append(f"CF:wifi_username={wan.wifi_username}")
        lines.append(f"CF:wifi_auth_mode={wan.wifi_auth_mode}")
        lines.append(f"CF:lte_apn={wan.lte_apn}")
        lines.append(f"CF:lte_username={wan.lte_username}")
        if wan.lte_password and wan.lte_password != PASSWORD_HIDDEN:
            lines.append(f"CF:lte_password={wan.lte_password}")
        lines.append(f"CF:lte_comm_type={wan.lte_comm_type}")
        lines.append(f"CF:lte_max_retries={wan.lte_max_retries}")
        lines.append(f"CF:lte_timeout_ms={wan.lte_timeout_ms}")
        lines.append(f"CF:lte_auto_reconnect={wan.lte_auto_reconnect}")
        lines.append(f"CF:server_type={wan.server_type}")
        lines.append(f"CF:mqtt_broker={wan.mqtt_broker}")
        if wan.mqtt_device_token and wan.mqtt_device_token != PASSWORD_HIDDEN:
            lines.append(f"CF:mqtt_device_token={wan.mqtt_device_token}")
        lines.append(f"CF:mqtt_pub_topic={wan.mqtt_pub_topic}")
        lines.append(f"CF:mqtt_sub_topic={wan.mqtt_sub_topic}")
        lines.append(f"CF:mqtt_attribute_topic={wan.mqtt_attribute_topic}")
        return '\r\n'.join(lines)
    
    @staticmethod
    def build_lan_config_command(lan: LanConfig) -> str:
        """Build CF command for LAN config"""
        lines = []
        
        # LoRa configs
        lines.append(f"CF:lora_e32_addh=0x{lan.lora.e32_addh:02X}")
        lines.append(f"CF:lora_e32_addl=0x{lan.lora.e32_addl:02X}")
        lines.append(f"CF:lora_e32_sped=0x{lan.lora.e32_sped:02X}")
        lines.append(f"CF:lora_e32_chan={lan.lora.e32_chan}")
        lines.append(f"CF:lora_e32_option=0x{lan.lora.e32_option:02X}")
        lines.append(f"CF:lora_role={lan.lora.role}")
        lines.append(f"CF:lora_node_id=0x{lan.lora.node_id:04X}")
        lines.append(f"CF:lora_gateway_id=0x{lan.lora.gateway_id:04X}")
        lines.append(f"CF:lora_num_slots={lan.lora.num_slots}")
        lines.append(f"CF:lora_my_slot={lan.lora.my_slot}")
        lines.append(f"CF:lora_slot_duration_ms={lan.lora.slot_duration_ms}")
        lines.append(f"CF:lora_crypto_key_len={lan.lora.crypto_key_len}")
        
        # CAN configs
        lines.append(f"CF:can_baud_rate={lan.can.baud_rate}")
        lines.append(f"CF:can_mode={lan.can.mode}")
        
        # RS485 configs
        lines.append(f"CF:rs485_baud_rate={lan.rs485.baud_rate}")
        
        # Stack configs
        lines.append(f"CF:stack_1_type={lan.stack.stack_1_type}")
        lines.append(f"CF:stack_2_type={lan.stack.stack_2_type}")
        
        return '\r\n'.join(lines)
