"""
WAN/Internet Configuration Tab
Complete configuration for WiFi, LTE, and Server settings per implement.md
"""

import tkinter as tk
from tkinter import ttk, messagebox
from typing import Optional
import threading

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(__file__)))))

from src.config.protocol import WanConfig


class WanTab(ttk.Frame):
    """WAN/Internet configuration tab with WiFi, LTE, and Server settings"""
    
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def _create_widgets(self):
        """Create WAN tab widgets"""
        # Scrollable container
        canvas = tk.Canvas(self, highlightthickness=0)
        scrollbar = ttk.Scrollbar(self, orient="vertical", command=canvas.yview)
        scrollable_frame = ttk.Frame(canvas)
        
        scrollable_frame.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all"))
        )
        
        canvas.create_window((0, 0), window=scrollable_frame, anchor="nw")
        canvas.configure(yscrollcommand=scrollbar.set)
        
        # Pack scrollbar and canvas
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        
        # Bind mousewheel
        def _on_mousewheel(event):
            canvas.yview_scroll(int(-1*(event.delta/120)), "units")
        canvas.bind_all("<MouseWheel>", _on_mousewheel)
        
        # Container with padding
        container = ttk.Frame(scrollable_frame, padding=10)
        container.pack(fill=tk.X, expand=True)
        
        # ═══════════════════════════════════════════════════════════════════
        # Internet Type Section
        # ═══════════════════════════════════════════════════════════════════
        type_frame = ttk.LabelFrame(container, text="Internet Type", padding=8)
        type_frame.pack(fill=tk.X, pady=5, anchor="nw")
        
        row = ttk.Frame(type_frame)
        row.pack(fill=tk.X, pady=3)
        ttk.Label(row, text="Type:", width=15).pack(side=tk.LEFT)
        self.internet_type_var = tk.StringVar(value="WIFI")
        ttk.Combobox(row, textvariable=self.internet_type_var,
                    values=["WIFI", "LTE"], state="readonly", width=18).pack(side=tk.LEFT, padx=5)
        
        # ═══════════════════════════════════════════════════════════════════
        # WiFi Configuration Section
        # ═══════════════════════════════════════════════════════════════════
        wifi_frame = ttk.LabelFrame(container, text="WiFi Configuration", padding=8)
        wifi_frame.pack(fill=tk.X, pady=5, anchor="nw")
        
        # SSID
        row1 = ttk.Frame(wifi_frame)
        row1.pack(fill=tk.X, pady=2)
        ttk.Label(row1, text="SSID:", width=15).pack(side=tk.LEFT)
        self.wifi_ssid_var = tk.StringVar()
        ttk.Entry(row1, textvariable=self.wifi_ssid_var, width=28).pack(side=tk.LEFT, padx=5)
        
        # Password
        row2 = ttk.Frame(wifi_frame)
        row2.pack(fill=tk.X, pady=2)
        ttk.Label(row2, text="Password:", width=15).pack(side=tk.LEFT)
        self.wifi_pwd_var = tk.StringVar()
        self.wifi_pwd_entry = ttk.Entry(row2, textvariable=self.wifi_pwd_var, width=28, show="*")
        self.wifi_pwd_entry.pack(side=tk.LEFT, padx=5)
        self.show_wifi_pwd = tk.BooleanVar()
        ttk.Checkbutton(row2, text="Show", variable=self.show_wifi_pwd,
                       command=lambda: self.wifi_pwd_entry.config(show="" if self.show_wifi_pwd.get() else "*")
                       ).pack(side=tk.LEFT)
        
        # Auth Mode - PERSONAL (0) or ENTERPRISE (1)
        row3 = ttk.Frame(wifi_frame)
        row3.pack(fill=tk.X, pady=2)
        ttk.Label(row3, text="Auth Mode:", width=15).pack(side=tk.LEFT)
        self.wifi_auth_var = tk.StringVar(value="PERSONAL")
        self.wifi_auth_combo = ttk.Combobox(row3, textvariable=self.wifi_auth_var, width=25, state="readonly",
                    values=["PERSONAL", "ENTERPRISE"])
        self.wifi_auth_combo.pack(side=tk.LEFT, padx=5)
        self.wifi_auth_combo.bind("<<ComboboxSelected>>", self._on_auth_change)
        
        # Username (Enterprise only)
        row3b = ttk.Frame(wifi_frame)
        row3b.pack(fill=tk.X, pady=2)
        ttk.Label(row3b, text="Username:", width=15).pack(side=tk.LEFT)
        self.wifi_user_var = tk.StringVar()
        self.wifi_user_entry = ttk.Entry(row3b, textvariable=self.wifi_user_var, width=28)
        self.wifi_user_entry.pack(side=tk.LEFT, padx=5)
        self.enterprise_label = ttk.Label(row3b, text="(Enterprise only)", foreground="#757575")
        self.enterprise_label.pack(side=tk.LEFT)
        self.wifi_user_entry.config(state="disabled")
        
        # ═══════════════════════════════════════════════════════════════════
        # LTE Configuration Section
        # ═══════════════════════════════════════════════════════════════════
        lte_frame = ttk.LabelFrame(container, text="LTE Configuration", padding=8)
        lte_frame.pack(fill=tk.X, pady=5, anchor="nw")
        
        # APN
        row4 = ttk.Frame(lte_frame)
        row4.pack(fill=tk.X, pady=2)
        ttk.Label(row4, text="APN:", width=15).pack(side=tk.LEFT)
        self.lte_apn_var = tk.StringVar(value="internet")
        ttk.Entry(row4, textvariable=self.lte_apn_var, width=28).pack(side=tk.LEFT, padx=5)
        
        # Username
        row5 = ttk.Frame(lte_frame)
        row5.pack(fill=tk.X, pady=2)
        ttk.Label(row5, text="Username:", width=15).pack(side=tk.LEFT)
        self.lte_user_var = tk.StringVar()
        ttk.Entry(row5, textvariable=self.lte_user_var, width=28).pack(side=tk.LEFT, padx=5)
        
        # Password
        row6 = ttk.Frame(lte_frame)
        row6.pack(fill=tk.X, pady=2)
        ttk.Label(row6, text="Password:", width=15).pack(side=tk.LEFT)
        self.lte_pwd_var = tk.StringVar()
        self.lte_pwd_entry = ttk.Entry(row6, textvariable=self.lte_pwd_var, width=28, show="*")
        self.lte_pwd_entry.pack(side=tk.LEFT, padx=5)
        self.show_lte_pwd = tk.BooleanVar()
        ttk.Checkbutton(row6, text="Show", variable=self.show_lte_pwd,
                       command=lambda: self.lte_pwd_entry.config(show="" if self.show_lte_pwd.get() else "*")
                       ).pack(side=tk.LEFT)
        
        # Comm Type
        row7 = ttk.Frame(lte_frame)
        row7.pack(fill=tk.X, pady=2)
        ttk.Label(row7, text="Comm Type:", width=15).pack(side=tk.LEFT)
        self.lte_comm_var = tk.StringVar(value="UART")
        ttk.Combobox(row7, textvariable=self.lte_comm_var, width=25, state="readonly",
                    values=["UART", "USB"]).pack(side=tk.LEFT, padx=5)
        
        # Auto Reconnect + Timeout + Max Retry row
        row8 = ttk.Frame(lte_frame)
        row8.pack(fill=tk.X, pady=2)
        
        self.lte_reconnect_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(row8, text="Auto Reconnect", variable=self.lte_reconnect_var).pack(side=tk.LEFT)
        
        ttk.Label(row8, text="   Timeout:").pack(side=tk.LEFT)
        self.lte_timeout_var = tk.StringVar(value="30000")
        ttk.Entry(row8, textvariable=self.lte_timeout_var, width=8).pack(side=tk.LEFT, padx=2)
        ttk.Label(row8, text="ms").pack(side=tk.LEFT)
        
        ttk.Label(row8, text="   Max Retry:").pack(side=tk.LEFT)
        self.lte_retry_var = tk.StringVar(value="5")
        ttk.Entry(row8, textvariable=self.lte_retry_var, width=5).pack(side=tk.LEFT, padx=2)
        
        # ═══════════════════════════════════════════════════════════════════
        # Server Configuration Section
        # ═══════════════════════════════════════════════════════════════════
        server_frame = ttk.LabelFrame(container, text="Server Configuration", padding=8)
        server_frame.pack(fill=tk.X, pady=5, anchor="nw")
        
        # Server Type
        row9 = ttk.Frame(server_frame)
        row9.pack(fill=tk.X, pady=2)
        ttk.Label(row9, text="Server Type:", width=15).pack(side=tk.LEFT)
        self.server_type_var = tk.StringVar(value="MQTT")
        ttk.Combobox(row9, textvariable=self.server_type_var, width=25, state="readonly",
                    values=["MQTT"]).pack(side=tk.LEFT, padx=5)  # Only MQTT implemented
        
        # Broker URI
        row10 = ttk.Frame(server_frame)
        row10.pack(fill=tk.X, pady=2)
        ttk.Label(row10, text="Broker URI:", width=15).pack(side=tk.LEFT)
        self.mqtt_broker_var = tk.StringVar(value="mqtt.thingsboard.cloud")
        ttk.Entry(row10, textvariable=self.mqtt_broker_var, width=35).pack(side=tk.LEFT, padx=5)
        
        # Device Token
        row11 = ttk.Frame(server_frame)
        row11.pack(fill=tk.X, pady=2)
        ttk.Label(row11, text="Device Token:", width=15).pack(side=tk.LEFT)
        self.mqtt_token_var = tk.StringVar()
        self.mqtt_token_entry = ttk.Entry(row11, textvariable=self.mqtt_token_var, width=35, show="*")
        self.mqtt_token_entry.pack(side=tk.LEFT, padx=5)
        self.show_token = tk.BooleanVar()
        ttk.Checkbutton(row11, text="Show", variable=self.show_token,
                       command=lambda: self.mqtt_token_entry.config(show="" if self.show_token.get() else "*")
                       ).pack(side=tk.LEFT)
        
        # Subscribe Topic
        row12 = ttk.Frame(server_frame)
        row12.pack(fill=tk.X, pady=2)
        ttk.Label(row12, text="Subscribe Topic:", width=15).pack(side=tk.LEFT)
        self.mqtt_sub_var = tk.StringVar(value="v1/devices/me/rpc/request/+")
        ttk.Entry(row12, textvariable=self.mqtt_sub_var, width=35).pack(side=tk.LEFT, padx=5)
        
        # Publish Topic
        row13 = ttk.Frame(server_frame)
        row13.pack(fill=tk.X, pady=2)
        ttk.Label(row13, text="Publish Topic:", width=15).pack(side=tk.LEFT)
        self.mqtt_pub_var = tk.StringVar(value="v1/devices/me/telemetry")
        ttk.Entry(row13, textvariable=self.mqtt_pub_var, width=35).pack(side=tk.LEFT, padx=5)
        
        # Attribute Topic
        row14 = ttk.Frame(server_frame)
        row14.pack(fill=tk.X, pady=2)
        ttk.Label(row14, text="Attribute Topic:", width=15).pack(side=tk.LEFT)
        self.mqtt_attr_var = tk.StringVar(value="v1/devices/me/attributes")
        ttk.Entry(row14, textvariable=self.mqtt_attr_var, width=35).pack(side=tk.LEFT, padx=5)
        
        # ═══════════════════════════════════════════════════════════════════
        # Set Button
        # ═══════════════════════════════════════════════════════════════════
        btn_frame = ttk.Frame(container)
        btn_frame.pack(fill=tk.X, pady=10, anchor="nw")
        ttk.Button(btn_frame, text="Set WAN Config", style='Set.TButton',
                  command=self._set_wan_config).pack(anchor="e", padx=5)
    
    def _on_auth_change(self, event=None):
        """Handle auth mode change - enable/disable username field"""
        if self.wifi_auth_var.get() == "ENTERPRISE":
            self.wifi_user_entry.config(state="normal")
        else:
            self.wifi_user_entry.config(state="disabled")
            self.wifi_user_var.set("")
    
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
    
    def _set_wan_config(self):
        """Set WAN configuration - sends commands per implement.md"""
        if not self._check_connection():
            return
        
        inet_type = self.internet_type_var.get()
        
        # ─────────────────────────────────────────────────────────────────
        # 1. Set Internet Type: CFIN:TYPE
        # ─────────────────────────────────────────────────────────────────
        self._send_command(f"CFIN:{inet_type}", f"Internet Type = {inet_type}")
        
        # ─────────────────────────────────────────────────────────────────
        # 2. WiFi Config: CFWF:SSID:PASSWORD:AUTH_MODE
        #    AUTH_MODE: PERSONAL=0, ENTERPRISE=1 (if enterprise, add username)
        # ─────────────────────────────────────────────────────────────────
        if inet_type == "WIFI":
            ssid = self.wifi_ssid_var.get().strip()
            pwd = self.wifi_pwd_var.get()
            auth_mode = self.wifi_auth_var.get()
            
            if ssid:
                if auth_mode == "ENTERPRISE":
                    username = self.wifi_user_var.get().strip()
                    # For enterprise, format: CFWF:SSID:PASSWORD:1:USERNAME
                    cmd = f"CFWF:{ssid}:{pwd}:1:{username}"
                else:
                    # For personal: CFWF:SSID:PASSWORD:0
                    cmd = f"CFWF:{ssid}:{pwd}:0"
                self._send_command(cmd, "WiFi Config")
        
        # ─────────────────────────────────────────────────────────────────
        # 3. LTE Config: CFLT:COMM:APN:USER:PASS:RECONNECT:TIMEOUT:RETRY
        # ─────────────────────────────────────────────────────────────────
        if inet_type == "LTE":
            comm = self.lte_comm_var.get()
            apn = self.lte_apn_var.get().strip()
            user = self.lte_user_var.get().strip()
            pwd = self.lte_pwd_var.get()
            reconnect = "true" if self.lte_reconnect_var.get() else "false"
            timeout = self.lte_timeout_var.get().strip() or "30000"
            retry = self.lte_retry_var.get().strip() or "0"
            
            if apn:
                cmd = f"CFLT:{comm}:{apn}:{user}:{pwd}:{reconnect}:{timeout}:{retry}"
                self._send_command(cmd, "LTE Config")
        
        # ─────────────────────────────────────────────────────────────────
        # 4. Server Type: CFSV:TYPE
        # ─────────────────────────────────────────────────────────────────
        server_type = self.server_type_var.get()
        self._send_command(f"CFSV:{server_type}", f"Server Type = {server_type}")
        
        # ─────────────────────────────────────────────────────────────────
        # 5. MQTT Config: CFMQ:BROKER|TOKEN|SUB|PUB|ATTR
        # ─────────────────────────────────────────────────────────────────
        broker = self.mqtt_broker_var.get().strip()
        token = self.mqtt_token_var.get().strip()
        sub_topic = self.mqtt_sub_var.get().strip()
        pub_topic = self.mqtt_pub_var.get().strip()
        attr_topic = self.mqtt_attr_var.get().strip()
        
        if broker:
            cmd = f"CFMQ:{broker}|{token}|{sub_topic}|{pub_topic}|{attr_topic}"
            self._send_command(cmd, "MQTT Config")
    
    def set_config(self, config: WanConfig):
        """Set WAN config to UI"""
        self.internet_type_var.set(config.internet_type or "WIFI")
        self.wifi_ssid_var.set(config.wifi_ssid or "")
        if config.wifi_password and config.wifi_password != "***HIDDEN***":
            self.wifi_pwd_var.set(config.wifi_password)
        self.wifi_auth_var.set("PERSONAL")  # Default
        self._on_auth_change()
        
        self.lte_apn_var.set(config.lte_apn or "internet")
        self.lte_user_var.set(config.lte_username or "")
        if config.lte_password and config.lte_password != "***HIDDEN***":
            self.lte_pwd_var.set(config.lte_password)
        self.lte_comm_var.set(config.lte_comm_type or "UART")
        self.lte_reconnect_var.set(config.lte_auto_reconnect == "true" if config.lte_auto_reconnect else False)
        self.lte_timeout_var.set(str(config.lte_timeout_ms or 30000))
        self.lte_retry_var.set(str(config.lte_max_retries or 5))
        
        self.server_type_var.set(config.server_type or "MQTT")
        self.mqtt_broker_var.set(config.mqtt_broker or "mqtt.thingsboard.cloud")
        if config.mqtt_device_token and config.mqtt_device_token != "***HIDDEN***":
            self.mqtt_token_var.set(config.mqtt_device_token)
        self.mqtt_sub_var.set(config.mqtt_sub_topic or "v1/devices/me/rpc/request/+")
        self.mqtt_pub_var.set(config.mqtt_pub_topic or "v1/devices/me/telemetry")
        self.mqtt_attr_var.set(config.mqtt_attribute_topic or "v1/devices/me/attributes")
    
    def get_config(self) -> dict:
        """Get WAN config from UI"""
        return {
            'internet_type': self.internet_type_var.get(),
            'wifi_ssid': self.wifi_ssid_var.get(),
            'wifi_password': self.wifi_pwd_var.get(),
            'wifi_auth_mode': 1 if self.wifi_auth_var.get() == "ENTERPRISE" else 0,
            'wifi_username': self.wifi_user_var.get(),
            'lte_apn': self.lte_apn_var.get(),
            'lte_username': self.lte_user_var.get(),
            'lte_password': self.lte_pwd_var.get(),
            'lte_comm_type': self.lte_comm_var.get(),
            'lte_auto_reconnect': self.lte_reconnect_var.get(),
            'lte_timeout_ms': int(self.lte_timeout_var.get() or 30000),
            'lte_max_retries': int(self.lte_retry_var.get() or 5),
            'server_type': self.server_type_var.get(),
            'mqtt_broker': self.mqtt_broker_var.get(),
            'mqtt_device_token': self.mqtt_token_var.get(),
            'mqtt_sub_topic': self.mqtt_sub_var.get(),
            'mqtt_pub_topic': self.mqtt_pub_var.get(),
            'mqtt_attribute_topic': self.mqtt_attr_var.get(),
        }
