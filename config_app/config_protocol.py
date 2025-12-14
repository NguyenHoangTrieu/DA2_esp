#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Gateway Configuration Protocol Module - WITH CAN WHITELIST
Contains all command definitions matching firmware exactly
"""

from typing import Dict, Optional, Tuple, List
from dataclasses import dataclass
from enum import Enum


# ===== COMMAND MAPPINGS FROM FIRMWARE (WITH WHITELIST) =====

# WAN MCU Configuration Commands
WAN_CONFIG_COMMANDS = {
    'internet_type': 'CFIN:',           # CFIN:WIFI|LTE|ETHERNET|NBIOT
    'wifi_full': 'WF:',                 # WF:SSID:PASSWORD:AUTH_MODE
    'mqtt_full': 'MQ:',                 # MQ:BROKER|TOKEN|SUB|PUB|ATTR
    'lte_full': 'LT:',                  # LT:COMM:APN:USER:PASS:...
    'server_type': 'SV:',               # SV:MQTT|HTTP|COAP
    'firmware_update': 'FW',            # FW | FW:url | FW:url:FORCE
}

# LAN MCU Configuration Commands
LAN_CONFIG_COMMANDS = {
    # CAN Configuration
    'can_baud_rate': 'CFCB:',           # CFCB:baudrate
    'can_mode': 'CFCM:',                # CFCM:NORMAL|LOOPBACK|SILENT
    
    # CAN Whitelist Management (NEW)
    'can_whitelist_add': 'CFCW:ADD:',   # CFCW:ADD:0x123 (add single ID)
    'can_whitelist_remove': 'CFCW:REM:',# CFCW:REM:0x123 (remove single ID)
    'can_whitelist_clear': 'CFCW:CLR',  # CFCW:CLR (clear all)
    'can_whitelist_set': 'CFCW:SET:',   # CFCW:SET:0x123,0x456,0x789 (set entire list)
    
    # LoRa Configuration
    'lora_modem': 'CFLR:MODEM:',        # CFLR:MODEM:<6 bytes binary>
    'lora_handler': 'CFLR:HDLCF:',      # CFLR:HDLCF:<11 bytes binary>
    'lora_crypto': 'CFLR:CRYPT:',       # CFLR:CRYPT:<key_len><key_bytes>
    
    # Firmware Update (LAN MCU)
    'firmware_update': 'CFFW',          # CFFW | CFFW:url | CFFW:url:FORCE
}


@dataclass
class WiFiConfig:
    """WiFi Configuration"""
    ssid: str
    password: str
    username: str = ""
    auth_mode: str = "PERSONAL"
    
    def to_command(self) -> str:
        if self.username:
            return f"WF:{self.ssid}:{self.password}:{self.username}:{self.auth_mode}"
        else:
            return f"WF:{self.ssid}:{self.password}:{self.auth_mode}"


@dataclass
class LTEConfig:
    """LTE Configuration"""
    comm_type: str
    apn: str
    username: str = ""
    password: str = ""
    auto_reconnect: bool = False
    reconnect_timeout_ms: int = 30000
    max_reconnect_attempts: int = 0
    
    def to_command(self) -> str:
        reconnect_str = "true" if self.auto_reconnect else "false"
        return (f"LT:{self.comm_type}:{self.apn}:{self.username}:{self.password}:"
                f"{reconnect_str}:{self.reconnect_timeout_ms}:{self.max_reconnect_attempts}")


@dataclass
class MQTTConfig:
    """MQTT Configuration"""
    broker_uri: str
    device_token: str
    subscribe_topic: str
    publish_topic: str
    attribute_topic: str
    
    def to_command(self) -> str:
        return (f"MQ:{self.broker_uri}|{self.device_token}|{self.subscribe_topic}|"
                f"{self.publish_topic}|{self.attribute_topic}")


@dataclass
class CANConfig:
    """CAN Bus Configuration"""
    baud_rate: int
    mode: str
    
    def to_commands(self) -> list:
        return [
            f"CFCB:{self.baud_rate}",
            f"CFCM:{self.mode}"
        ]


@dataclass
class CANWhitelist:
    """CAN Whitelist Management"""
    ids: List[int]  # List of CAN IDs (0x000 - 0x7FF for standard, 0x00000000 - 0x1FFFFFFF for extended)
    
    def to_add_commands(self) -> List[str]:
        """Generate ADD commands for each ID"""
        return [f"CFCW:ADD:{id:#05X}" for id in self.ids]
    
    def to_remove_commands(self) -> List[str]:
        """Generate REMOVE commands for each ID"""
        return [f"CFCW:REM:{id:#05X}" for id in self.ids]
    
    def to_set_command(self) -> str:
        """Generate SET command with comma-separated IDs"""
        id_list = ",".join([f"{id:#05X}" for id in self.ids])
        return f"CFCW:SET:{id_list}"
    
    @staticmethod
    def to_clear_command() -> str:
        """Generate CLEAR command"""
        return "CFCW:CLR"


@dataclass
class LoRaModemConfig:
    """LoRa E32 Modem Configuration (6 bytes)"""
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
class LoRaTDMAConfig:
    """LoRa TDMA Handler Configuration (11 bytes)"""
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
        return f"CFIN:{internet_type}"
    
    @staticmethod
    def build_wifi_config(wifi: WiFiConfig) -> str:
        return wifi.to_command()
    
    @staticmethod
    def build_lte_config(lte: LTEConfig) -> str:
        return lte.to_command()
    
    @staticmethod
    def build_mqtt_config(mqtt: MQTTConfig) -> str:
        return mqtt.to_command()
    
    @staticmethod
    def build_server_type(server_type: str) -> str:
        return f"SV:{server_type}"
    
    @staticmethod
    def build_can_config(can: CANConfig) -> list:
        return can.to_commands()
    
    # NEW: CAN Whitelist commands
    @staticmethod
    def build_can_whitelist_add(can_id: int) -> str:
        """Add single CAN ID to whitelist"""
        return f"CFCW:ADD:{can_id:#05X}"
    
    @staticmethod
    def build_can_whitelist_remove(can_id: int) -> str:
        """Remove single CAN ID from whitelist"""
        return f"CFCW:REM:{can_id:#05X}"
    
    @staticmethod
    def build_can_whitelist_clear() -> str:
        """Clear entire CAN whitelist"""
        return "CFCW:CLR"
    
    @staticmethod
    def build_can_whitelist_set(can_ids: List[int]) -> str:
        """Set entire CAN whitelist (replaces existing)"""
        id_list = ",".join([f"{can_id:#05X}" for can_id in can_ids])
        return f"CFCW:SET:{id_list}"
    
    @staticmethod
    def build_lora_modem_command(config: LoRaModemConfig) -> bytes:
        prefix = b"CFLR:MODEM:"
        return prefix + config.to_bytes()
    
    @staticmethod
    def build_lora_tdma_command(config: LoRaTDMAConfig) -> bytes:
        prefix = b"CFLR:HDLCF:"
        return prefix + config.to_bytes()
    
    @staticmethod
    def build_lora_crypto_command(key: bytes) -> bytes:
        if len(key) == 0 or len(key) > 32:
            raise ValueError("Crypto key must be 1-32 bytes")
        prefix = b"CFLR:CRYPT:"
        return prefix + bytes([len(key)]) + key
    
    @staticmethod
    def build_firmware_update_wan(url: Optional[str] = None, force: bool = False) -> str:
        if url is None:
            return "FW"
        elif force:
            return f"FW:{url}:FORCE"
        else:
            return f"FW:{url}"
    
    @staticmethod
    def build_firmware_update_lan(url: Optional[str] = None, force: bool = False) -> str:
        if url is None:
            return "CFFW"
        elif force:
            return f"CFFW:{url}:FORCE"
        else:
            return f"CFFW:{url}"


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


# Command format documentation
COMMAND_FORMAT_DOCS = {
    'CFIN': 'Internet Type: CFIN:WIFI|LTE|ETHERNET|NBIOT',
    'WF': 'WiFi Config: WF:SSID:PASSWORD:AUTH_MODE or WF:SSID:PASSWORD:USERNAME:AUTH_MODE',
    'LT': 'LTE Config: LT:COMM_TYPE:APN:USER:PASS:AUTO_RECONNECT:TIMEOUT:MAX_RETRY',
    'MQ': 'MQTT Config: MQ:BROKER|TOKEN|SUB_TOPIC|PUB_TOPIC|ATTR_TOPIC',
    'SV': 'Server Type: SV:MQTT|HTTP|COAP',
    'FW': 'WAN Firmware: FW | FW:url | FW:url:FORCE',
    'CFCB': 'CAN Baud: CFCB:baudrate (125000/250000/500000/800000/1000000)',
    'CFCM': 'CAN Mode: CFCM:NORMAL|LOOPBACK|SILENT',
    'CFCW:ADD': 'CAN Whitelist Add: CFCW:ADD:0x123',
    'CFCW:REM': 'CAN Whitelist Remove: CFCW:REM:0x123',
    'CFCW:CLR': 'CAN Whitelist Clear: CFCW:CLR',
    'CFCW:SET': 'CAN Whitelist Set: CFCW:SET:0x123,0x456,0x789',
    'CFFW': 'LAN Firmware: CFFW | CFFW:url | CFFW:url:FORCE',
    'CFLR:MODEM': 'LoRa Modem: CFLR:MODEM:<6 bytes binary>',
    'CFLR:HDLCF': 'LoRa TDMA: CFLR:HDLCF:<11 bytes binary>',
    'CFLR:CRYPT': 'LoRa Crypto: CFLR:CRYPT:<key_len><key_bytes>',
}


def get_command_help(command: str) -> str:
    """Get help text for a specific command"""
    return COMMAND_FORMAT_DOCS.get(command, "No documentation available")
