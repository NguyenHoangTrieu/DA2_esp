"""
UART Log Panel Widget for ESP32 Gateway Configuration Tool
Displays raw serial data from the connected COM port
"""

import tkinter as tk
from tkinter import ttk
from typing import Optional


class UartLogPanel(ttk.Frame):
    """UART Log panel widget - displays raw serial data"""
    
    def __init__(self, parent, **kwargs):
        super().__init__(parent, **kwargs)
        
        self.port_name = "Not Connected"
        self._create_widgets()
    
    def _create_widgets(self):
        """Create UART log widgets"""
        # Header with title and toolbar
        header = ttk.Frame(self)
        header.pack(fill=tk.X)
        
        self.title_label = ttk.Label(header, text="📡 UART Log (Not Connected)", 
                                      font=('Segoe UI', 10, 'bold'))
        self.title_label.pack(side=tk.LEFT, padx=5)
        
        self.clear_btn = ttk.Button(header, text="🧹 Clear", width=8,
                                     command=self.clear)
        self.clear_btn.pack(side=tk.RIGHT, padx=2)
        
        self.autoscroll_var = tk.BooleanVar(value=True)
        self.autoscroll_cb = ttk.Checkbutton(header, text="Auto-scroll",
                                              variable=self.autoscroll_var)
        self.autoscroll_cb.pack(side=tk.RIGHT, padx=5)
        
        # Text area with scrollbar
        text_frame = ttk.Frame(self)
        text_frame.pack(fill=tk.BOTH, expand=True, pady=(2, 0))
        
        self.scrollbar = ttk.Scrollbar(text_frame)
        self.scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.text = tk.Text(text_frame, wrap=tk.WORD,
                           font=("Consolas", 9),
                           yscrollcommand=self.scrollbar.set,
                           state=tk.DISABLED,
                           bg="#FFF5F5", fg="#333333",  # Light pink background
                           insertbackground="#000000")
        self.text.pack(fill=tk.BOTH, expand=True)
        
        self.scrollbar.config(command=self.text.yview)
        
        # Configure tags for TX/RX
        self.text.tag_configure('TX', foreground='#0066CC')  # Blue for sent
        self.text.tag_configure('RX', foreground='#006600')  # Green for received
        self.text.tag_configure('MARKER', foreground='#888888')  # Gray for markers
    
    def set_port(self, port_name: str):
        """Update the port name in header"""
        self.port_name = port_name
        self.title_label.config(text=f"📡 UART Log ({port_name})")
    
    def set_disconnected(self):
        """Set to disconnected state"""
        self.port_name = "Not Connected"
        self.title_label.config(text="📡 UART Log (Not Connected)")
    
    def log_tx(self, data: str):
        """Log transmitted data"""
        self.text.config(state=tk.NORMAL)
        self.text.insert(tk.END, "→ ", 'MARKER')
        self.text.insert(tk.END, f"{data}\n", 'TX')
        self.text.config(state=tk.DISABLED)
        
        if self.autoscroll_var.get():
            self.text.see(tk.END)
    
    def log_rx(self, data: str):
        """Log received data"""
        self.text.config(state=tk.NORMAL)
        # Don't add marker for multi-line responses, just show data
        lines = data.strip().split('\n')
        for line in lines:
            if line.strip():
                self.text.insert(tk.END, f"{line}\n", 'RX')
        self.text.config(state=tk.DISABLED)
        
        if self.autoscroll_var.get():
            self.text.see(tk.END)
    
    def log_raw(self, data: str, direction: str = "RX"):
        """Log raw data with direction indicator"""
        self.text.config(state=tk.NORMAL)
        tag = 'TX' if direction == "TX" else 'RX'
        marker = "→ " if direction == "TX" else "← "
        self.text.insert(tk.END, marker, 'MARKER')
        self.text.insert(tk.END, f"{data}\n", tag)
        self.text.config(state=tk.DISABLED)
        
        if self.autoscroll_var.get():
            self.text.see(tk.END)
    
    def clear(self):
        """Clear UART log"""
        self.text.config(state=tk.NORMAL)
        self.text.delete(1.0, tk.END)
        self.text.config(state=tk.DISABLED)
