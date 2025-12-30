"""
Advanced Mode Panel for ESP32 Gateway Configuration Tool
Contains tabbed interface for detailed configuration - 7 tabs
WiFi | LTE | Server | LoRa | CAN | RS485 | Firmware
"""

import tkinter as tk
from tkinter import ttk
from typing import Optional, Callable

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(__file__))))

from src.config.protocol import GatewayConfig, WanConfig, LanConfig
from src.ui.advanced.wifi_tab import WiFiTab
from src.ui.advanced.lte_tab import LTETab
from src.ui.advanced.server_tab import ServerTab
from src.ui.advanced.lora_tab import LoraTab
from src.ui.advanced.can_tab import CanTab
from src.ui.advanced.rs485_tab import Rs485Tab
from src.ui.advanced.firmware_tab import FirmwareTab


class AdvancedPanel(ttk.Frame):
    """Advanced mode configuration panel with 7 tabs"""
    
    def __init__(self, parent, serial_manager=None,
                 log_callback: Optional[Callable] = None, **kwargs):
        super().__init__(parent, **kwargs)
        
        self.serial_manager = serial_manager
        self.log_callback = log_callback
        self._create_widgets()
    
    def _create_widgets(self):
        """Create advanced mode widgets"""
        # Title
        title_frame = ttk.Frame(self)
        title_frame.pack(fill=tk.X, padx=10, pady=5)
        
        ttk.Label(title_frame, text="⚙️ ADVANCED CONFIGURATION",
                 font=("Segoe UI", 12, "bold")).pack(anchor="w")
        
        # Notebook with 7 tabs (no expand to avoid whitespace)
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill=tk.X, padx=5, pady=5)
        
        # Create tabs with serial_manager and log_callback
        # WAN split into: WiFi, LTE, Server
        self.wifi_tab = WiFiTab(self.notebook, 
                                serial_manager=self.serial_manager,
                                log_callback=self.log_callback)
        self.lte_tab = LTETab(self.notebook, 
                              serial_manager=self.serial_manager,
                              log_callback=self.log_callback)
        self.server_tab = ServerTab(self.notebook, 
                                    serial_manager=self.serial_manager,
                                    log_callback=self.log_callback)
        # LAN interfaces
        self.lora_tab = LoraTab(self.notebook,
                                serial_manager=self.serial_manager,
                                log_callback=self.log_callback)
        self.can_tab = CanTab(self.notebook,
                              serial_manager=self.serial_manager,
                              log_callback=self.log_callback)
        self.rs485_tab = Rs485Tab(self.notebook,
                                  serial_manager=self.serial_manager,
                                  log_callback=self.log_callback)
        self.firmware_tab = FirmwareTab(self.notebook, 
                                         log_callback=self.log_callback)
        
        # Add 7 tabs to notebook
        self.notebook.add(self.wifi_tab, text="📶 WiFi")
        self.notebook.add(self.lte_tab, text="📱 LTE")
        self.notebook.add(self.server_tab, text="☁️ Server")
        self.notebook.add(self.lora_tab, text="📡 LoRa")
        self.notebook.add(self.can_tab, text="🔌 CAN")
        self.notebook.add(self.rs485_tab, text="📟 RS485")
        self.notebook.add(self.firmware_tab, text="🔄 FW")
    
    def set_config(self, config: GatewayConfig):
        """Set config to all tabs"""
        # WiFi settings
        if hasattr(config.wan, 'wifi_ssid'):
            self.wifi_tab.set_config(config.wan)
        # LTE settings
        if hasattr(config.wan, 'lte_apn'):
            self.lte_tab.set_config(config.wan)
        # Server settings
        if hasattr(config.wan, 'server_type'):
            self.server_tab.set_config(config.wan)
        # LAN interfaces
        self.lora_tab.set_config(config.lan.lora)
        self.can_tab.set_config(config.lan.can)
        self.rs485_tab.set_config(config.lan.rs485)
    
    def refresh_ports(self, ports: list):
        """Refresh port list in firmware tab"""
        self.firmware_tab.refresh_ports(ports)
