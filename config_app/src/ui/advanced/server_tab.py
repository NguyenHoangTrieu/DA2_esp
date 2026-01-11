"""
Server Configuration Tab (Advanced)
MQTT/HTTP/CoAP server settings and topics
"""

import tkinter as tk
from tkinter import ttk, messagebox
import threading


class ServerTab(ttk.Frame):
    """Advanced Server configuration tab"""
    
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def _create_widgets(self):
        """Create Server tab widgets"""
        # Container with padding
        container = ttk.Frame(self, padding=10)
        container.pack(fill=tk.X, anchor="nw")
        
        # ═══════════════════════════════════════════════════════════════════
        # Server Type Section
        # ═══════════════════════════════════════════════════════════════════
        type_frame = ttk.LabelFrame(container, text="Server Type", padding=8)
        type_frame.pack(fill=tk.X, pady=5)
        
        row1 = ttk.Frame(type_frame)
        row1.pack(fill=tk.X, pady=2)
        ttk.Label(row1, text="Type:", width=15).pack(side=tk.LEFT)
        self.type_var = tk.StringVar(value="MQTT")
        self.type_combo = ttk.Combobox(row1, textvariable=self.type_var, state="readonly",
                                       values=["MQTT"])  # Only MQTT implemented currently
        self.type_combo.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        self.type_combo.bind("<<ComboboxSelected>>", self._on_type_change)
        
        # ═══════════════════════════════════════════════════════════════════
        # MQTT Settings Section
        # ═══════════════════════════════════════════════════════════════════
        self.mqtt_frame = ttk.LabelFrame(container, text="MQTT Settings", padding=8)
        self.mqtt_frame.pack(fill=tk.X, pady=5)
        
        # Broker URI
        row2 = ttk.Frame(self.mqtt_frame)
        row2.pack(fill=tk.X, pady=2)
        ttk.Label(row2, text="Broker URI:", width=15).pack(side=tk.LEFT)
        self.broker_var = tk.StringVar()
        ttk.Entry(row2, textvariable=self.broker_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Device Token
        row3 = ttk.Frame(self.mqtt_frame)
        row3.pack(fill=tk.X, pady=2)
        ttk.Label(row3, text="Device Token:", width=15).pack(side=tk.LEFT)
        self.token_var = tk.StringVar()
        ttk.Entry(row3, textvariable=self.token_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Info
        row_info = ttk.Frame(self.mqtt_frame)
        row_info.pack(fill=tk.X, pady=2)
        ttk.Label(row_info, text="", width=15).pack(side=tk.LEFT)
        ttk.Label(row_info, text="ℹ️ Format: mqtt[s]://host:port (e.g., mqtt://broker.example.com:1883)",
                  foreground="#757575").pack(side=tk.LEFT)
        
        # ═══════════════════════════════════════════════════════════════════
        # MQTT Topics Section
        # ═══════════════════════════════════════════════════════════════════
        self.topics_frame = ttk.LabelFrame(container, text="MQTT Topics", padding=8)
        self.topics_frame.pack(fill=tk.X, pady=5)
        
        # Subscribe Topic
        row4 = ttk.Frame(self.topics_frame)
        row4.pack(fill=tk.X, pady=2)
        ttk.Label(row4, text="Subscribe Topic:", width=15).pack(side=tk.LEFT)
        self.sub_topic_var = tk.StringVar()
        ttk.Entry(row4, textvariable=self.sub_topic_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Publish Topic
        row5 = ttk.Frame(self.topics_frame)
        row5.pack(fill=tk.X, pady=2)
        ttk.Label(row5, text="Publish Topic:", width=15).pack(side=tk.LEFT)
        self.pub_topic_var = tk.StringVar()
        ttk.Entry(row5, textvariable=self.pub_topic_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Attribute Topic
        row6 = ttk.Frame(self.topics_frame)
        row6.pack(fill=tk.X, pady=2)
        ttk.Label(row6, text="Attribute Topic:", width=15).pack(side=tk.LEFT)
        self.attr_topic_var = tk.StringVar()
        ttk.Entry(row6, textvariable=self.attr_topic_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # ═══════════════════════════════════════════════════════════════════
        # Set Button
        # ═══════════════════════════════════════════════════════════════════
        btn_frame = ttk.Frame(container)
        btn_frame.pack(fill=tk.X, pady=15, anchor="nw")
        ttk.Button(btn_frame, text="✅ Set Server Config", style='Set.TButton',
                  command=self._set_server_config).pack(anchor="e", padx=5)
        
        # ═══════════════════════════════════════════════════════════════════
        # Command Info
        # ═══════════════════════════════════════════════════════════════════
        info_frame = ttk.Frame(container)
        info_frame.pack(fill=tk.X, anchor="nw")
        ttk.Separator(info_frame, orient='horizontal').pack(fill=tk.X, pady=5)
        ttk.Label(info_frame, text="Commands: CFSV:TYPE + CFMQ:BROKER|TOKEN|SUB|PUB|ATTR",
                  foreground="#757575", font=('Consolas', 9)).pack(anchor="w")
        ttk.Label(info_frame, text="TYPE: 0=MQTT, 1=HTTP, 2=CoAP",
                  foreground="#757575", font=('Consolas', 9)).pack(anchor="w")
    
    def _on_type_change(self, event=None):
        """Handle server type change"""
        server_type = self.type_var.get()
        if server_type == "MQTT":
            self.mqtt_frame.pack(fill=tk.X, pady=5, anchor="nw", after=self.mqtt_frame.master.winfo_children()[0])
            self.topics_frame.pack(fill=tk.X, pady=5, anchor="nw", after=self.mqtt_frame)
        else:
            # Hide MQTT-specific fields for HTTP/CoAP
            self.mqtt_frame.pack_forget()
            self.topics_frame.pack_forget()
    
    def _check_connection(self) -> bool:
        """Check if serial is connected"""
        if not self.serial_manager or not self.serial_manager.is_connected():
            messagebox.showwarning("Warning", "Not connected to gateway")
            return False
        return True
    
    def _send_command(self, cmd: str, description: str):
        """Send command without waiting for response"""
        self.log(f"→ {cmd}", "DEBUG")
        if self.serial_manager.send(cmd):
            self.log(f"✓ {description} - Sent", "SUCCESS")
        else:
            self.log(f"✗ {description} - Send failed", "ERROR")
    
    def _set_server_config(self):
        """Set Server configuration"""
        if not self._check_connection():
            return
        
        server_type = self.type_var.get()
        
        # Server type mapping
        type_map = {"MQTT": 0, "HTTP": 1, "CoAP": 2}
        type_code = type_map.get(server_type, 0)
        
        # Send server type command
        self._send_command(f"CFSV:{type_code}", f"Set Server Type = {server_type}")
        
        # If MQTT, send MQTT settings
        if server_type == "MQTT":
            broker = self.broker_var.get().strip()
            token = self.token_var.get().strip()
            sub_topic = self.sub_topic_var.get().strip()
            pub_topic = self.pub_topic_var.get().strip()
            attr_topic = self.attr_topic_var.get().strip()
            
            # CFMQ:BROKER|TOKEN|SUB|PUB|ATTR
            cmd = f"CFMQ:{broker}|{token}|{sub_topic}|{pub_topic}|{attr_topic}"
            self._send_command(cmd, "MQTT Config")
    
    def set_config(self, config):
        """Set config from data"""
        if hasattr(config, 'server_type'):
            type_map = {0: "MQTT", 1: "HTTP", 2: "CoAP"}
            self.type_var.set(type_map.get(config.server_type, "MQTT"))
            self._on_type_change()
        if hasattr(config, 'mqtt_broker'):
            self.broker_var.set(config.mqtt_broker or "")
        if hasattr(config, 'device_token'):
            self.token_var.set(config.device_token or "")
        if hasattr(config, 'mqtt_sub_topic'):
            self.sub_topic_var.set(config.mqtt_sub_topic or "")
        if hasattr(config, 'mqtt_pub_topic'):
            self.pub_topic_var.set(config.mqtt_pub_topic or "")
        if hasattr(config, 'mqtt_attr_topic'):
            self.attr_topic_var.set(config.mqtt_attr_topic or "")
    
    def get_config(self) -> dict:
        """Get config as dict"""
        type_map = {"MQTT": 0, "HTTP": 1, "CoAP": 2}
        return {
            'server_type': type_map.get(self.type_var.get(), 0),
            'mqtt_broker': self.broker_var.get(),
            'device_token': self.token_var.get(),
            'mqtt_sub_topic': self.sub_topic_var.get(),
            'mqtt_pub_topic': self.pub_topic_var.get(),
            'mqtt_attr_topic': self.attr_topic_var.get(),
        }
