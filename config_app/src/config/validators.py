"""
Input validators for ESP32 Gateway Configuration Tool
"""

import re
from typing import Tuple, Optional


class Validators:
    """Collection of validation functions"""
    
    @staticmethod
    def validate_wifi_ssid(ssid: str) -> Tuple[bool, str]:
        """Validate WiFi SSID"""
        if not ssid:
            return False, "SSID không được để trống"
        if len(ssid) > 32:
            return False, "SSID tối đa 32 ký tự"
        return True, ""
    
    @staticmethod
    def validate_wifi_password(password: str) -> Tuple[bool, str]:
        """Validate WiFi password"""
        if password and len(password) < 8:
            return False, "Password tối thiểu 8 ký tự (WPA2)"
        if len(password) > 64:
            return False, "Password tối đa 64 ký tự"
        return True, ""
    
    @staticmethod
    def validate_mqtt_broker(broker: str) -> Tuple[bool, str]:
        """Validate MQTT broker URL"""
        if not broker:
            return False, "MQTT Broker không được để trống"
        pattern = r'^(mqtt|mqtts|tcp|ssl)://[a-zA-Z0-9.-]+:\d+$'
        if not re.match(pattern, broker):
            return False, "Định dạng: mqtt://hostname:port"
        return True, ""
    
    @staticmethod
    def validate_hex_byte(value: str) -> Tuple[bool, str]:
        """Validate hex byte value (0x00-0xFF)"""
        try:
            if value.startswith("0x") or value.startswith("0X"):
                val = int(value, 16)
            else:
                val = int(value)
            if 0 <= val <= 0xFF:
                return True, ""
            return False, "Giá trị phải trong khoảng 0x00-0xFF"
        except ValueError:
            return False, "Giá trị hex không hợp lệ"
    
    @staticmethod
    def validate_hex_word(value: str) -> Tuple[bool, str]:
        """Validate hex word value (0x0000-0xFFFF)"""
        try:
            if value.startswith("0x") or value.startswith("0X"):
                val = int(value, 16)
            else:
                val = int(value)
            if 0 <= val <= 0xFFFF:
                return True, ""
            return False, "Giá trị phải trong khoảng 0x0000-0xFFFF"
        except ValueError:
            return False, "Giá trị hex không hợp lệ"
    
    @staticmethod
    def validate_can_id(value: str) -> Tuple[bool, str]:
        """Validate CAN ID (standard: 0x000-0x7FF)"""
        try:
            if value.startswith("0x") or value.startswith("0X"):
                val = int(value, 16)
            else:
                val = int(value)
            if 0 <= val <= 0x7FF:
                return True, ""
            return False, "CAN ID phải trong khoảng 0x000-0x7FF"
        except ValueError:
            return False, "Giá trị CAN ID không hợp lệ"
    
    @staticmethod
    def validate_channel(value: str) -> Tuple[bool, str]:
        """Validate LoRa channel (0-31)"""
        try:
            val = int(value)
            if 0 <= val <= 31:
                return True, ""
            return False, "Channel phải trong khoảng 0-31"
        except ValueError:
            return False, "Giá trị channel không hợp lệ"
    
    @staticmethod
    def validate_number_range(value: str, min_val: int, max_val: int) -> Tuple[bool, str]:
        """Validate number in range"""
        try:
            val = int(value)
            if min_val <= val <= max_val:
                return True, ""
            return False, f"Giá trị phải trong khoảng {min_val}-{max_val}"
        except ValueError:
            return False, "Giá trị không hợp lệ"
    
    @staticmethod
    def validate_baud_rate(value: str, allowed: list) -> Tuple[bool, str]:
        """Validate baud rate"""
        try:
            val = int(value)
            if val in allowed:
                return True, ""
            return False, f"Baud rate phải là một trong: {allowed}"
        except ValueError:
            return False, "Baud rate không hợp lệ"


def parse_hex(value: str) -> int:
    """Parse hex string to int"""
    if value.startswith("0x") or value.startswith("0X"):
        return int(value, 16)
    return int(value)


def format_hex_byte(value: int) -> str:
    """Format int to hex byte string"""
    return f"0x{value:02X}"


def format_hex_word(value: int) -> str:
    """Format int to hex word string"""
    return f"0x{value:04X}"
