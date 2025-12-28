#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32 IoT Gateway Configuration Tool v3.2
Improved GUI with Dropdowns for LoRa Configuration
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox, filedialog
import serial
import serial.tools.list_ports
import threading
import json
import subprocess
import sys
import os
from pathlib import Path
from typing import Dict, Any, List
from datetime import datetime
from copy import deepcopy
import traceback

# Import the protocol modules
try:
    from config_protocol import (
        ConfigCommandBuilder, WiFiConfig, LTEConfig, MQTTConfig,
        CANConfig, CANWhitelist, LoRaModemConfig, LoRaTDMAConfig,
        StackTypeConfig, ProtocolValidator, ConfigResponseParser
    )
    from lora_config_definitions import (
        UART_PARITY_OPTIONS, UART_BAUD_RATE_OPTIONS, AIR_DATA_RATE_OPTIONS,
        TRANSMISSION_POWER_OPTIONS, FEC_OPTIONS, TRANSMISSION_MODE_OPTIONS,
        IO_DRIVE_MODE_OPTIONS, WIRELESS_WAKEUP_OPTIONS, CHANNEL_OPTIONS,
        SAVE_ON_POWER_DOWN_OPTIONS, LORA_ROLE_OPTIONS, LoRaE32Config,
        DEFAULT_LORA_E32_CONFIG, DEFAULT_LORA_TDMA_CONFIG
    )
    PROTOCOL_AVAILABLE = True
except ImportError as e:
    print(f"Warning: Required modules not found: {e}")
    PROTOCOL_AVAILABLE = False

def get_application_path():
    """Get the path where the application/script is located"""
    if getattr(sys, 'frozen', False):
        return Path(sys.executable).parent
    else:
        return Path(__file__).parent

class LoRaConfigDialog(tk.Toplevel):
    """Dialog for LoRa E32 Configuration with dropdown selections"""

    def __init__(self, parent, current_config=None):
        super().__init__(parent)
        self.title("LoRa E32 Configuration")
        self.geometry("600x700")
        self.resizable(False, False)

        self.result = None
        self.current_config = current_config or LoRaE32Config(**DEFAULT_LORA_E32_CONFIG)

        self.setup_ui()
        self.load_current_values()

        # Modal
        self.transient(parent)
        self.grab_set()

    def setup_ui(self):
        """Setup dialog UI with all dropdowns"""
        main_frame = ttk.Frame(self, padding="10")
        main_frame.pack(fill=tk.BOTH, expand=True)

        # Title
        title_label = ttk.Label(main_frame, text="LoRa E32 Modem Configuration", 
                                font=('TkDefaultFont', 12, 'bold'))
        title_label.pack(pady=(0, 10))

        # Scrollable frame
        canvas = tk.Canvas(main_frame, height=550)
        scrollbar = ttk.Scrollbar(main_frame, orient="vertical", command=canvas.yview)
        scrollable_frame = ttk.Frame(canvas)

        scrollable_frame.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all"))
        )

        canvas.create_window((0, 0), window=scrollable_frame, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)

        canvas.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        # ===== Configuration Options =====
        row = 0

        # Save Mode
        ttk.Label(scrollable_frame, text="Save Mode:", font=('TkDefaultFont', 9, 'bold')).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=(10, 5))
        row += 1
        self.save_mode_var = tk.StringVar()
        self.save_mode_combo = ttk.Combobox(scrollable_frame, textvariable=self.save_mode_var,
                                            state='readonly', width=40)
        self.save_mode_combo['values'] = list(SAVE_ON_POWER_DOWN_OPTIONS.keys())
        self.save_mode_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # Device Address (0x0000 - 0xFFFF)
        ttk.Label(scrollable_frame, text="Device Address (0x0000-0xFFFF):", 
                  font=('TkDefaultFont', 9, 'bold')).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=(10, 5))
        row += 1
        self.address_var = tk.StringVar()
        address_frame = ttk.Frame(scrollable_frame)
        address_frame.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        ttk.Label(address_frame, text="0x").pack(side=tk.LEFT)
        self.address_entry = ttk.Entry(address_frame, textvariable=self.address_var, width=8)
        self.address_entry.pack(side=tk.LEFT)
        ttk.Label(address_frame, text="  (hex, e.g., 0001 for 0x0001)", 
                  foreground='gray').pack(side=tk.LEFT)
        row += 1

        # UART Configuration
        ttk.Separator(scrollable_frame, orient='horizontal').grid(
            row=row, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=10)
        row += 1
        ttk.Label(scrollable_frame, text="UART Configuration", 
                  font=('TkDefaultFont', 10, 'bold')).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=5)
        row += 1

        # UART Parity
        ttk.Label(scrollable_frame, text="UART Parity:").grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        row += 1
        self.uart_parity_var = tk.StringVar()
        self.uart_parity_combo = ttk.Combobox(scrollable_frame, textvariable=self.uart_parity_var,
                                              state='readonly', width=40)
        self.uart_parity_combo['values'] = list(UART_PARITY_OPTIONS.keys())
        self.uart_parity_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # UART Baud Rate
        ttk.Label(scrollable_frame, text="UART Baud Rate:").grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        row += 1
        self.uart_baud_var = tk.StringVar()
        self.uart_baud_combo = ttk.Combobox(scrollable_frame, textvariable=self.uart_baud_var,
                                            state='readonly', width=40)
        self.uart_baud_combo['values'] = list(UART_BAUD_RATE_OPTIONS.keys())
        self.uart_baud_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # Air Data Rate
        ttk.Label(scrollable_frame, text="Air Data Rate:").grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        row += 1
        self.air_rate_var = tk.StringVar()
        self.air_rate_combo = ttk.Combobox(scrollable_frame, textvariable=self.air_rate_var,
                                           state='readonly', width=40)
        self.air_rate_combo['values'] = list(AIR_DATA_RATE_OPTIONS.keys())
        self.air_rate_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # Wireless Configuration
        ttk.Separator(scrollable_frame, orient='horizontal').grid(
            row=row, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=10)
        row += 1
        ttk.Label(scrollable_frame, text="Wireless Configuration", 
                  font=('TkDefaultFont', 10, 'bold')).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=5)
        row += 1

        # Channel
        ttk.Label(scrollable_frame, text="Channel:").grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        row += 1
        self.channel_var = tk.StringVar()
        self.channel_combo = ttk.Combobox(scrollable_frame, textvariable=self.channel_var,
                                          state='readonly', width=40)
        self.channel_combo['values'] = list(CHANNEL_OPTIONS.keys())
        self.channel_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # Transmission Power
        ttk.Label(scrollable_frame, text="Transmission Power:").grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        row += 1
        self.tx_power_var = tk.StringVar()
        self.tx_power_combo = ttk.Combobox(scrollable_frame, textvariable=self.tx_power_var,
                                           state='readonly', width=40)
        self.tx_power_combo['values'] = list(TRANSMISSION_POWER_OPTIONS.keys())
        self.tx_power_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # Advanced Options
        ttk.Separator(scrollable_frame, orient='horizontal').grid(
            row=row, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=10)
        row += 1
        ttk.Label(scrollable_frame, text="Advanced Options", 
                  font=('TkDefaultFont', 10, 'bold')).grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=5)
        row += 1

        # FEC
        ttk.Label(scrollable_frame, text="Forward Error Correction (FEC):").grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        row += 1
        self.fec_var = tk.StringVar()
        self.fec_combo = ttk.Combobox(scrollable_frame, textvariable=self.fec_var,
                                      state='readonly', width=40)
        self.fec_combo['values'] = list(FEC_OPTIONS.keys())
        self.fec_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # Transmission Mode
        ttk.Label(scrollable_frame, text="Transmission Mode:").grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        row += 1
        self.tx_mode_var = tk.StringVar()
        self.tx_mode_combo = ttk.Combobox(scrollable_frame, textvariable=self.tx_mode_var,
                                          state='readonly', width=40)
        self.tx_mode_combo['values'] = list(TRANSMISSION_MODE_OPTIONS.keys())
        self.tx_mode_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # IO Drive Mode
        ttk.Label(scrollable_frame, text="IO Drive Mode:").grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        row += 1
        self.io_mode_var = tk.StringVar()
        self.io_mode_combo = ttk.Combobox(scrollable_frame, textvariable=self.io_mode_var,
                                          state='readonly', width=40)
        self.io_mode_combo['values'] = list(IO_DRIVE_MODE_OPTIONS.keys())
        self.io_mode_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # Wireless Wakeup Time
        ttk.Label(scrollable_frame, text="Wireless Wakeup Time:").grid(
            row=row, column=0, sticky=tk.W, padx=5, pady=2)
        row += 1
        self.wakeup_var = tk.StringVar()
        self.wakeup_combo = ttk.Combobox(scrollable_frame, textvariable=self.wakeup_var,
                                         state='readonly', width=40)
        self.wakeup_combo['values'] = list(WIRELESS_WAKEUP_OPTIONS.keys())
        self.wakeup_combo.grid(row=row, column=0, columnspan=2, padx=5, pady=2, sticky=tk.W)
        row += 1

        # Buttons
        button_frame = ttk.Frame(main_frame)
        button_frame.pack(pady=10)

        ttk.Button(button_frame, text="Apply", command=self.apply, width=15).pack(side=tk.LEFT, padx=5)
        ttk.Button(button_frame, text="Cancel", command=self.cancel, width=15).pack(side=tk.LEFT, padx=5)
        ttk.Button(button_frame, text="Reset to Default", command=self.reset_default, width=15).pack(side=tk.LEFT, padx=5)

    def load_current_values(self):
        """Load current configuration values into dropdowns"""
        cfg = self.current_config

        # Find matching options
        for key, val in SAVE_ON_POWER_DOWN_OPTIONS.items():
            if val == cfg.save_on_power_down:
                self.save_mode_var.set(key)
                break

        self.address_var.set(f"{cfg.device_address:04X}")

        for key, val in UART_PARITY_OPTIONS.items():
            if val == cfg.uart_parity:
                self.uart_parity_var.set(key)
                break

        for key, val in UART_BAUD_RATE_OPTIONS.items():
            if val == cfg.uart_baud_rate:
                self.uart_baud_var.set(key)
                break

        for key, val in AIR_DATA_RATE_OPTIONS.items():
            if val == cfg.air_data_rate:
                self.air_rate_var.set(key)
                break

        for key, val in CHANNEL_OPTIONS.items():
            if val == cfg.channel:
                self.channel_var.set(key)
                break

        for key, val in TRANSMISSION_POWER_OPTIONS.items():
            if val == cfg.transmission_power:
                self.tx_power_var.set(key)
                break

        for key, val in FEC_OPTIONS.items():
            if val == cfg.fec_switch:
                self.fec_var.set(key)
                break

        for key, val in TRANSMISSION_MODE_OPTIONS.items():
            if val == cfg.transmission_mode:
                self.tx_mode_var.set(key)
                break

        for key, val in IO_DRIVE_MODE_OPTIONS.items():
            if val == cfg.io_drive_mode:
                self.io_mode_var.set(key)
                break

        for key, val in WIRELESS_WAKEUP_OPTIONS.items():
            if val == cfg.wireless_wakeup_time:
                self.wakeup_var.set(key)
                break

    def apply(self):
        """Apply configuration"""
        try:
            # Parse address
            addr_str = self.address_var.get().strip()
            device_address = int(addr_str, 16)
            if not (0 <= device_address <= 0xFFFF):
                raise ValueError("Address must be 0x0000-0xFFFF")

            # Build configuration
            self.result = LoRaE32Config(
                save_on_power_down=SAVE_ON_POWER_DOWN_OPTIONS[self.save_mode_var.get()],
                device_address=device_address,
                uart_parity=UART_PARITY_OPTIONS[self.uart_parity_var.get()],
                uart_baud_rate=UART_BAUD_RATE_OPTIONS[self.uart_baud_var.get()],
                air_data_rate=AIR_DATA_RATE_OPTIONS[self.air_rate_var.get()],
                channel=CHANNEL_OPTIONS[self.channel_var.get()],
                transmission_power=TRANSMISSION_POWER_OPTIONS[self.tx_power_var.get()],
                fec_switch=FEC_OPTIONS[self.fec_var.get()],
                transmission_mode=TRANSMISSION_MODE_OPTIONS[self.tx_mode_var.get()],
                io_drive_mode=IO_DRIVE_MODE_OPTIONS[self.io_mode_var.get()],
                wireless_wakeup_time=WIRELESS_WAKEUP_OPTIONS[self.wakeup_var.get()]
            )

            self.destroy()

        except Exception as e:
            messagebox.showerror("Error", f"Invalid configuration: {e}")

    def cancel(self):
        """Cancel configuration"""
        self.result = None
        self.destroy()

    def reset_default(self):
        """Reset to default values"""
        self.current_config = LoRaE32Config(**DEFAULT_LORA_E32_CONFIG)
        self.load_current_values()

class LoRaTDMAConfigDialog(tk.Toplevel):
    """Dialog for LoRa TDMA Handler Configuration with dropdowns"""

    def __init__(self, parent, current_config=None):
        super().__init__(parent)
        self.title("LoRa TDMA Configuration")
        self.geometry("500x400")
        self.resizable(False, False)

        self.result = None
        self.current_config = current_config

        self.setup_ui()
        if current_config:
            self.load_current_values()
        else:
            self.load_defaults()

        # Modal
        self.transient(parent)
        self.grab_set()

    def setup_ui(self):
        """Setup TDMA config UI"""
        main_frame = ttk.Frame(self, padding="20")
        main_frame.pack(fill=tk.BOTH, expand=True)

        # Title
        title_label = ttk.Label(main_frame, text="LoRa TDMA Handler Configuration", 
                                font=('TkDefaultFont', 12, 'bold'))
        title_label.pack(pady=(0, 20))

        # Role
        role_frame = ttk.Frame(main_frame)
        role_frame.pack(fill=tk.X, pady=5)
        ttk.Label(role_frame, text="Role:", width=20).pack(side=tk.LEFT)
        self.role_var = tk.StringVar()
        self.role_combo = ttk.Combobox(role_frame, textvariable=self.role_var,
                                       state='readonly', width=30)
        self.role_combo['values'] = list(LORA_ROLE_OPTIONS.keys())
        self.role_combo.pack(side=tk.LEFT, padx=5)

        # Node ID
        node_frame = ttk.Frame(main_frame)
        node_frame.pack(fill=tk.X, pady=5)
        ttk.Label(node_frame, text="Node ID (hex):", width=20).pack(side=tk.LEFT)
        self.node_id_var = tk.StringVar()
        node_entry_frame = ttk.Frame(node_frame)
        node_entry_frame.pack(side=tk.LEFT)
        ttk.Label(node_entry_frame, text="0x").pack(side=tk.LEFT)
        self.node_id_entry = ttk.Entry(node_entry_frame, textvariable=self.node_id_var, width=10)
        self.node_id_entry.pack(side=tk.LEFT)

        # Gateway ID
        gw_frame = ttk.Frame(main_frame)
        gw_frame.pack(fill=tk.X, pady=5)
        ttk.Label(gw_frame, text="Gateway ID (hex):", width=20).pack(side=tk.LEFT)
        self.gateway_id_var = tk.StringVar()
        gw_entry_frame = ttk.Frame(gw_frame)
        gw_entry_frame.pack(side=tk.LEFT)
        ttk.Label(gw_entry_frame, text="0x").pack(side=tk.LEFT)
        self.gateway_id_entry = ttk.Entry(gw_entry_frame, textvariable=self.gateway_id_var, width=10)
        self.gateway_id_entry.pack(side=tk.LEFT)

        # Number of slots (dropdown 2-16)
        slots_frame = ttk.Frame(main_frame)
        slots_frame.pack(fill=tk.X, pady=5)
        ttk.Label(slots_frame, text="Number of Slots:", width=20).pack(side=tk.LEFT)
        self.num_slots_var = tk.StringVar()
        self.num_slots_combo = ttk.Combobox(slots_frame, textvariable=self.num_slots_var,
                                            state='readonly', width=30)
        self.num_slots_combo['values'] = [str(i) for i in range(2, 17)]
        self.num_slots_combo.pack(side=tk.LEFT, padx=5)

        # My slot (dropdown 0-15)
        my_slot_frame = ttk.Frame(main_frame)
        my_slot_frame.pack(fill=tk.X, pady=5)
        ttk.Label(my_slot_frame, text="My Slot:", width=20).pack(side=tk.LEFT)
        self.my_slot_var = tk.StringVar()
        self.my_slot_combo = ttk.Combobox(my_slot_frame, textvariable=self.my_slot_var,
                                          state='readonly', width=30)
        self.my_slot_combo['values'] = [str(i) for i in range(16)]
        self.my_slot_combo.pack(side=tk.LEFT, padx=5)

        # Slot duration (dropdown common values)
        duration_frame = ttk.Frame(main_frame)
        duration_frame.pack(fill=tk.X, pady=5)
        ttk.Label(duration_frame, text="Slot Duration (ms):", width=20).pack(side=tk.LEFT)
        self.slot_duration_var = tk.StringVar()
        self.slot_duration_combo = ttk.Combobox(duration_frame, textvariable=self.slot_duration_var,
                                                state='readonly', width=30)
        self.slot_duration_combo['values'] = ['100', '200', '250', '500', '1000']
        self.slot_duration_combo.pack(side=tk.LEFT, padx=5)

        # Buttons
        button_frame = ttk.Frame(main_frame)
        button_frame.pack(pady=20)

        ttk.Button(button_frame, text="Apply", command=self.apply, width=15).pack(side=tk.LEFT, padx=5)
        ttk.Button(button_frame, text="Cancel", command=self.cancel, width=15).pack(side=tk.LEFT, padx=5)

    def load_defaults(self):
        """Load default values"""
        self.role_var.set('Gateway')
        self.node_id_var.set('0001')
        self.gateway_id_var.set('0001')
        self.num_slots_var.set('8')
        self.my_slot_var.set('0')
        self.slot_duration_var.set('200')

    def load_current_values(self):
        """Load current configuration values"""
        cfg = self.current_config

        for key, val in LORA_ROLE_OPTIONS.items():
            if val == cfg.role:
                self.role_var.set(key)
                break

        self.node_id_var.set(f"{cfg.node_id:04X}")
        self.gateway_id_var.set(f"{cfg.gateway_id:04X}")
        self.num_slots_var.set(str(cfg.num_slots))
        self.my_slot_var.set(str(cfg.my_slot))
        self.slot_duration_var.set(str(cfg.slot_duration_ms))

    def apply(self):
        """Apply configuration"""
        try:
            node_id = int(self.node_id_var.get(), 16)
            gateway_id = int(self.gateway_id_var.get(), 16)

            if not (0 <= node_id <= 0xFFFF):
                raise ValueError("Node ID must be 0x0000-0xFFFF")
            if not (0 <= gateway_id <= 0xFFFF):
                raise ValueError("Gateway ID must be 0x0000-0xFFFF")

            self.result = LoRaTDMAConfig(
                role=LORA_ROLE_OPTIONS[self.role_var.get()],
                node_id=node_id,
                gateway_id=gateway_id,
                num_slots=int(self.num_slots_var.get()),
                my_slot=int(self.my_slot_var.get()),
                slot_duration_ms=int(self.slot_duration_var.get())
            )

            self.destroy()

        except Exception as e:
            messagebox.showerror("Error", f"Invalid configuration: {e}")

    def cancel(self):
        """Cancel configuration"""
        self.result = None
        self.destroy()

class GatewayConfigTool:
    """Main application class - enhanced with dropdown UI"""

    READ_ONLY_FIELDS = {
        'GATEWAY_INFO': '*',
        'LAN_CONFIG': ['can_whitelist_count', 'can_whitelist', 'lora_e32_baud', 'lora_e32_header'],
    }

    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 Gateway Config Tool v3.2 - Dropdown Edition")
        self.root.geometry("1200x800")

        self.serial_port = None
        self.config_data = {}
        self.original_config = {}
        self.reading_config = False
        self.config_buffer = []
        self.config_modified = False

        # CAN whitelist tracking
        self.can_whitelist_ids = []

        # LoRa configurations
        self.lora_modem_config = None
        self.lora_tdma_config = None

        self.app_path = get_application_path()

        self.setup_ui()
        self.refresh_ports()

    def setup_ui(self):
        """Setup main UI"""
        # Main container
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))

        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)
        main_frame.rowconfigure(2, weight=1)

        # ===== Serial Connection Panel =====
        conn_frame = ttk.LabelFrame(main_frame, text="Serial Connection", padding="5")
        conn_frame.grid(row=0, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)

        ttk.Label(conn_frame, text="Port:").grid(row=0, column=0, padx=5)
        self.port_combo = ttk.Combobox(conn_frame, width=15, state='readonly')
        self.port_combo.grid(row=0, column=1, padx=5)

        ttk.Label(conn_frame, text="Baud:").grid(row=0, column=2, padx=5)
        self.baud_combo = ttk.Combobox(conn_frame, width=10, state='readonly',
                                       values=['9600', '115200', '230400', '460800', '921600'])
        self.baud_combo.set('115200')
        self.baud_combo.grid(row=0, column=3, padx=5)

        self.refresh_btn = ttk.Button(conn_frame, text="Refresh", command=self.refresh_ports)
        self.refresh_btn.grid(row=0, column=4, padx=5)

        self.connect_btn = ttk.Button(conn_frame, text="Connect", command=self.toggle_connection)
        self.connect_btn.grid(row=0, column=5, padx=5)

        self.test_btn = ttk.Button(conn_frame, text="Test", command=self.test_connection, state='disabled')
        self.test_btn.grid(row=0, column=6, padx=5)

        self.status_label = ttk.Label(conn_frame, text="Disconnected", foreground="red")
        self.status_label.grid(row=0, column=7, padx=10)

        # ===== Actions Panel =====
        actions_frame = ttk.LabelFrame(main_frame, text="Actions", padding="5")
        actions_frame.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)

        self.scan_btn = ttk.Button(actions_frame, text="Read Config (CFSC)",
                                    command=self.read_config, state='disabled', width=20)
        self.scan_btn.grid(row=0, column=0, padx=5)

        self.write_btn = ttk.Button(actions_frame, text="Write Changes",
                                     command=self.write_config, state='disabled', width=20)
        self.write_btn.grid(row=0, column=1, padx=5)

        ttk.Separator(actions_frame, orient='vertical').grid(row=0, column=2, sticky='ns', padx=10)

        self.config_mode_btn = ttk.Button(actions_frame, text="Config Mode",
                                          command=self.enter_config_mode, state='disabled', width=15)
        self.config_mode_btn.grid(row=0, column=3, padx=5)

        self.normal_mode_btn = ttk.Button(actions_frame, text="Normal Mode",
                                          command=self.enter_normal_mode, state='disabled', width=15)
        self.normal_mode_btn.grid(row=0, column=4, padx=5)

        ttk.Separator(actions_frame, orient='vertical').grid(row=0, column=5, sticky='ns', padx=10)

        self.save_btn = ttk.Button(actions_frame, text="Save to File",
                                    command=self.save_to_file, state='disabled', width=15)
        self.save_btn.grid(row=0, column=6, padx=5)

        self.load_btn = ttk.Button(actions_frame, text="Load from File",
                                    command=self.load_from_file, width=15)
        self.load_btn.grid(row=0, column=7, padx=5)

        ttk.Separator(actions_frame, orient='vertical').grid(row=0, column=8, sticky='ns', padx=10)

        self.firmware_btn = ttk.Button(actions_frame, text="Update Firmware",
                                       command=self.update_firmware, width=20)
        self.firmware_btn.grid(row=0, column=9, padx=5)

        self.clear_btn = ttk.Button(actions_frame, text="Clear",
                                     command=self.clear_config, width=10)
        self.clear_btn.grid(row=0, column=10, padx=5)

        # ===== Config Display Panel =====
        config_frame = ttk.LabelFrame(main_frame, text="Configuration Data", padding="5")
        config_frame.grid(row=2, column=0, columnspan=2, sticky=(tk.W, tk.E, tk.N, tk.S), pady=5)
        config_frame.columnconfigure(0, weight=1)
        config_frame.rowconfigure(0, weight=1)

        # Create notebook for tabs
        self.notebook = ttk.Notebook(config_frame)
        self.notebook.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))

        # Raw view tab
        raw_frame = ttk.Frame(self.notebook)
        self.notebook.add(raw_frame, text="Raw View")

        self.raw_text = scrolledtext.ScrolledText(raw_frame, wrap=tk.WORD,
                                                   font=('Courier New', 10))
        self.raw_text.pack(fill=tk.BOTH, expand=True)

        # Editable view tab
        edit_frame = ttk.Frame(self.notebook)
        self.notebook.add(edit_frame, text="Edit Config")

        tree_frame = ttk.Frame(edit_frame)
        tree_frame.pack(fill=tk.BOTH, expand=True)

        self.tree = ttk.Treeview(tree_frame, columns=('value', 'changed'), show='tree headings')
        self.tree.heading('#0', text='Parameter')
        self.tree.heading('value', text='Value')
        self.tree.heading('changed', text='Status')
        self.tree.column('#0', width=250)
        self.tree.column('value', width=400)
        self.tree.column('changed', width=80)

        tree_scroll = ttk.Scrollbar(tree_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=tree_scroll.set)

        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        tree_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        self.tree.bind('<Double-Button-1>', self.edit_tree_item)

        edit_controls = ttk.Frame(edit_frame)
        edit_controls.pack(fill=tk.X, padx=5, pady=5)

        ttk.Button(edit_controls, text="Edit Selected",
                   command=self.edit_selected_item).pack(side=tk.LEFT, padx=5)
        ttk.Button(edit_controls, text="Revert Changes",
                   command=self.revert_changes).pack(side=tk.LEFT, padx=5)

        self.changes_label = ttk.Label(edit_controls, text="No changes", foreground="green")
        self.changes_label.pack(side=tk.LEFT, padx=20)

        # CAN Whitelist tab
        can_whitelist_frame = ttk.Frame(self.notebook)
        self.notebook.add(can_whitelist_frame, text="CAN Whitelist")
        self.setup_can_whitelist_tab(can_whitelist_frame)

        # NEW: LoRa Configuration tab
        lora_config_frame = ttk.Frame(self.notebook)
        self.notebook.add(lora_config_frame, text="LoRa Config")
        self.setup_lora_config_tab(lora_config_frame)

        # ===== Console Panel =====
        console_frame = ttk.LabelFrame(main_frame, text="Console Log", padding="5")
        console_frame.grid(row=3, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)

        self.console = scrolledtext.ScrolledText(console_frame, height=8,
                                                  font=('Courier New', 9),
                                                  bg='#1e1e1e', fg='#00ff00')
        self.console.pack(fill=tk.BOTH, expand=True)

    def setup_can_whitelist_tab(self, parent):
        """Setup CAN Whitelist management tab"""
        top_frame = ttk.LabelFrame(parent, text="Current Whitelist", padding="10")
        top_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        list_frame = ttk.Frame(top_frame)
        list_frame.pack(fill=tk.BOTH, expand=True)

        self.whitelist_listbox = tk.Listbox(list_frame, font=('Courier New', 10), height=10)
        self.whitelist_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        list_scroll = ttk.Scrollbar(list_frame, orient="vertical", command=self.whitelist_listbox.yview)
        self.whitelist_listbox.configure(yscrollcommand=list_scroll.set)
        list_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        controls_frame = ttk.Frame(parent, padding="5")
        controls_frame.pack(fill=tk.X, padx=5, pady=5)

        add_frame = ttk.LabelFrame(controls_frame, text="Add CAN ID", padding="5")
        add_frame.pack(side=tk.LEFT, padx=5)

        ttk.Label(add_frame, text="CAN ID (hex):").grid(row=0, column=0, padx=5)
        self.can_id_entry = ttk.Entry(add_frame, width=12)
        self.can_id_entry.grid(row=0, column=1, padx=5)
        self.can_id_entry.insert(0, "0x")

        ttk.Button(add_frame, text="Add", command=self.add_can_whitelist_id).grid(row=0, column=2, padx=5)

        action_frame = ttk.Frame(controls_frame)
        action_frame.pack(side=tk.LEFT, padx=20)

        ttk.Button(action_frame, text="Remove Selected",
                   command=self.remove_can_whitelist_id, width=18).pack(pady=2)
        ttk.Button(action_frame, text="Clear All",
                   command=self.clear_can_whitelist, width=18).pack(pady=2)
        ttk.Button(action_frame, text="Apply to Gateway",
                   command=self.apply_can_whitelist, width=18).pack(pady=2)

    def setup_lora_config_tab(self, parent):
        """Setup LoRa Configuration tab with dropdown buttons"""
        # Info frame
        info_frame = ttk.LabelFrame(parent, text="Current LoRa Configuration", padding="10")
        info_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        # Modem config display
        modem_frame = ttk.LabelFrame(info_frame, text="E32 Modem Config", padding="10")
        modem_frame.pack(fill=tk.BOTH, expand=True, pady=5)

        self.lora_modem_text = scrolledtext.ScrolledText(modem_frame, height=10,
                                                          font=('Courier New', 9),
                                                          wrap=tk.WORD)
        self.lora_modem_text.pack(fill=tk.BOTH, expand=True)

        # TDMA config display
        tdma_frame = ttk.LabelFrame(info_frame, text="TDMA Handler Config", padding="10")
        tdma_frame.pack(fill=tk.BOTH, expand=True, pady=5)

        self.lora_tdma_text = scrolledtext.ScrolledText(tdma_frame, height=8,
                                                         font=('Courier New', 9),
                                                         wrap=tk.WORD)
        self.lora_tdma_text.pack(fill=tk.BOTH, expand=True)

        # Control buttons
        button_frame = ttk.Frame(parent, padding="10")
        button_frame.pack(fill=tk.X, padx=10, pady=5)

        ttk.Button(button_frame, text="Configure E32 Modem",
                   command=self.open_lora_modem_dialog, width=25).pack(side=tk.LEFT, padx=5)
        ttk.Button(button_frame, text="Configure TDMA Handler",
                   command=self.open_lora_tdma_dialog, width=25).pack(side=tk.LEFT, padx=5)
        ttk.Button(button_frame, text="Apply to Gateway",
                   command=self.apply_lora_config, width=20).pack(side=tk.LEFT, padx=5)

    def open_lora_modem_dialog(self):
        """Open LoRa E32 modem configuration dialog"""
        if not PROTOCOL_AVAILABLE:
            messagebox.showerror("Error", "Protocol modules not available")
            return

        dialog = LoRaConfigDialog(self.root, self.lora_modem_config)
        self.root.wait_window(dialog)

        if dialog.result:
            self.lora_modem_config = dialog.result
            self.update_lora_display()
            self.log("LoRa E32 modem configuration updated", 'INFO')

    def open_lora_tdma_dialog(self):
        """Open LoRa TDMA handler configuration dialog"""
        if not PROTOCOL_AVAILABLE:
            messagebox.showerror("Error", "Protocol modules not available")
            return

        dialog = LoRaTDMAConfigDialog(self.root, self.lora_tdma_config)
        self.root.wait_window(dialog)

        if dialog.result:
            self.lora_tdma_config = dialog.result
            self.update_lora_display()
            self.log("LoRa TDMA handler configuration updated", 'INFO')

    def update_lora_display(self):
        """Update LoRa configuration display"""
        # Update modem config
        self.lora_modem_text.delete('1.0', tk.END)
        if self.lora_modem_config:
            cfg = self.lora_modem_config
            modem_info = f"""Save Mode: {[k for k, v in SAVE_ON_POWER_DOWN_OPTIONS.items() if v == cfg.save_on_power_down][0]}
Device Address: 0x{cfg.device_address:04X}

UART Configuration:
  Parity: {[k for k, v in UART_PARITY_OPTIONS.items() if v == cfg.uart_parity][0]}
  Baud Rate: {[k for k, v in UART_BAUD_RATE_OPTIONS.items() if v == cfg.uart_baud_rate][0]}
  Air Data Rate: {[k for k, v in AIR_DATA_RATE_OPTIONS.items() if v == cfg.air_data_rate][0]}

Wireless Configuration:
  Channel: {[k for k, v in CHANNEL_OPTIONS.items() if v == cfg.channel][0]}
  TX Power: {[k for k, v in TRANSMISSION_POWER_OPTIONS.items() if v == cfg.transmission_power][0]}

Advanced Options:
  FEC: {[k for k, v in FEC_OPTIONS.items() if v == cfg.fec_switch][0]}
  TX Mode: {[k for k, v in TRANSMISSION_MODE_OPTIONS.items() if v == cfg.transmission_mode][0]}
  IO Mode: {[k for k, v in IO_DRIVE_MODE_OPTIONS.items() if v == cfg.io_drive_mode][0]}
  Wakeup Time: {[k for k, v in WIRELESS_WAKEUP_OPTIONS.items() if v == cfg.wireless_wakeup_time][0]}

Raw Bytes: {cfg.to_modem_config().to_bytes().hex().upper()}
"""
            self.lora_modem_text.insert('1.0', modem_info)
        else:
            self.lora_modem_text.insert('1.0', "No configuration loaded")

        # Update TDMA config
        self.lora_tdma_text.delete('1.0', tk.END)
        if self.lora_tdma_config:
            cfg = self.lora_tdma_config
            tdma_info = f"""Role: {[k for k, v in LORA_ROLE_OPTIONS.items() if v == cfg.role][0]}
Node ID: 0x{cfg.node_id:04X}
Gateway ID: 0x{cfg.gateway_id:04X}
Number of Slots: {cfg.num_slots}
My Slot: {cfg.my_slot}
Slot Duration: {cfg.slot_duration_ms} ms

Raw Bytes: {cfg.to_bytes().hex().upper()}
"""
            self.lora_tdma_text.insert('1.0', tdma_info)
        else:
            self.lora_tdma_text.insert('1.0', "No configuration loaded")

    def apply_lora_config(self):
        """Apply LoRa configuration to gateway"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        if not PROTOCOL_AVAILABLE:
            messagebox.showerror("Error", "Protocol modules not available")
            return

        try:
            commands_sent = []

            # Send modem config if available
            if self.lora_modem_config:
                modem_cfg = self.lora_modem_config.to_modem_config()
                cmd_bytes = ConfigCommandBuilder.build_lora_modem_command(modem_cfg)
                self.serial_port.write(cmd_bytes + b"\r\n")
                self.serial_port.flush()
                commands_sent.append("E32 Modem Config")
                self.log(f"Sent LoRa modem config: {cmd_bytes.hex()}", 'SUCCESS')

            # Send TDMA config if available
            if self.lora_tdma_config:
                cmd_bytes = ConfigCommandBuilder.build_lora_tdma_command(self.lora_tdma_config)
                self.serial_port.write(cmd_bytes + b"\r\n")
                self.serial_port.flush()
                commands_sent.append("TDMA Handler Config")
                self.log(f"Sent LoRa TDMA config: {cmd_bytes.hex()}", 'SUCCESS')

            if commands_sent:
                messagebox.showinfo("Success", 
                                    f"LoRa configuration applied:\n" + "\n".join(commands_sent))
            else:
                messagebox.showwarning("Warning", "No LoRa configuration to apply")

        except Exception as e:
            self.log(f"Failed to apply LoRa config: {e}", 'ERROR')
            messagebox.showerror("Error", str(e))

    # ===== Serial Communication Methods =====

    def log(self, message, level='INFO'):
        """Thread-safe logging"""
        def _log():
            timestamp = datetime.now().strftime('%H:%M:%S')
            color = {'INFO': '#00ff00', 'WARN': '#ffaa00', 'ERROR': '#ff0000',
                     'SUCCESS': '#00ffff', 'DEBUG': '#888888'}.get(level, '#ffffff')

            self.console.tag_config(level, foreground=color)
            self.console.insert(tk.END, f"[{timestamp}] ", 'INFO')
            self.console.insert(tk.END, f"{message}\n", level)
            self.console.see(tk.END)

        if threading.current_thread() != threading.main_thread():
            self.root.after(0, _log)
        else:
            _log()

    def refresh_ports(self):
        """Refresh serial port list"""
        ports = serial.tools.list_ports.comports()
        port_list = [p.device for p in ports]
        self.port_combo['values'] = port_list
        if port_list:
            self.port_combo.current(0)
        self.log(f"Found {len(port_list)} serial port(s)")

    def toggle_connection(self):
        """Toggle serial connection"""
        if self.serial_port and self.serial_port.is_open:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        """Connect to serial port"""
        port = self.port_combo.get()
        baud = int(self.baud_combo.get())

        if not port:
            messagebox.showerror("Error", "Please select a serial port")
            return

        try:
            self.serial_port = serial.Serial(
                port=port,
                baudrate=baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1,
                write_timeout=2,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False
            )

            self.serial_port.reset_input_buffer()
            self.serial_port.reset_output_buffer()
            threading.Event().wait(0.1)

            self.status_label.config(text=f"Connected to {port}", foreground="green")
            self.connect_btn.config(text="Disconnect")
            self.scan_btn.config(state='normal')
            self.test_btn.config(state='normal')
            self.config_mode_btn.config(state='normal')
            self.normal_mode_btn.config(state='normal')

            self.log(f"Connected to {port} at {baud} baud", 'SUCCESS')

            self.reading_thread = threading.Thread(target=self.read_serial, daemon=True)
            self.reading_thread.start()

        except Exception as e:
            messagebox.showerror("Connection Error", str(e))
            self.log(f"Connection failed: {e}", 'ERROR')

    def disconnect(self):
        """Disconnect from serial port"""
        if self.serial_port:
            try:
                self.serial_port.close()
            except:
                pass
            self.serial_port = None

        self.status_label.config(text="Disconnected", foreground="red")
        self.connect_btn.config(text="Connect")
        self.scan_btn.config(state='disabled')
        self.test_btn.config(state='disabled')
        self.config_mode_btn.config(state='disabled')
        self.normal_mode_btn.config(state='disabled')

        self.log("Disconnected")

    def test_connection(self):
        """Test serial connection"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        try:
            self.log("="*60, 'DEBUG')
            self.log("Testing connection...", 'INFO')

            test_cmd = b"\r\n"
            bytes_written = self.serial_port.write(test_cmd)
            self.serial_port.flush()

            self.log(f"Sent {bytes_written} bytes (CRLF)", 'SUCCESS')
            self.log("="*60, 'DEBUG')

            messagebox.showinfo("Test", "Test command sent successfully!")

        except Exception as e:
            self.log(f"Test failed: {e}", 'ERROR')
            messagebox.showerror("Error", f"Test failed: {e}")

    def read_serial(self):
        """Read serial data in background thread"""
        buffer = ""
        while self.serial_port and self.serial_port.is_open:
            try:
                if self.serial_port.in_waiting:
                    data = self.serial_port.read(self.serial_port.in_waiting).decode('utf-8', errors='ignore')
                    buffer += data

                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        line = line.strip()
                        if line:
                            self.process_line(line)

            except Exception as e:
                self.log(f"Read error: {e}", 'ERROR')
                break

    def process_line(self, line):
        """Process received line"""
        # Skip ESP-IDF log messages
        if line.startswith('I (') or line.startswith('W (') or line.startswith('E ('):
            return

        if line == "CFSC_RESP:START":
            self.reading_config = True
            self.config_buffer = []
            self.log("Started reading config...", 'INFO')
            return

        elif line == "CFSC_RESP:END":
            self.reading_config = False
            self.parse_config(self.config_buffer)
            self.log("Config reading completed", 'SUCCESS')
            return

        if self.reading_config:
            self.config_buffer.append(line)

    def parse_config(self, lines):
        """Parse configuration from gateway"""
        if PROTOCOL_AVAILABLE:
            self.config_data = ConfigResponseParser.parse_config_dump(lines)
        else:
            # Fallback parsing
            self.config_data = {'GATEWAY_INFO': {}, 'WAN_CONFIG': {}, 'LAN_CONFIG': {}}
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
                    if current_section in self.config_data:
                        self.config_data[current_section][key.strip()] = value.strip()

        # Parse CAN whitelist if present
        if 'can_whitelist' in self.config_data.get('LAN_CONFIG', {}):
            whitelist_str = self.config_data['LAN_CONFIG']['can_whitelist']
            if PROTOCOL_AVAILABLE:
                self.can_whitelist_ids = ConfigResponseParser.parse_can_whitelist(whitelist_str)
                self.update_whitelist_display()

        self.original_config = deepcopy(self.config_data)
        self.config_modified = False
        self.display_config()
        self.save_btn.config(state='normal')
        self.update_changes_status()

    # ===== Configuration Management Methods =====

    def read_config(self):
        """Read configuration from gateway"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        try:
            self.serial_port.write(b"CFSC\r\n")
            self.serial_port.flush()
            self.log("Sent CFSC command", 'SUCCESS')

        except Exception as e:
            self.log(f"Failed to send CFSC: {e}", 'ERROR')
            messagebox.showerror("Error", str(e))

    def write_config(self):
        """Write configuration to gateway"""
        messagebox.showinfo("Info", "Write configuration feature - to be implemented")

    def enter_config_mode(self):
        """Enter configuration mode"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        try:
            self.serial_port.write(b"CONFIG\r\n")
            self.serial_port.flush()
            self.log("Sent CONFIG command", 'SUCCESS')
            messagebox.showinfo("Success", "Config mode command sent")

        except Exception as e:
            self.log(f"Failed to send CONFIG: {e}", 'ERROR')
            messagebox.showerror("Error", str(e))

    def enter_normal_mode(self):
        """Enter normal mode"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        try:
            self.serial_port.write(b"NORMAL\r\n")
            self.serial_port.flush()
            self.log("Sent NORMAL command", 'SUCCESS')
            messagebox.showinfo("Success", "Normal mode command sent")

        except Exception as e:
            self.log(f"Failed to send NORMAL: {e}", 'ERROR')
            messagebox.showerror("Error", str(e))

    # ===== CAN Whitelist Methods =====

    def add_can_whitelist_id(self):
        """Add CAN ID to whitelist"""
        id_str = self.can_id_entry.get().strip()
        if not id_str:
            messagebox.showwarning("Warning", "Please enter a CAN ID")
            return

        try:
            can_id = int(id_str, 16)

            if PROTOCOL_AVAILABLE:
                valid, msg = ProtocolValidator.validate_can_id(can_id, extended=False)
                if not valid:
                    messagebox.showerror("Error", msg)
                    return

            if can_id in self.can_whitelist_ids:
                messagebox.showinfo("Info", f"ID {id_str} already in whitelist")
                return

            self.can_whitelist_ids.append(can_id)
            self.can_whitelist_ids.sort()
            self.update_whitelist_display()
            self.config_modified = True
            self.update_changes_status()
            self.log(f"Added CAN ID {id_str} to whitelist", 'INFO')

            self.can_id_entry.delete(0, tk.END)
            self.can_id_entry.insert(0, "0x")

        except ValueError:
            messagebox.showerror("Error", "Invalid hex format. Use 0xXXX")

    def remove_can_whitelist_id(self):
        """Remove selected CAN ID"""
        selection = self.whitelist_listbox.curselection()
        if not selection:
            messagebox.showwarning("Warning", "Please select an ID to remove")
            return

        idx = selection[0]
        removed_id = self.can_whitelist_ids.pop(idx)
        self.update_whitelist_display()
        self.config_modified = True
        self.update_changes_status()
        self.log(f"Removed CAN ID 0x{removed_id:03X} from whitelist", 'INFO')

    def clear_can_whitelist(self):
        """Clear entire whitelist"""
        if not self.can_whitelist_ids:
            messagebox.showinfo("Info", "Whitelist is already empty")
            return

        if messagebox.askyesno("Confirm", "Clear entire whitelist?"):
            self.can_whitelist_ids.clear()
            self.update_whitelist_display()
            self.config_modified = True
            self.update_changes_status()
            self.log("Cleared CAN whitelist", 'INFO')

    def apply_can_whitelist(self):
        """Apply whitelist to gateway"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        if not PROTOCOL_AVAILABLE:
            messagebox.showerror("Error", "config_protocol.py not available")
            return

        try:
            if len(self.can_whitelist_ids) == 0:
                cmd = ConfigCommandBuilder.build_can_whitelist_clear()
                self.serial_port.write(f"{cmd}\r\n".encode('utf-8'))
                self.serial_port.flush()
                self.log(f"Sent: {cmd}", 'SUCCESS')
            else:
                cmd = ConfigCommandBuilder.build_can_whitelist_set(self.can_whitelist_ids)
                self.serial_port.write(f"{cmd}\r\n".encode('utf-8'))
                self.serial_port.flush()
                self.log(f"Sent: {cmd}", 'SUCCESS')

            messagebox.showinfo("Success", "CAN whitelist applied to gateway")

        except Exception as e:
            self.log(f"Failed to apply whitelist: {e}", 'ERROR')
            messagebox.showerror("Error", str(e))

    def update_whitelist_display(self):
        """Update whitelist listbox"""
        self.whitelist_listbox.delete(0, tk.END)
        for can_id in self.can_whitelist_ids:
            self.whitelist_listbox.insert(tk.END, f"0x{can_id:03X}")

    # ===== Display and Edit Methods =====

    def display_config(self):
        """Display configuration"""
        # Raw view
        self.raw_text.delete('1.0', tk.END)
        raw_content = self.format_config_raw()
        self.raw_text.insert('1.0', raw_content)

        # Tree view
        self.tree.delete(*self.tree.get_children())

        for section, params in self.config_data.items():
            section_node = self.tree.insert('', 'end', text=f'[{section}]',
                                             values=('', ''), open=True, tags=('section',))
            self.tree.tag_configure('section', font=('TkDefaultFont', 10, 'bold'))

            for key, value in params.items():
                status = ''
                tag = ''

                if section in self.original_config:
                    orig_value = self.original_config[section].get(key, '')
                    if orig_value != value:
                        status = 'Changed'
                        tag = 'changed'

                self.tree.insert(section_node, 'end', text=key,
                                 values=(value, status), tags=(tag,))

        self.tree.tag_configure('changed', foreground='blue', font=('TkDefaultFont', 9, 'bold'))

    def format_config_raw(self):
        """Format configuration as raw text"""
        lines = []
        for section, params in self.config_data.items():
            lines.append(f"[{section}]")
            for key, value in params.items():
                lines.append(f"{key}={value}")
            lines.append("")
        return '\n'.join(lines)

    def edit_tree_item(self, event):
        """Edit tree item on double-click"""
        self.edit_selected_item()

    def edit_selected_item(self):
        """Edit selected item with READ-ONLY protection"""
        selection = self.tree.selection()
        if not selection:
            messagebox.showwarning("Warning", "Please select an item to edit")
            return

        item = selection[0]
        parent = self.tree.parent(item)

        if not parent:
            return

        key = self.tree.item(item, 'text')
        current_value = self.tree.item(item, 'values')[0]
        section = self.tree.item(parent, 'text').strip('[]')

        # Check READ-ONLY
        if section in self.READ_ONLY_FIELDS:
            if self.READ_ONLY_FIELDS[section] == '*':
                messagebox.showinfo("Info", f"{section} is read-only")
                return
            elif key in self.READ_ONLY_FIELDS[section]:
                messagebox.showinfo("Info", f"'{key}' is read-only. Use appropriate tab to manage.")
                return

        # Simple edit dialog (to be enhanced with dropdowns for specific fields)
        new_value = tk.simpledialog.askstring("Edit", f"{key}:", initialvalue=current_value)

        if new_value is not None and new_value != current_value:
            self.config_data[section][key] = new_value
            self.config_modified = True
            self.display_config()
            self.update_changes_status()

    def revert_changes(self):
        """Revert all changes"""
        if not self.config_modified:
            messagebox.showinfo("Info", "No changes to revert")
            return

        if messagebox.askyesno("Confirm", "Revert all changes?"):
            self.config_data = deepcopy(self.original_config)
            self.config_modified = False
            self.display_config()
            self.update_changes_status()
            self.log("Changes reverted", 'INFO')

    def update_changes_status(self):
        """Update changes status label"""
        if self.config_modified:
            self.changes_label.config(text="Modified", foreground="orange")
            self.write_btn.config(state='normal')
        else:
            self.changes_label.config(text="No changes", foreground="green")
            self.write_btn.config(state='disabled')

    def get_changed_count(self):
        """Get count of changed items"""
        count = 0
        for section in self.config_data:
            if section in self.original_config:
                for key, value in self.config_data[section].items():
                    if self.original_config[section].get(key, '') != value:
                        count += 1
        return count

    # ===== File Management Methods =====

    def save_to_file(self):
        """Save configuration to JSON file"""
        filename = filedialog.asksaveasfilename(
            defaultextension=".json",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )

        if filename:
            try:
                save_data = deepcopy(self.config_data)

                # Include whitelist
                if self.can_whitelist_ids:
                    if 'LAN_CONFIG' not in save_data:
                        save_data['LAN_CONFIG'] = {}
                    save_data['LAN_CONFIG']['can_whitelist_ids'] = self.can_whitelist_ids

                # Include LoRa configs
                if self.lora_modem_config:
                    if 'LORA_CONFIG' not in save_data:
                        save_data['LORA_CONFIG'] = {}
                    save_data['LORA_CONFIG']['modem'] = {
                        'save_on_power_down': self.lora_modem_config.save_on_power_down,
                        'device_address': self.lora_modem_config.device_address,
                        'uart_parity': self.lora_modem_config.uart_parity,
                        'uart_baud_rate': self.lora_modem_config.uart_baud_rate,
                        'air_data_rate': self.lora_modem_config.air_data_rate,
                        'channel': self.lora_modem_config.channel,
                        'transmission_power': self.lora_modem_config.transmission_power,
                        'fec_switch': self.lora_modem_config.fec_switch,
                        'transmission_mode': self.lora_modem_config.transmission_mode,
                        'io_drive_mode': self.lora_modem_config.io_drive_mode,
                        'wireless_wakeup_time': self.lora_modem_config.wireless_wakeup_time
                    }

                if self.lora_tdma_config:
                    if 'LORA_CONFIG' not in save_data:
                        save_data['LORA_CONFIG'] = {}
                    save_data['LORA_CONFIG']['tdma'] = {
                        'role': self.lora_tdma_config.role,
                        'node_id': self.lora_tdma_config.node_id,
                        'gateway_id': self.lora_tdma_config.gateway_id,
                        'num_slots': self.lora_tdma_config.num_slots,
                        'my_slot': self.lora_tdma_config.my_slot,
                        'slot_duration_ms': self.lora_tdma_config.slot_duration_ms
                    }

                with open(filename, 'w', encoding='utf-8') as f:
                    json.dump(save_data, f, indent=2, ensure_ascii=False)

                self.log(f"Configuration saved to {filename}", 'SUCCESS')
                messagebox.showinfo("Success", "Configuration saved")

            except Exception as e:
                self.log(f"Save failed: {e}", 'ERROR')
                messagebox.showerror("Error", str(e))

    def load_from_file(self):
        """Load configuration from JSON file"""
        filename = filedialog.askopenfilename(
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )

        if filename:
            try:
                with open(filename, 'r', encoding='utf-8') as f:
                    loaded_config = json.load(f)

                # Merge basic config
                for section, params in loaded_config.items():
                    if section == 'LORA_CONFIG':
                        continue  # Handle separately
                    if section not in self.config_data:
                        self.config_data[section] = {}
                    self.config_data[section].update(params)

                # Load whitelist
                if 'can_whitelist_ids' in loaded_config.get('LAN_CONFIG', {}):
                    self.can_whitelist_ids = loaded_config['LAN_CONFIG']['can_whitelist_ids']
                    self.update_whitelist_display()

                # Load LoRa configs
                if 'LORA_CONFIG' in loaded_config:
                    if 'modem' in loaded_config['LORA_CONFIG']:
                        modem_data = loaded_config['LORA_CONFIG']['modem']
                        self.lora_modem_config = LoRaE32Config(**modem_data)

                    if 'tdma' in loaded_config['LORA_CONFIG']:
                        tdma_data = loaded_config['LORA_CONFIG']['tdma']
                        self.lora_tdma_config = LoRaTDMAConfig(**tdma_data)

                    self.update_lora_display()

                self.config_modified = True
                self.display_config()
                self.save_btn.config(state='normal')
                self.update_changes_status()

                self.log(f"Configuration loaded from {filename}", 'SUCCESS')
                messagebox.showinfo("Success", "Configuration loaded")

            except Exception as e:
                self.log(f"Load failed: {e}", 'ERROR')
                messagebox.showerror("Error", str(e))

    def update_firmware(self):
        """Update firmware"""
        flash_script = self.app_path / "flash_WAN.bat"
        if not flash_script.exists():
            error_msg = (
                f"flash_WAN.bat not found!\n\n"
                f"Expected: {flash_script}\n\n"
                f"Place flash_WAN.bat and .bin files in:\n{self.app_path}"
            )
            messagebox.showerror("Error", error_msg)
            return

        # Disconnect if connected
        port = None
        if self.serial_port and self.serial_port.is_open:
            port = self.serial_port.port
            self.disconnect()
            threading.Event().wait(1)
        else:
            port = self.port_combo.get()

        if not port:
            messagebox.showerror("Error", "Please select a COM port")
            return

        if not messagebox.askyesno("Confirm Firmware Update",
                                    f"Update firmware for both WAN and LAN MCU?\n\nPort: {port}"):
            return

        self.log("Firmware update feature - to be implemented", 'INFO')
        messagebox.showinfo("Info", "Firmware update feature")

    def clear_config(self):
        """Clear all configuration data"""
        if self.config_modified or self.get_changed_count() > 0:
            if not messagebox.askyesno("Confirm", "Clear all data? Unsaved changes will be lost."):
                return

        self.config_data = {}
        self.original_config = {}
        self.config_modified = False
        self.can_whitelist_ids = []
        self.lora_modem_config = None
        self.lora_tdma_config = None

        self.update_whitelist_display()
        self.update_lora_display()
        self.raw_text.delete('1.0', tk.END)
        self.tree.delete(*self.tree.get_children())
        self.save_btn.config(state='disabled')
        self.write_btn.config(state='disabled')
        self.update_changes_status()

        self.log("Configuration cleared")

def main():
    """Main entry point"""
    root = tk.Tk()
    root.option_add("*Font", ('Segoe UI', 9))

    app = GatewayConfigTool(root)

    def on_closing():
        if app.serial_port and app.serial_port.is_open:
            app.disconnect()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_closing)
    root.mainloop()

if __name__ == '__main__':
    main()
