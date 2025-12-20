#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Gateway Configuration Protocol Module
"""
from typing import Dict, Optional, Tuple, List
from dataclasses import dataclass
from enum import Enum

# ===== COMMAND MAPPINGS FROM FIRMWARE =====

# WAN MCU Configuration Commands (with CF prefix)
WAN_CONFIG_COMMANDS = {
    'internet_type': 'CFIN:',      # CFIN:WIFI|LTE|ETHERNET|NBIOT
    'wifi_full': 'CFWF:',          # CFWF:SSID:PASSWORD:AUTH_MODE
    'mqtt_full': 'CFMQ:',          # CFMQ:BROKER|TOKEN|SUB|PUB|ATTR
    'lte_full': 'CFLT:',           # CFLT:COMM:APN:USER:PASS:...
    'server_type': 'CFSV:',        # CFSV:MQTT|HTTP|COAP
    # REMOVED: 'firmware_update': 'FW' - only for debug OTA
}

# LAN MCU Configuration Commands (with CFML prefix)
LAN_CONFIG_COMMANDS = {
    # CAN Configuration
    'can_baud_rate': 'CFML:CFCB:',           # CFML:CFCB:baudrate
    'can_mode': 'CFML:CFCM:',                # CFML:CFCM:NORMAL|LOOPBACK|SILENT
    # CAN Whitelist Management
    'can_whitelist_add': 'CFML:CFCW:ADD:',   # CFML:CFCW:ADD:0x123
    'can_whitelist_remove': 'CFML:CFCW:REM:',# CFML:CFCW:REM:0x123
    'can_whitelist_clear': 'CFML:CFCW:CLR',  # CFML:CFCW:CLR
    'can_whitelist_set': 'CFML:CFCW:SET:',   # CFML:CFCW:SET:0x123,0x456,0x789
    # LoRa Configuration
    'lora_modem': 'CFML:CFLR:MODEM:',        # CFML:CFLR:MODEM:<6 bytes binary>
    'lora_handler': 'CFML:CFLR:HDLCF:',      # CFML:CFLR:HDLCF:<11 bytes binary>
    'lora_crypto': 'CFML:CFLR:CRYPT:',       # CFML:CFLR:CRYPT:<keylen><key bytes>
    'stack_type': 'CFML:CFST:',
    # REMOVED: 'firmware_update': 'CFFW' - only for debug OTA
}

@dataclass
class WiFiConfig:
    """WiFi Configuration - generates SINGLE command"""
    ssid: str
    password: str
    username: str = ""
    auth_mode: str = "PERSONAL"
    
    def to_command(self) -> str:
        """Generate SINGLE WiFi command with CF prefix"""
        if self.username:
            return f"CFWF:{self.ssid}:{self.password}:{self.username}:{self.auth_mode}"
        else:
            return f"CFWF:{self.ssid}:{self.password}:{self.auth_mode}"

@dataclass
class LTEConfig:
    """LTE Configuration - generates SINGLE command"""
    comm_type: str
    apn: str
    username: str = ""
    password: str = ""
    auto_reconnect: bool = False
    reconnect_timeout_ms: int = 30000
    max_reconnect_attempts: int = 0
    
    def to_command(self) -> str:
        """Generate SINGLE LTE command with CF prefix"""
        reconnect_str = "true" if self.auto_reconnect else "false"
        return (f"CFLT:{self.comm_type}:{self.apn}:{self.username}:{self.password}:"
                f"{reconnect_str}:{self.reconnect_timeout_ms}:{self.max_reconnect_attempts}")

@dataclass
class MQTTConfig:
    """MQTT Configuration - generates SINGLE command"""
    broker_uri: str
    device_token: str
    subscribe_topic: str
    publish_topic: str
    attribute_topic: str
    
    def to_command(self) -> str:
        """Generate SINGLE MQTT command with CF prefix"""
        return (f"CFMQ:{self.broker_uri}|{self.device_token}|{self.subscribe_topic}|"
                f"{self.publish_topic}|{self.attribute_topic}")

@dataclass
class CANConfig:
    """CAN Bus Configuration"""
    baud_rate: int
    mode: str
    
    def to_commands(self) -> list:
        """Generate CAN commands with CFML: prefix"""
        return [
            f"CFML:CFCB:{self.baud_rate}",
            f"CFML:CFCM:{self.mode}"
        ]

@dataclass
class CANWhitelist:
    """CAN Whitelist Management - with CFML prefix"""
    ids: List[int]
    
    def to_add_commands(self) -> List[str]:
        """Generate ADD commands for each ID with CFML prefix"""
        return [f"CFML:CFCW:ADD:{id:#05X}" for id in self.ids]
    
    def to_remove_commands(self) -> List[str]:
        """Generate REMOVE commands for each ID with CFML prefix"""
        return [f"CFML:CFCW:REM:{id:#05X}" for id in self.ids]
    
    def to_set_command(self) -> str:
        """Generate SET command with comma-separated IDs and CFML prefix"""
        id_list = ",".join([f"{id:#05X}" for id in self.ids])
        return f"CFML:CFCW:SET:{id_list}"
    
    @staticmethod
    def to_clear_command() -> str:
        """Generate CLEAR command with CFML prefix"""
        return "CFML:CFCW:CLR"

@dataclass
class LoRaModemConfig:
    """LoRa E32 Modem Configuration (6 bytes) - with CFML prefix"""
    head: int
    addh: int
    addl: int
    sped: int
    chan: int
    option: int
    
    def to_bytes(self) -> bytes:
        return bytes([self.head, self.addh, self.addl, self.sped, self.chan, self.option])
    
    @classmethod
    def from_bytes(cls, data: bytes) -> 'LoRaModemConfig':
        if len(data) != 6:
            raise ValueError(f"LoRa modem config requires 6 bytes, got {len(data)}")
        return cls(
            head=data[0], addh=data[1], addl=data[2],
            sped=data[3], chan=data[4], option=data[5]
        )

@dataclass
class StackTypeConfig:
    """Stack Type Configuration - with CFML prefix"""
    stack_id: int  # 0 for ST_1, 1 for ST_2
    stack_type: str  # NONE, LORA, RS485, ZIGBEE, CAN
    
    def to_command(self) -> str:
        """Generate stack type command with CFML prefix
        
        Format: CFML:CFST:ST_1:LORA or CFML:CFST:ST_2:RS485
        """
        stack_name = f"ST_{self.stack_id + 1}"
        return f"CFML:CFST:{stack_name}:{self.stack_type.upper()}"
    
    @staticmethod
    def validate_stack_type(stack_type: str) -> bool:
        """Validate stack type value"""
        valid_types = ['NONE', 'LORA', 'RS485', 'ZIGBEE', 'CAN']
        return stack_type.upper() in valid_types

@dataclass
class LoRaTDMAConfig:
    """LoRa TDMA Handler Configuration (11 bytes) - with CFML prefix"""
    role: int
    node_id: int
    gateway_id: int
    num_slots: int
    my_slot: int
    slot_duration_ms: int
    
    def to_bytes(self) -> bytes:
        data = bytearray(11)
        data[0] = self.role
        data[1] = (self.node_id >> 8) & 0xFF
        data[2] = self.node_id & 0xFF
        data[3] = (self.gateway_id >> 8) & 0xFF
        data[4] = self.gateway_id & 0xFF
        data[5] = self.num_slots
        data[6] = self.my_slot
        data[7] = (self.slot_duration_ms >> 24) & 0xFF
        data[8] = (self.slot_duration_ms >> 16) & 0xFF
        data[9] = (self.slot_duration_ms >> 8) & 0xFF
        data[10] = self.slot_duration_ms & 0xFF
        return bytes(data)
    
    @classmethod
    def from_bytes(cls, data: bytes) -> 'LoRaTDMAConfig':
        if len(data) != 11:
            raise ValueError(f"LoRa TDMA config requires 11 bytes, got {len(data)}")
        return cls(
            role=data[0],
            node_id=(data[1] << 8) | data[2],
            gateway_id=(data[3] << 8) | data[4],
            num_slots=data[5],
            my_slot=data[6],
            slot_duration_ms=(data[7] << 24) | (data[8] << 16) | (data[9] << 8) | data[10]
        )

class ConfigCommandBuilder:
    """Builds configuration commands according to firmware protocol"""
    
    @staticmethod
    def build_internet_type(internet_type: str) -> str:
        """Build internet type command with CF prefix"""
        return f"CFIN:{internet_type}"
    
    @staticmethod
    def build_wifi_config(wifi: WiFiConfig) -> str:
        """Build SINGLE WiFi command (CFWF:SSID:PASSWORD:AUTH)"""
        return wifi.to_command()
    
    @staticmethod
    def build_lte_config(lte: LTEConfig) -> str:
        """Build SINGLE LTE command (CFLT:...)"""
        return lte.to_command()
    
    @staticmethod
    def build_mqtt_config(mqtt: MQTTConfig) -> str:
        """Build SINGLE MQTT command (CFMQ:...)"""
        return mqtt.to_command()
    
    @staticmethod
    def build_server_type(server_type: str) -> str:
        """Build server type command with CF prefix"""
        return f"CFSV:{server_type}"
    
    @staticmethod
    def build_can_config(can: CANConfig) -> list:
        """Build CAN commands with CFML: prefix"""
        return can.to_commands()
    
    # CAN Whitelist commands (with CFML prefix)
    @staticmethod
    def build_can_whitelist_add(can_id: int) -> str:
        """Add single CAN ID to whitelist with CFML prefix"""
        return f"CFML:CFCW:ADD:{can_id:#05X}"
    
    @staticmethod
    def build_can_whitelist_remove(can_id: int) -> str:
        """Remove single CAN ID from whitelist with CFML prefix"""
        return f"CFML:CFCW:REM:{can_id:#05X}"
    
    @staticmethod
    def build_can_whitelist_clear() -> str:
        """Clear entire CAN whitelist with CFML prefix"""
        return "CFML:CFCW:CLR"
    
    @staticmethod
    def build_can_whitelist_set(can_ids: List[int]) -> str:
        """Set entire CAN whitelist with CFML prefix"""
        id_list = ",".join([f"{can_id:#05X}" for can_id in can_ids])
        return f"CFML:CFCW:SET:{id_list}"
    
    @staticmethod
    def build_lora_modem_command(config: LoRaModemConfig) -> bytes:
        """Build LoRa modem command with CFML prefix"""
        prefix = b"CFML:CFLR:MODEM:"
        return prefix + config.to_bytes()
    
    @staticmethod
    def build_lora_tdma_command(config: LoRaTDMAConfig) -> bytes:
        """Build LoRa TDMA command with CFML prefix"""
        prefix = b"CFML:CFLR:HDLCF:"
        return prefix + config.to_bytes()
    
    @staticmethod
    def build_lora_crypto_command(key: bytes) -> bytes:
        """Build LoRa crypto command with CFML prefix"""
        if len(key) == 0 or len(key) > 32:
            raise ValueError("Crypto key must be 1-32 bytes")
        prefix = b"CFML:CFLR:CRYPT:"
        return prefix + bytes([len(key)]) + key
    
    @staticmethod
    def build_stack_type(stack_id: int, stack_type: str) -> str:
        """Build stack type command with CFML prefix
        
        Args:
            stack_id: 0 for Stack 1, 1 for Stack 2
            stack_type: NONE, LORA, RS485, ZIGBEE, CAN
            
        Returns:
            Command string: CFML:CFST:ST_1:LORA
        """
        if stack_id not in [0, 1]:
            raise ValueError(f"Invalid stack_id: {stack_id}. Must be 0 or 1")
        
        if not StackTypeConfig.validate_stack_type(stack_type):
            raise ValueError(f"Invalid stack type: {stack_type}. "
                           f"Valid: NONE, LORA, RS485, ZIGBEE, CAN")
        
        config = StackTypeConfig(stack_id=stack_id, stack_type=stack_type)
        return config.to_command()

    # REMOVED: build_firmware_update_wan() and build_firmware_update_lan()
    # These are debug-only OTA commands, not for production use

class ConfigResponseParser:
    """Parses configuration responses from gateway"""
    
    @staticmethod
    def parse_config_dump(lines: list) -> Dict[str, Dict[str, str]]:
        """Parse CFSC response"""
        config_data = {
            'GATEWAY_INFO': {},
            'WAN_CONFIG': {},
            'LAN_CONFIG': {}
        }
        
        current_section = None
        for line in lines:
            line = line.strip()
            if not line:
                continue
            if line.startswith('[') and line.endswith(']'):
                current_section = line[1:-1]
                continue
            if '=' in line and current_section:
                key, value = line.split('=', 1)
                key = key.strip()
                value = value.strip()
                if current_section in config_data:
                    config_data[current_section][key] = value
        
        return config_data
    
    @staticmethod
    def parse_can_whitelist(whitelist_str: str) -> List[int]:
        """Parse CAN whitelist from string format
        Example: "0x123,0x456,0x789" -> [0x123, 0x456, 0x789]
        """
        if not whitelist_str or whitelist_str == "empty":
            return []
        
        ids = []
        for id_str in whitelist_str.split(','):
            id_str = id_str.strip()
            if id_str:
                try:
                    ids.append(int(id_str, 16))
                except ValueError:
                    pass
        return ids

class ProtocolValidator:
    """Validates configuration values"""
    
    @staticmethod
    def validate_can_baudrate(baudrate: int) -> Tuple[bool, str]:
        valid_rates = [125000, 250000, 500000, 800000, 1000000]
        if baudrate not in valid_rates:
            return False, f"Invalid CAN baud rate. Valid: {valid_rates}"
        return True, ""
    
    @staticmethod
    def validate_can_mode(mode: str) -> Tuple[bool, str]:
        valid_modes = ["NORMAL", "LOOPBACK", "SILENT"]
        if mode not in valid_modes:
            return False, f"Invalid CAN mode. Valid: {valid_modes}"
        return True, ""
    
    @staticmethod
    def validate_can_id(can_id: int, extended: bool = False) -> Tuple[bool, str]:
        """Validate CAN ID range"""
        if extended:
            if not (0 <= can_id <= 0x1FFFFFFF):
                return False, "Extended CAN ID must be 0x00000000-0x1FFFFFFF"
        else:
            if not (0 <= can_id <= 0x7FF):
                return False, "Standard CAN ID must be 0x000-0x7FF"
        return True, ""
    
    @staticmethod
    def validate_internet_type(inet_type: str) -> Tuple[bool, str]:
        valid_types = ["WIFI", "LTE", "ETHERNET", "NBIOT"]
        if inet_type not in valid_types:
            return False, f"Invalid internet type. Valid: {valid_types}"
        return True, ""
    @staticmethod
    def validate_stack_type(stack_type: str) -> Tuple[bool, str]:
        """Validate stack type"""
        valid_types = ["NONE", "LORA", "RS485", "ZIGBEE", "CAN"]
        if stack_type.upper() not in valid_types:
            return False, f"Invalid stack type. Valid: {valid_types}"
        return True, ""
    
    @staticmethod
    def validate_stack_id(stack_id: int) -> Tuple[bool, str]:
        """Validate stack ID"""
        if stack_id not in [0, 1]:
            return False, "Invalid stack ID. Must be 0 (ST_1) or 1 (ST_2)"
        return True, ""
# Command format documentation
COMMAND_FORMAT_DOCS = {
    'CFIN': 'Internet Type: CFIN:WIFI|LTE|ETHERNET|NBIOT',
    'CFWF': 'WiFi Config: CFWF:SSID:PASSWORD:AUTH_MODE or CFWF:SSID:PASSWORD:USERNAME:AUTH_MODE',
    'CFLT': 'LTE Config: CFLT:COMM_TYPE:APN:USER:PASS:AUTO_RECONNECT:TIMEOUT:MAX_RETRY',
    'CFMQ': 'MQTT Config: CFMQ:BROKER|TOKEN|SUB_TOPIC|PUB_TOPIC|ATTR_TOPIC',
    'CFSV': 'Server Type: CFSV:MQTT|HTTP|COAP',
    'CFML:CFCB': 'CAN Baud: CFML:CFCB:baudrate (125000/250000/500000/800000/1000000)',
    'CFML:CFCM': 'CAN Mode: CFML:CFCM:NORMAL|LOOPBACK|SILENT',
    'CFML:CFCW:ADD': 'CAN Whitelist Add: CFML:CFCW:ADD:0x123',
    'CFML:CFCW:REM': 'CAN Whitelist Remove: CFML:CFCW:REM:0x123',
    'CFML:CFCW:CLR': 'CAN Whitelist Clear: CFML:CFCW:CLR',
    'CFML:CFCW:SET': 'CAN Whitelist Set: CFML:CFCW:SET:0x123,0x456,0x789',
    'CFML:CFLR:MODEM': 'LoRa Modem: CFML:CFLR:MODEM:<6 bytes binary>',
    'CFML:CFLR:HDLCF': 'LoRa TDMA: CFML:CFLR:HDLCF:<11 bytes binary>',
    'CFML:CFLR:CRYPT': 'LoRa Crypto: CFML:CFLR:CRYPT:<keylen><key bytes>',
    'CFML:CFST': 'Stack Type: CFML:CFST:ST_1:LORA or CFML:CFST:ST_2:RS485 (types: NONE, LORA, RS485, ZIGBEE, CAN)',
}

def get_command_help(command: str) -> str:
    """Get help text for a specific command"""
    return COMMAND_FORMAT_DOCS.get(command, "No documentation available")
