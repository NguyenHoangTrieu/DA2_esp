"""
CAN Bus Configuration Tab
"""

import tkinter as tk
from tkinter import ttk, messagebox
import threading

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(__file__)))))

from src.config.protocol import CanConfig


class CanTab(ttk.Frame):
    """CAN Bus configuration tab"""
    
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def _create_widgets(self):
        """Create CAN tab widgets"""
        # Container - no expand to avoid whitespace
        container = ttk.Frame(self, padding=10)
        container.pack(fill=tk.X, anchor="nw")
        
        # CAN Settings - compact LabelFrame
        settings_section = ttk.LabelFrame(container, text="CAN Settings", padding=8)
        settings_section.pack(fill=tk.X, pady=5)
        
        # Baud Rate
        baud_frame = ttk.Frame(settings_section)
        baud_frame.pack(pady=2, anchor="w")
        
        ttk.Label(baud_frame, text="Baud Rate:", width=12).pack(side=tk.LEFT)
        self.baud_var = tk.StringVar(value="500000")
        baud_combo = ttk.Combobox(baud_frame, textvariable=self.baud_var,
                                  values=["125000", "250000", "500000", "800000", "1000000"],
                                  state="readonly", width=15)
        baud_combo.pack(side=tk.LEFT, padx=5)
        
        # Mode
        mode_frame = ttk.Frame(settings_section)
        mode_frame.pack(pady=2, anchor="w")
        
        ttk.Label(mode_frame, text="Mode:", width=12).pack(side=tk.LEFT)
        self.mode_var = tk.StringVar(value="NORMAL")
        mode_combo = ttk.Combobox(mode_frame, textvariable=self.mode_var,
                                  values=["NORMAL", "LOOPBACK", "SILENT"],
                                  state="readonly", width=15)
        mode_combo.pack(side=tk.LEFT, padx=5)
        
        # Whitelist section - compact
        whitelist_section = ttk.LabelFrame(container, text="CAN ID Whitelist", padding=8)
        whitelist_section.pack(fill=tk.X, pady=5)
        
        # Count
        count_frame = ttk.Frame(whitelist_section)
        count_frame.pack(pady=2, anchor="w")
        
        ttk.Label(count_frame, text="Total IDs:", width=12).pack(side=tk.LEFT)
        self.count_label = ttk.Label(count_frame, text="0", foreground="#757575")
        self.count_label.pack(side=tk.LEFT)
        
        # Whitelist display - smaller
        list_frame = ttk.Frame(whitelist_section)
        list_frame.pack(fill=tk.X, pady=2)
        
        scrollbar = ttk.Scrollbar(list_frame)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.whitelist_text = tk.Text(list_frame, height=4, width=30,
                                       yscrollcommand=scrollbar.set,
                                       state=tk.DISABLED,
                                       font=("Consolas", 9))
        self.whitelist_text.pack(fill=tk.X)
        scrollbar.config(command=self.whitelist_text.yview)
        
        # Set Button
        btn_frame = ttk.Frame(container)
        btn_frame.pack(fill=tk.X, pady=10)
        ttk.Button(btn_frame, text="Set CAN Config", style='Set.TButton',
                  command=self._set_can_config).pack(anchor="e", padx=5)
    
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
    
    def _set_can_config(self):
        """Set CAN configuration"""
        if not self._check_connection():
            return
        
        baud = self.baud_var.get()
        mode = self.mode_var.get()
        
        # CFCB:baudrate - CAN Baud rate
        self._send_command(f"CFCB:{baud}", f"CAN Baud = {baud}")
        
        # CFCM:mode - CAN Mode
        self._send_command(f"CFCM:{mode}", f"CAN Mode = {mode}")
    
    def get_config(self) -> CanConfig:
        """Get CAN config from UI"""
        config = CanConfig()
        try:
            config.baud_rate = int(self.baud_var.get())
        except:
            config.baud_rate = 500000
        config.mode = self.mode_var.get()
        return config
    
    def set_config(self, config: CanConfig):
        """Set CAN config to UI"""
        self.baud_var.set(str(config.baud_rate))
        self.mode_var.set(config.mode)
        self.count_label.config(text=str(config.whitelist_count))
        
        # Update whitelist display
        self.whitelist_text.config(state=tk.NORMAL)
        self.whitelist_text.delete(1.0, tk.END)
        if config.whitelist:
            ids = config.whitelist.split(',')
            for can_id in ids:
                self.whitelist_text.insert(tk.END, f"• {can_id.strip()}\n")
        self.whitelist_text.config(state=tk.DISABLED)
