"""
Connection Bar Widget for ESP32 Gateway Configuration Tool
"""

import tkinter as tk
from tkinter import ttk
from typing import Callable, Optional


class ConnectionBar(ttk.Frame):
    """Connection bar widget for serial port selection and connection"""
    
    def __init__(self, parent, on_connect: Callable, on_disconnect: Callable,
                 on_refresh: Callable, **kwargs):
        super().__init__(parent, **kwargs)
        
        self.on_connect = on_connect
        self.on_disconnect = on_disconnect
        self.on_refresh = on_refresh
        self.is_connected = False
        
        self._create_widgets()
    
    def _create_widgets(self):
        """Create connection bar widgets"""
        # Port selection
        ttk.Label(self, text="Port:").pack(side=tk.LEFT, padx=(10, 5))
        
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(self, textvariable=self.port_var, 
                                        width=15, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=5)
        
        # Refresh button - text "[Refresh]"
        self.refresh_btn = ttk.Button(self, text="Refresh", width=7, 
                                       command=self._on_refresh)
        self.refresh_btn.pack(side=tk.LEFT, padx=2)
        
        # Baud rate selection
        ttk.Label(self, text="Baud:").pack(side=tk.LEFT, padx=(20, 5))
        
        self.baud_var = tk.StringVar(value="115200")
        self.baud_combo = ttk.Combobox(self, textvariable=self.baud_var,
                                        width=10, state="readonly",
                                        values=["9600", "19200", "38400", 
                                               "57600", "115200", "230400"])
        self.baud_combo.pack(side=tk.LEFT, padx=5)
        
        # Connect button
        self.connect_btn = ttk.Button(self, text="🔌 Connect", width=12,
                                       command=self._on_connect_click)
        self.connect_btn.pack(side=tk.LEFT, padx=(20, 10))
        
        # Status indicator
        self.status_frame = ttk.Frame(self)
        self.status_frame.pack(side=tk.RIGHT, padx=10)
        
        self.status_indicator = tk.Canvas(self.status_frame, width=12, height=12,
                                          highlightthickness=0)
        self.status_indicator.pack(side=tk.LEFT, padx=(0, 5))
        self._draw_status_indicator(False)
        
        self.status_label = ttk.Label(self.status_frame, text="Disconnected")
        self.status_label.pack(side=tk.LEFT)
    
    def _draw_status_indicator(self, connected: bool):
        """Draw status indicator circle"""
        self.status_indicator.delete("all")
        color = "#4CAF50" if connected else "#F44336"
        self.status_indicator.create_oval(2, 2, 10, 10, fill=color, outline=color)
    
    def _on_refresh(self):
        """Handle refresh button click"""
        ports = self.on_refresh()
        # Show only COM name without description
        self.port_combo['values'] = [p[0] for p in ports]
        if ports:
            self.port_combo.current(0)
    
    def _on_connect_click(self):
        """Handle connect/disconnect button click"""
        if self.is_connected:
            self.on_disconnect()
        else:
            port = self.port_var.get()
            if port:
                baudrate = int(self.baud_var.get())
                self.on_connect(port, baudrate)
    
    def set_connected(self, connected: bool, port: str = ""):
        """Update connection status"""
        self.is_connected = connected
        self._draw_status_indicator(connected)
        
        if connected:
            self.connect_btn.config(text="⏏️ Disconnect")
            self.status_label.config(text=f"Connected to {port}")
            self.port_combo.config(state="disabled")
            self.baud_combo.config(state="disabled")
        else:
            self.connect_btn.config(text="🔌 Connect")
            self.status_label.config(text="Disconnected")
            self.port_combo.config(state="readonly")
            self.baud_combo.config(state="readonly")
    
    def get_selected_port(self) -> Optional[str]:
        """Get selected port name"""
        return self.port_var.get() or None
