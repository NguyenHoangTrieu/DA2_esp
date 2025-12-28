#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
LoRa E32 Configuration Definitions
Chứa các options cho dropdown menus
"""

from typing import Dict, List
from dataclasses import dataclass

# ===== LORA E32 MODEM CONFIGURATION OPTIONS =====

# SPED Byte breakdown
UART_PARITY_OPTIONS = {
    '8N1 (No parity)': 0b00,
    '8O1 (Odd parity)': 0b01,
    '8E1 (Even parity)': 0b10
}

UART_BAUD_RATE_OPTIONS = {
    '1200 bps': 0b000,
    '2400 bps': 0b001,
    '4800 bps': 0b010,
    '9600 bps': 0b011,
    '19200 bps': 0b100,
    '38400 bps': 0b101,
    '57600 bps': 0b110,
    '115200 bps': 0b111
}

AIR_DATA_RATE_OPTIONS = {
    '0.3k bps': 0b000,
    '1.2k bps': 0b001,
    '2.4k bps': 0b010,
    '4.8k bps': 0b011,
    '9.6k bps': 0b100,
    '19.2k bps': 0b101
}

# OPTION Byte breakdown
TRANSMISSION_POWER_OPTIONS = {
    '30dBm (1W)': 0b00,
    '27dBm (500mW)': 0b01,
    '24dBm (250mW)': 0b10,
    '21dBm (125mW)': 0b11
}

FEC_OPTIONS = {
    'FEC Off': 0b0,
    'FEC On': 0b1
}

TRANSMISSION_MODE_OPTIONS = {
    'Transparent': 0b0,
    'Fixed': 0b1
}

IO_DRIVE_MODE_OPTIONS = {
    'Push-pull (TXD/RXD)': 0b0,
    'Open-drain (TXD/RXD)': 0b1
}

WIRELESS_WAKEUP_OPTIONS = {
    '250ms': 0b000,
    '500ms': 0b001,
    '750ms': 0b010,
    '1000ms': 0b011,
    '1250ms': 0b100,
    '1500ms': 0b101,
    '1750ms': 0b110,
    '2000ms': 0b111
}

# CHAN Byte
CHANNEL_OPTIONS = {
    f'Ch {i} ({410.125 + i * 1} MHz)': i for i in range(0, 84)
}

# HEAD Byte options
SAVE_ON_POWER_DOWN_OPTIONS = {
    'Not Save (Temp)': 0xC0,
    'Save (Permanent)': 0xC2
}

@dataclass
class LoRaE32Config:
    """LoRa E32 Complete Configuration"""
    # HEAD byte
    save_on_power_down: int
    
    # ADDH + ADDL (16-bit address)
    device_address: int  # 0x0000 - 0xFFFF
    
    # SPED byte components
    uart_parity: int
    uart_baud_rate: int
    air_data_rate: int
    
    # CHAN byte
    channel: int  # 0-83
    
    # OPTION byte components
    transmission_power: int
    fec_switch: int
    transmission_mode: int
    io_drive_mode: int
    wireless_wakeup_time: int
    
    def encode_sped(self) -> int:
        """Encode SPED byte from components"""
        return (self.uart_parity << 6) | (self.uart_baud_rate << 3) | self.air_data_rate
    
    def encode_option(self) -> int:
        """Encode OPTION byte from components"""
        return (self.transmission_power << 6) | \
               (self.fec_switch << 2) | \
               (self.transmission_mode << 1) | \
               (self.io_drive_mode << 0) | \
               (self.wireless_wakeup_time << 3)
    
    def to_modem_config(self):
        """Convert to LoRaModemConfig for protocol"""
        from config_protocol import LoRaModemConfig
        
        addh = (self.device_address >> 8) & 0xFF
        addl = self.device_address & 0xFF
        
        return LoRaModemConfig(
            head=self.save_on_power_down,
            addh=addh,
            addl=addl,
            sped=self.encode_sped(),
            chan=self.channel,
            option=self.encode_option()
        )
    
    @classmethod
    def from_modem_config(cls, modem_cfg):
        """Parse from LoRaModemConfig"""
        device_address = (modem_cfg.addh << 8) | modem_cfg.addl
        
        # Decode SPED
        uart_parity = (modem_cfg.sped >> 6) & 0b11
        uart_baud_rate = (modem_cfg.sped >> 3) & 0b111
        air_data_rate = modem_cfg.sped & 0b111
        
        # Decode OPTION
        transmission_power = (modem_cfg.option >> 6) & 0b11
        wireless_wakeup_time = (modem_cfg.option >> 3) & 0b111
        fec_switch = (modem_cfg.option >> 2) & 0b1
        transmission_mode = (modem_cfg.option >> 1) & 0b1
        io_drive_mode = modem_cfg.option & 0b1
        
        return cls(
            save_on_power_down=modem_cfg.head,
            device_address=device_address,
            uart_parity=uart_parity,
            uart_baud_rate=uart_baud_rate,
            air_data_rate=air_data_rate,
            channel=modem_cfg.chan,
            transmission_power=transmission_power,
            fec_switch=fec_switch,
            transmission_mode=transmission_mode,
            io_drive_mode=io_drive_mode,
            wireless_wakeup_time=wireless_wakeup_time
        )


# ===== LORA TDMA HANDLER OPTIONS =====

LORA_ROLE_OPTIONS = {
    'Gateway': 0,
    'Node': 1
}

# Default values
DEFAULT_LORA_E32_CONFIG = {
    'save_on_power_down': 0xC2,  # Save permanent
    'device_address': 0x0001,
    'uart_parity': 0b00,  # 8N1
    'uart_baud_rate': 0b011,  # 9600
    'air_data_rate': 0b010,  # 2.4k
    'channel': 23,  # Ch 23 (433 MHz)
    'transmission_power': 0b00,  # 30dBm
    'fec_switch': 0b1,  # FEC On
    'transmission_mode': 0b0,  # Transparent
    'io_drive_mode': 0b1,  # Open-drain
    'wireless_wakeup_time': 0b000  # 250ms
}

DEFAULT_LORA_TDMA_CONFIG = {
    'role': 0,  # Gateway
    'node_id': 0x0001,
    'gateway_id': 0x0001,
    'num_slots': 8,
    'my_slot': 0,
    'slot_duration_ms': 200
}
