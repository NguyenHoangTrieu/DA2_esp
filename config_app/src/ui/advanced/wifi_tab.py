"""
WiFi Configuration Tab (Advanced)
Supports PERSONAL and ENTERPRISE authentication modes
"""

import tkinter as tk
from tkinter import ttk, messagebox
import threading


class WiFiTab(ttk.Frame):
    """Advanced WiFi configuration tab"""
    
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def _create_widgets(self):
        """Create WiFi tab widgets"""
        # Container with padding
        container = ttk.Frame(self, padding=10)
        container.pack(fill=tk.X, anchor="nw")
        
        # ═══════════════════════════════════════════════════════════════════
        # WiFi Network Settings Section
        # ═══════════════════════════════════════════════════════════════════
        network_frame = ttk.LabelFrame(container, text="WiFi Network Settings", padding=8)
        network_frame.pack(fill=tk.X, pady=5)
        
        # SSID
        row1 = ttk.Frame(network_frame)
        row1.pack(fill=tk.X, pady=2)
        ttk.Label(row1, text="SSID:", width=15).pack(side=tk.LEFT)
        self.ssid_var = tk.StringVar()
        ttk.Entry(row1, textvariable=self.ssid_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Password
        row2 = ttk.Frame(network_frame)
        row2.pack(fill=tk.X, pady=2)
        ttk.Label(row2, text="Password:", width=15).pack(side=tk.LEFT)
        
        # Pack Checkbutton first (right side)
        self.show_pwd = tk.BooleanVar()
        ttk.Checkbutton(row2, text="Show", variable=self.show_pwd,
                       command=lambda: self.password_entry.config(show="" if self.show_pwd.get() else "*")
                       ).pack(side=tk.RIGHT)
        
        # Then Entry fills remaining space
        self.password_var = tk.StringVar()
        self.password_entry = ttk.Entry(row2, textvariable=self.password_var, show="*")
        self.password_entry.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # ═══════════════════════════════════════════════════════════════════
        # Authentication Section
        # ═══════════════════════════════════════════════════════════════════
        auth_frame = ttk.LabelFrame(container, text="Authentication", padding=8)
        auth_frame.pack(fill=tk.X, pady=5)
        
        # Auth Mode
        row3 = ttk.Frame(auth_frame)
        row3.pack(fill=tk.X, pady=2)
        ttk.Label(row3, text="Auth Mode:", width=15).pack(side=tk.LEFT)
        self.auth_var = tk.StringVar(value="PERSONAL")
        self.auth_combo = ttk.Combobox(row3, textvariable=self.auth_var, state="readonly",
                                       values=["PERSONAL", "ENTERPRISE"])
        self.auth_combo.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        self.auth_combo.bind("<<ComboboxSelected>>", self._on_auth_change)
        
        # Username (Enterprise only) - Initially hidden
        self.username_frame = ttk.Frame(auth_frame)
        # Don't pack initially - hidden when PERSONAL
        ttk.Label(self.username_frame, text="Username:", width=15).pack(side=tk.LEFT)
        self.username_var = tk.StringVar()
        self.username_entry = ttk.Entry(self.username_frame, textvariable=self.username_var)
        self.username_entry.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # ═══════════════════════════════════════════════════════════════════
        # Set Button
        # ═══════════════════════════════════════════════════════════════════
        btn_frame = ttk.Frame(container)
        btn_frame.pack(fill=tk.X, pady=15, anchor="nw")
        ttk.Button(btn_frame, text="✅ Set WiFi Config", style='Set.TButton',
                  command=self._set_wifi_config).pack(anchor="e", padx=5)
        
        # ═══════════════════════════════════════════════════════════════════
        # Command Info
        # ═══════════════════════════════════════════════════════════════════
        info_frame = ttk.Frame(container)
        info_frame.pack(fill=tk.X, anchor="nw")
        ttk.Separator(info_frame, orient='horizontal').pack(fill=tk.X, pady=5)
        ttk.Label(info_frame, text="Commands: CFWF:SSID:PASSWORD:AUTH_MODE or CFWF:SSID:PASSWORD:USERNAME:AUTH_MODE",
                  foreground="#757575", font=('Consolas', 9)).pack(anchor="w")
        ttk.Label(info_frame, text="Then CFIN:WIFI (after 500ms delay) | AUTH_MODE: PERSONAL or ENTERPRISE",
                  foreground="#757575", font=('Consolas', 9)).pack(anchor="w")
    
    def _on_auth_change(self, event=None):
        """Handle auth mode change - show/hide username field"""
        if self.auth_var.get() == "ENTERPRISE":
            self.username_frame.pack(fill=tk.X, pady=2)
        else:
            self.username_frame.pack_forget()
            self.username_var.set("")
    
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
    
    def _set_wifi_config(self):
        """Set WiFi configuration"""
        if not self._check_connection():
            return
        
        ssid = self.ssid_var.get().strip()
        if not ssid:
            messagebox.showwarning("Warning", "Please enter SSID")
            return
        
        password = self.password_var.get()
        auth_mode = self.auth_var.get()
        username = self.username_var.get().strip()
        
        # Validate username for Enterprise mode
        if auth_mode == "ENTERPRISE" and not username:
            messagebox.showwarning("Warning", "Please enter Username for Enterprise mode")
            return
        
        # Build WiFi command: CFWF:SSID:PASSWORD:AUTH_MODE or CFWF:SSID:PASSWORD:USERNAME:AUTH_MODE
        if auth_mode == "ENTERPRISE":
            cmd = f"CFWF:{ssid}:{password}:{username}:ENTERPRISE"
        else:
            cmd = f"CFWF:{ssid}:{password}:PERSONAL"
        
        # Send CFWF first, then CFIN:WIFI after 500ms delay (no response waiting)
        def send_wifi_sequence():
            self.log(f"→ {cmd}", "DEBUG")
            self.serial_manager.send(cmd)
            self.log(f"✓ WiFi Config - Sent", "SUCCESS")
            
            # Wait 500ms before sending CFIN:WIFI
            import time
            time.sleep(0.5)
            
            self.log("→ CFIN:WIFI", "DEBUG")
            self.serial_manager.send("CFIN:WIFI")
            self.log(f"✓ Set Internet Type = WIFI - Sent", "SUCCESS")
        
        thread = threading.Thread(target=send_wifi_sequence)
        thread.daemon = True
        thread.start()
    
    def set_config(self, config):
        """Set config from data"""
        if hasattr(config, 'wifi_ssid'):
            self.ssid_var.set(config.wifi_ssid or "")
        if hasattr(config, 'wifi_password'):
            if config.wifi_password and config.wifi_password != "***HIDDEN***":
                self.password_var.set(config.wifi_password)
        if hasattr(config, 'wifi_auth_mode'):
            self.auth_var.set("ENTERPRISE" if config.wifi_auth_mode == 1 else "PERSONAL")
            self._on_auth_change()
        if hasattr(config, 'wifi_username'):
            self.username_var.set(config.wifi_username or "")
    
    def get_config(self) -> dict:
        """Get config as dict"""
        return {
            'wifi_ssid': self.ssid_var.get(),
            'wifi_password': self.password_var.get(),
            'wifi_auth_mode': 1 if self.auth_var.get() == "ENTERPRISE" else 0,
            'wifi_username': self.username_var.get(),
        }
