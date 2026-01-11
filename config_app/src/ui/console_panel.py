"""
Debug Log Panel Widget for ESP32 Gateway Configuration Tool
Displays application events with timestamps and icons
"""

import tkinter as tk
from tkinter import ttk
from datetime import datetime
from typing import Optional


class ConsolePanel(ttk.Frame):
    """Debug Log panel widget - displays app events"""
    
    # Log level colors
    COLORS = {
        'INFO': '#2196F3',      # Blue
        'SUCCESS': '#4CAF50',   # Green
        'WARNING': '#FF9800',   # Orange
        'ERROR': '#F44336',     # Red
        'DEBUG': '#757575',     # Gray
    }
    
    def __init__(self, parent, height=120, **kwargs):
        super().__init__(parent, **kwargs)
        
        self.fixed_height = height
        self._create_widgets()
    
    def _create_widgets(self):
        """Create debug log widgets"""
        # Header with title and toolbar - compact single row
        header = ttk.Frame(self)
        header.pack(fill=tk.X)
        
        ttk.Label(header, text="📟 Debug Log", font=('Segoe UI', 10, 'bold')).pack(side=tk.LEFT, padx=5)
        
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
                           bg="#FFFFFF", fg="#333333",  # White background
                           insertbackground="#000000",
                           height=8)  # Fixed height in lines
        self.text.pack(fill=tk.BOTH, expand=True)
        
        self.scrollbar.config(command=self.text.yview)
        
        # Configure tags for colors
        for level, color in self.COLORS.items():
            self.text.tag_configure(level, foreground=color)
        
        self.text.tag_configure('TIMESTAMP', foreground='#888888')
    
    def log(self, message: str, level: str = "INFO"):
        """Add log message"""
        timestamp = datetime.now().strftime("[%H:%M:%S]")
        
        self.text.config(state=tk.NORMAL)
        
        # Insert timestamp
        self.text.insert(tk.END, timestamp + " ", 'TIMESTAMP')
        
        # Insert level indicator
        level_indicators = {
            'INFO': 'ℹ️',
            'SUCCESS': '✅',
            'WARNING': '⚠️',
            'ERROR': '❌',
            'DEBUG': '🔧',
        }
        indicator = level_indicators.get(level, 'ℹ️')
        
        # Insert message with color
        self.text.insert(tk.END, f"{indicator} {message}\n", level)
        
        self.text.config(state=tk.DISABLED)
        
        # Auto-scroll
        if self.autoscroll_var.get():
            self.text.see(tk.END)
    
    def clear(self):
        """Clear console"""
        self.text.config(state=tk.NORMAL)
        self.text.delete(1.0, tk.END)
        self.text.config(state=tk.DISABLED)
    
    def get_text(self) -> str:
        """Get all console text"""
        return self.text.get(1.0, tk.END)
