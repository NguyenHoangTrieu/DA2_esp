"""
RS485 Configuration Tab
"""

import tkinter as tk
from tkinter import ttk, messagebox
import threading

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(__file__)))))

from src.config.protocol import Rs485Config


class Rs485Tab(ttk.Frame):
    """RS485 configuration tab"""
    
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def _create_widgets(self):
        """Create RS485 tab widgets"""
        # Container - no expand to avoid whitespace
        container = ttk.Frame(self, padding=10)
        container.pack(fill=tk.X, anchor="nw")
        
        # RS485 Settings - compact
        section = ttk.LabelFrame(container, text="RS485 Settings", padding=8)
        section.pack(fill=tk.X, pady=5)
        
        # Baud Rate
        baud_frame = ttk.Frame(section)
        baud_frame.pack(pady=2, anchor="w")
        
        ttk.Label(baud_frame, text="Baud Rate:", width=12).pack(side=tk.LEFT)
        self.baud_var = tk.StringVar(value="115200")
        baud_combo = ttk.Combobox(baud_frame, textvariable=self.baud_var,
                                  values=["9600", "19200", "38400", "57600", "115200"],
                                  state="readonly", width=15)
        baud_combo.pack(side=tk.LEFT, padx=5)
        
        # Info
        ttk.Label(section, text="Data format: 8N1",
                 font=("Segoe UI", 9), foreground="#757575").pack(anchor="w", pady=3)
        
        # Set Button
        btn_frame = ttk.Frame(container)
        btn_frame.pack(fill=tk.X, pady=10)
        ttk.Button(btn_frame, text="Set RS485 Config", style='Set.TButton',
                  command=self._set_rs485_config).pack(anchor="e", padx=5)
    
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
    
    def _set_rs485_config(self):
        """Set RS485 configuration"""
        if not self._check_connection():
            return
        
        baud = self.baud_var.get()
        
        # CFRS:BR:baudrate - RS485 Baud rate
        self._send_command(f"CFRS:BR:{baud}", f"RS485 Baud = {baud}")
    
    def get_config(self) -> Rs485Config:
        """Get RS485 config from UI"""
        config = Rs485Config()
        try:
            config.baud_rate = int(self.baud_var.get())
        except:
            config.baud_rate = 115200
        return config
    
    def set_config(self, config: Rs485Config):
        """Set RS485 config to UI"""
        self.baud_var.set(str(config.baud_rate))
