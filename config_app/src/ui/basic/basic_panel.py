"""
Basic Mode Panel for ESP32 Gateway Configuration Tool
Simple configuration interface with 4 tabs
"""

import tkinter as tk
from tkinter import ttk, messagebox
from typing import Callable, Optional
import threading
import time

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(__file__))))

from src.config.protocol import GatewayConfig


class BasicPanel(ttk.Frame):
    """Basic mode configuration panel with tabs"""
    
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def _create_widgets(self):
        """Create tabbed interface"""
        # Title
        title_frame = ttk.Frame(self)
        title_frame.pack(fill=tk.X, padx=10, pady=5)
        
        ttk.Label(title_frame, text="📋 BASIC CONFIGURATION", 
                 font=("Segoe UI", 12, "bold")).pack(anchor="w")
        
        # Notebook with 4 tabs (no expand to avoid whitespace)
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill=tk.X, padx=5, pady=5)
        
        # Create tabs
        self._create_wifi_tab()
        self._create_lte_tab()
        self._create_server_tab()
        self._create_interfaces_tab()
    
    def _create_wifi_tab(self):
        """Create WiFi configuration tab"""
        tab = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(tab, text="📶 WiFi")
        
        # WiFi Settings LabelFrame
        wifi_frame = ttk.LabelFrame(tab, text="WiFi Settings", padding=10)
        wifi_frame.pack(fill=tk.X, pady=5)
        
        # SSID
        row1 = ttk.Frame(wifi_frame)
        row1.pack(fill=tk.X, pady=3)
        ttk.Label(row1, text="SSID:", width=15).pack(side=tk.LEFT)
        self.wifi_ssid_var = tk.StringVar()
        ttk.Entry(row1, textvariable=self.wifi_ssid_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Password
        row2 = ttk.Frame(wifi_frame)
        row2.pack(fill=tk.X, pady=3)
        ttk.Label(row2, text="Password:", width=15).pack(side=tk.LEFT)
        
        # Pack Checkbutton first (right side)
        self.show_pwd_var = tk.BooleanVar()
        ttk.Checkbutton(row2, text="Show", variable=self.show_pwd_var,
                       command=lambda: self.wifi_pwd_entry.config(show="" if self.show_pwd_var.get() else "*")
                       ).pack(side=tk.RIGHT, padx=5)
        
        # Then Entry fills remaining space
        self.wifi_pwd_var = tk.StringVar()
        self.wifi_pwd_entry = ttk.Entry(row2, textvariable=self.wifi_pwd_var, show="*")
        self.wifi_pwd_entry.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Auth Mode - only PERSONAL (0) or ENTERPRISE (1)
        row3 = ttk.Frame(wifi_frame)
        row3.pack(fill=tk.X, pady=3)
        ttk.Label(row3, text="Auth Mode:", width=15).pack(side=tk.LEFT)
        self.wifi_auth_var = tk.StringVar(value="PERSONAL")
        auth_combo = ttk.Combobox(row3, textvariable=self.wifi_auth_var, state="readonly",
                                  values=["PERSONAL", "ENTERPRISE"])
        auth_combo.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        auth_combo.bind("<<ComboboxSelected>>", self._on_wifi_auth_change)
        
        # Username (Enterprise only) - Initially hidden
        self.wifi_username_frame = ttk.Frame(wifi_frame)
        # Don't pack initially - hidden when PERSONAL
        ttk.Label(self.wifi_username_frame, text="Username:", width=15).pack(side=tk.LEFT)
        self.wifi_username_var = tk.StringVar()
        ttk.Entry(self.wifi_username_frame, textvariable=self.wifi_username_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Set Button
        btn_frame = ttk.Frame(tab)
        btn_frame.pack(fill=tk.X, pady=10)
        ttk.Button(btn_frame, text="Set WiFi Config", style='Set.TButton',
                  command=self._set_wifi_config).pack(anchor="e", padx=5)
    
    def _on_wifi_auth_change(self, event=None):
        """Handle WiFi auth mode change - show/hide username field"""
        if self.wifi_auth_var.get() == "ENTERPRISE":
            self.wifi_username_frame.pack(fill=tk.X, pady=3)
        else:
            self.wifi_username_frame.pack_forget()
            self.wifi_username_var.set("")
    
    def _create_lte_tab(self):
        """Create LTE configuration tab"""
        tab = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(tab, text="📱 LTE")
        
        # LTE Settings LabelFrame
        lte_frame = ttk.LabelFrame(tab, text="LTE Settings", padding=10)
        lte_frame.pack(fill=tk.X, pady=5)
        
        # APN
        row1 = ttk.Frame(lte_frame)
        row1.pack(fill=tk.X, pady=3)
        ttk.Label(row1, text="APN:", width=15).pack(side=tk.LEFT)
        self.lte_apn_var = tk.StringVar(value="internet")
        ttk.Entry(row1, textvariable=self.lte_apn_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Username
        row2 = ttk.Frame(lte_frame)
        row2.pack(fill=tk.X, pady=3)
        ttk.Label(row2, text="Username:", width=15).pack(side=tk.LEFT)
        self.lte_user_var = tk.StringVar()
        ttk.Entry(row2, textvariable=self.lte_user_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Password
        row3 = ttk.Frame(lte_frame)
        row3.pack(fill=tk.X, pady=3)
        ttk.Label(row3, text="Password:", width=15).pack(side=tk.LEFT)
        self.lte_pwd_var = tk.StringVar()
        self.lte_pwd_entry = ttk.Entry(row3, textvariable=self.lte_pwd_var, show="*")
        self.lte_pwd_entry.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        self.show_lte_pwd_var = tk.BooleanVar()
        ttk.Checkbutton(row3, text="Show", variable=self.show_lte_pwd_var,
                       command=lambda: self.lte_pwd_entry.config(show="" if self.show_lte_pwd_var.get() else "*")
                       ).pack(side=tk.LEFT, padx=5)
        
        # Set Button
        btn_frame = ttk.Frame(tab)
        btn_frame.pack(fill=tk.X, pady=10)
        ttk.Button(btn_frame, text="Set LTE Config", style='Set.TButton',
                  command=self._set_lte_config).pack(anchor="e", padx=5)
    
    def _create_server_tab(self):
        """Create Server configuration tab"""
        tab = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(tab, text="🌐 Server")
        
        # MQTT Settings LabelFrame  
        mqtt_frame = ttk.LabelFrame(tab, text="MQTT Settings", padding=10)
        mqtt_frame.pack(fill=tk.X, pady=5)
        
        # Broker
        row1 = ttk.Frame(mqtt_frame)
        row1.pack(fill=tk.X, pady=3)
        ttk.Label(row1, text="Broker:", width=15).pack(side=tk.LEFT)
        self.mqtt_broker_var = tk.StringVar(value="mqtt.thingsboard.cloud")
        ttk.Entry(row1, textvariable=self.mqtt_broker_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Device Token
        row2 = ttk.Frame(mqtt_frame)
        row2.pack(fill=tk.X, pady=3)
        ttk.Label(row2, text="Device Token:", width=15).pack(side=tk.LEFT)
        
        # Pack Checkbutton first (right side)
        self.show_token_var = tk.BooleanVar()
        ttk.Checkbutton(row2, text="Show", variable=self.show_token_var,
                       command=lambda: self.mqtt_token_entry.config(show="" if self.show_token_var.get() else "*")
                       ).pack(side=tk.RIGHT, padx=5)
        
        # Then Entry fills remaining space
        self.mqtt_token_var = tk.StringVar()
        self.mqtt_token_entry = ttk.Entry(row2, textvariable=self.mqtt_token_var, show="*")
        self.mqtt_token_entry.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Help text
        ttk.Label(mqtt_frame, text="💡 Get token from ThingsBoard dashboard",
                 font=("Segoe UI", 9), foreground="#757575").pack(anchor="w", pady=(5, 0))
        
        # Set Button
        btn_frame = ttk.Frame(tab)
        btn_frame.pack(fill=tk.X, pady=10)
        ttk.Button(btn_frame, text="Set Server Config", style='Set.TButton',
                  command=self._set_server_config).pack(anchor="e", padx=5)
    
    def _create_interfaces_tab(self):
        """Create Interfaces configuration tab"""
        tab = ttk.Frame(self.notebook, padding=10)
        self.notebook.add(tab, text="🔌 Interfaces")
        
        # Stack Settings LabelFrame
        stack_frame = ttk.LabelFrame(tab, text="Communication Stacks", padding=10)
        stack_frame.pack(fill=tk.X, pady=5)
        
        stack_options = ["NONE", "LORA", "RS485", "ZIGBEE", "CAN"]
        
        # Stack 1
        row1 = ttk.Frame(stack_frame)
        row1.pack(fill=tk.X, pady=3)
        ttk.Label(row1, text="Stack 1:", width=15).pack(side=tk.LEFT)
        self.stack1_var = tk.StringVar(value="NONE")
        ttk.Combobox(row1, textvariable=self.stack1_var, values=stack_options,
                    state="readonly").pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Stack 2
        row2 = ttk.Frame(stack_frame)
        row2.pack(fill=tk.X, pady=3)
        ttk.Label(row2, text="Stack 2:", width=15).pack(side=tk.LEFT)
        self.stack2_var = tk.StringVar(value="NONE")
        ttk.Combobox(row2, textvariable=self.stack2_var, values=stack_options,
                    state="readonly").pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Set Button
        btn_frame = ttk.Frame(tab)
        btn_frame.pack(fill=tk.X, pady=10)
        ttk.Button(btn_frame, text="Set Interfaces", style='Set.TButton',
                  command=self._set_interfaces_config).pack(anchor="e", padx=5)
    
    def _check_connection(self) -> bool:
        """Check if serial is connected"""
        if not self.serial_manager or not self.serial_manager.is_connected():
            messagebox.showwarning("Warning", "Not connected to gateway")
            return False
        return True
    
    def _send_command(self, cmd: str, description: str):
        """Send command without waiting for response"""
        self.log(f"Sending: {description}", "INFO")
        if self.serial_manager.send(cmd):
            self.log(f"{description} - Sent", "SUCCESS")
        else:
            self.log(f"{description} - Send failed", "ERROR")
    
    def _set_wifi_config(self):
        """Set WiFi configuration"""
        if not self._check_connection():
            return
        
        ssid = self.wifi_ssid_var.get().strip()
        password = self.wifi_pwd_var.get()
        auth = self.wifi_auth_var.get()
        username = self.wifi_username_var.get().strip()
        
        if not ssid:
            messagebox.showwarning("Warning", "Please enter WiFi SSID")
            return
        
        # Validate username for Enterprise mode
        if auth == "ENTERPRISE" and not username:
            messagebox.showwarning("Warning", "Please enter Username for Enterprise mode")
            return
        
        # Build CFWF command: CFWF:SSID:PASSWORD:AUTH_MODE or CFWF:SSID:PASSWORD:USERNAME:AUTH_MODE
        if auth == "ENTERPRISE":
            cmd = f"CFWF:{ssid}:{password}:{username}:ENTERPRISE"
        else:
            cmd = f"CFWF:{ssid}:{password}:PERSONAL"
        
        # Send CFWF first, then CFIN:WIFI after 500ms delay (no response waiting)
        def send_wifi_sequence():
            import time
            self.log(f"Sending: WiFi Config", "INFO")
            self.serial_manager.send(cmd)
            self.log(f"WiFi Config - Sent", "SUCCESS")
            
            # Wait 500ms before sending CFIN:WIFI
            time.sleep(0.5)
            
            self.log(f"Sending: Set Internet Type = WIFI", "INFO")
            self.serial_manager.send("CFIN:WIFI")
            self.log(f"Set Internet Type = WIFI - Sent", "SUCCESS")
        
        thread = threading.Thread(target=send_wifi_sequence)
        thread.daemon = True
        thread.start()
    
    def _set_lte_config(self):
        """Set LTE configuration"""
        if not self._check_connection():
            return
        
        apn = self.lte_apn_var.get().strip()
        user = self.lte_user_var.get().strip()
        pwd = self.lte_pwd_var.get()
        
        if not apn:
            messagebox.showwarning("Warning", "Please enter APN")
            return
        
        # Build CFLT command: CFLT:APN:USERNAME:PASSWORD:COMM_TYPE:AUTO_RECONNECT:TIMEOUT:MAX_RETRY
        # Basic mode defaults: UART, false, 30000, 0
        cmd = f"CFLT:{apn}:{user}:{pwd}:UART:false:30000:0"
        
        # Send CFLT first, then CFIN:LTE after 500ms delay (no response waiting)
        def send_lte_sequence():
            import time
            self.log(f"Sending: LTE Config", "INFO")
            self.serial_manager.send(cmd)
            self.log(f"LTE Config - Sent", "SUCCESS")
            
            # Wait 500ms before sending CFIN:LTE
            time.sleep(0.5)
            
            self.log(f"Sending: Set Internet Type = LTE", "INFO")
            self.serial_manager.send("CFIN:LTE")
            self.log(f"Set Internet Type = LTE - Sent", "SUCCESS")
        
        thread = threading.Thread(target=send_lte_sequence)
        thread.daemon = True
        thread.start()
    
    def _set_server_config(self):
        """Set Server configuration"""
        if not self._check_connection():
            return
        
        broker = self.mqtt_broker_var.get().strip()
        token = self.mqtt_token_var.get().strip()
        
        if not broker:
            messagebox.showwarning("Warning", "Please enter MQTT broker")
            return
        
        # Default topics
        sub_topic = "v1/devices/me/rpc/request/+"
        pub_topic = "v1/devices/me/telemetry"
        attr_topic = "v1/devices/me/attributes"
        
        # Send CFMQ command: CFMQ:BROKER|TOKEN|SUB_TOPIC|PUB_TOPIC|ATTR_TOPIC
        cmd = f"CFMQ:{broker}|{token}|{sub_topic}|{pub_topic}|{attr_topic}"
        self._send_command(cmd, "MQTT Config")
    
    def _set_interfaces_config(self):
        """Set Interfaces configuration"""
        if not self._check_connection():
            return
        
        stack1 = self.stack1_var.get()
        stack2 = self.stack2_var.get()
        
        # Send CFML:CFST commands for each stack (CFML prefix for LAN MCU)
        # CFML:CFST:ST_1:TYPE or CFML:CFST:ST_2:TYPE
        self._send_command(f"CFML:CFST:ST_1:{stack1}", f"Stack 1 = {stack1}")
        
        # 500ms delay between commands
        time.sleep(0.5)
        
        self._send_command(f"CFML:CFST:ST_2:{stack2}", f"Stack 2 = {stack2}")
    
    def set_config(self, config: GatewayConfig):
        """Set config values from loaded config"""
        # WiFi
        self.wifi_ssid_var.set(config.wan.wifi_ssid or "")
        if config.wan.wifi_password and config.wan.wifi_password != "***HIDDEN***":
            self.wifi_pwd_var.set(config.wan.wifi_password)
        
        # LTE
        self.lte_apn_var.set(config.wan.lte_apn or "internet")
        
        # Server
        self.mqtt_broker_var.set(config.wan.mqtt_broker or "mqtt.thingsboard.cloud")
        if config.wan.mqtt_device_token and config.wan.mqtt_device_token != "***HIDDEN***":
            self.mqtt_token_var.set(config.wan.mqtt_device_token)
        
        # Interfaces
        self.stack1_var.set(config.lan.stack.stack_1_type or "NONE")
        self.stack2_var.set(config.lan.stack.stack_2_type or "NONE")
