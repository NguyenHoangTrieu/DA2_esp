"""
LTE Configuration Tab (Advanced)
APN Settings and Connection Parameters
"""

import tkinter as tk
from tkinter import ttk, messagebox
import threading


class LTETab(ttk.Frame):
    """Advanced LTE configuration tab"""
    
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def _create_widgets(self):
        """Create LTE tab widgets"""
        # Container with padding
        container = ttk.Frame(self, padding=10)
        container.pack(fill=tk.X, anchor="nw")
        
        # ═══════════════════════════════════════════════════════════════════
        # APN Settings Section
        # ═══════════════════════════════════════════════════════════════════
        apn_frame = ttk.LabelFrame(container, text="APN Settings", padding=8)
        apn_frame.pack(fill=tk.X, pady=5)
        
        # APN
        row1 = ttk.Frame(apn_frame)
        row1.pack(fill=tk.X, pady=2)
        ttk.Label(row1, text="APN:", width=15).pack(side=tk.LEFT)
        self.apn_var = tk.StringVar()
        ttk.Entry(row1, textvariable=self.apn_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Username
        row2 = ttk.Frame(apn_frame)
        row2.pack(fill=tk.X, pady=2)
        ttk.Label(row2, text="Username:", width=15).pack(side=tk.LEFT)
        self.username_var = tk.StringVar()
        ttk.Entry(row2, textvariable=self.username_var).pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Password
        row3 = ttk.Frame(apn_frame)
        row3.pack(fill=tk.X, pady=2)
        ttk.Label(row3, text="Password:", width=15).pack(side=tk.LEFT)
        
        # Pack Checkbutton first (right side)
        self.show_pwd = tk.BooleanVar()
        ttk.Checkbutton(row3, text="Show", variable=self.show_pwd,
                       command=lambda: self.password_entry.config(show="" if self.show_pwd.get() else "*")
                       ).pack(side=tk.RIGHT)
        
        # Then Entry fills remaining space
        self.password_var = tk.StringVar()
        self.password_entry = ttk.Entry(row3, textvariable=self.password_var, show="*")
        self.password_entry.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # ═══════════════════════════════════════════════════════════════════
        # Connection Settings Section
        # ═══════════════════════════════════════════════════════════════════
        conn_frame = ttk.LabelFrame(container, text="Connection Settings", padding=8)
        conn_frame.pack(fill=tk.X, pady=5)
        
        # Comm Type
        row4 = ttk.Frame(conn_frame)
        row4.pack(fill=tk.X, pady=2)
        ttk.Label(row4, text="Comm Type:", width=15).pack(side=tk.LEFT)
        self.comm_var = tk.StringVar(value="UART")
        self.comm_combo = ttk.Combobox(row4, textvariable=self.comm_var, state="readonly",
                                       values=["UART", "USB"])
        self.comm_combo.pack(side=tk.LEFT, padx=5, fill=tk.X, expand=True)
        
        # Auto Reconnect
        row5 = ttk.Frame(conn_frame)
        row5.pack(fill=tk.X, pady=2)
        ttk.Label(row5, text="Auto Reconnect:", width=15).pack(side=tk.LEFT)
        self.reconnect_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(row5, variable=self.reconnect_var, text="Enable").pack(side=tk.LEFT, padx=5)
        
        # Timeout
        row6 = ttk.Frame(conn_frame)
        row6.pack(fill=tk.X, pady=2)
        ttk.Label(row6, text="Timeout (s):", width=15).pack(side=tk.LEFT)
        self.timeout_var = tk.StringVar(value="30")
        timeout_spin = ttk.Spinbox(row6, textvariable=self.timeout_var, from_=5, to=300, width=10)
        timeout_spin.pack(side=tk.LEFT, padx=5)
        ttk.Label(row6, text="(5-300 seconds)", foreground="#757575").pack(side=tk.LEFT)
        
        # Max Retry
        row7 = ttk.Frame(conn_frame)
        row7.pack(fill=tk.X, pady=2)
        ttk.Label(row7, text="Max Retry:", width=15).pack(side=tk.LEFT)
        self.retry_var = tk.StringVar(value="3")
        retry_spin = ttk.Spinbox(row7, textvariable=self.retry_var, from_=0, to=10, width=10)
        retry_spin.pack(side=tk.LEFT, padx=5)
        ttk.Label(row7, text="(0=unlimited)", foreground="#757575").pack(side=tk.LEFT)
        
        # ═══════════════════════════════════════════════════════════════════
        # Set Button
        # ═══════════════════════════════════════════════════════════════════
        btn_frame = ttk.Frame(container)
        btn_frame.pack(fill=tk.X, pady=15, anchor="nw")
        ttk.Button(btn_frame, text="✅ Set LTE Config", style='Set.TButton',
                  command=self._set_lte_config).pack(anchor="e", padx=5)
        
        # ═══════════════════════════════════════════════════════════════════
        # Command Info
        # ═══════════════════════════════════════════════════════════════════
        info_frame = ttk.Frame(container)
        info_frame.pack(fill=tk.X, anchor="nw")
        ttk.Separator(info_frame, orient='horizontal').pack(fill=tk.X, pady=5)
        ttk.Label(info_frame, text="Commands: CFLT:APN:USER:PASS:COMM_TYPE:AUTO_RECONNECT:TIMEOUT:MAX_RETRY",
                  foreground="#757575", font=('Consolas', 9)).pack(anchor="w")
        ttk.Label(info_frame, text="Then CFIN:LTE (after 500ms delay) | COMM_TYPE: UART/USB | AUTO_RECONNECT: true/false",
                  foreground="#757575", font=('Consolas', 9)).pack(anchor="w")
    
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
    
    def _set_lte_config(self):
        """Set LTE configuration"""
        if not self._check_connection():
            return
        
        # Build CFLT command first
        # Format: CFLT:APN:USERNAME:PASSWORD:COMM_TYPE:AUTO_RECONNECT:TIMEOUT:MAX_RETRY
        apn = self.apn_var.get().strip()
        username = self.username_var.get().strip()
        password = self.password_var.get()
        comm_type = self.comm_var.get()  # UART or USB
        reconnect = "true" if self.reconnect_var.get() else "false"
        timeout = self.timeout_var.get()
        retry = self.retry_var.get()
        
        cmd = f"CFLT:{apn}:{username}:{password}:{comm_type}:{reconnect}:{timeout}:{retry}"
        
        # Send CFLT command first, then CFIN:LTE after 500ms delay (no response waiting)
        def send_lte_sequence():
            self.log(f"→ {cmd}", "DEBUG")
            self.serial_manager.send(cmd)
            self.log(f"✓ LTE Config - Sent", "SUCCESS")
            
            # Wait 500ms before sending CFIN:LTE
            import time
            time.sleep(0.5)
            
            self.log("→ CFIN:LTE", "DEBUG")
            self.serial_manager.send("CFIN:LTE")
            self.log(f"✓ Set Internet Type = LTE - Sent", "SUCCESS")
        
        thread = threading.Thread(target=send_lte_sequence)
        thread.daemon = True
        thread.start()
    
    def set_config(self, config):
        """Set config from data"""
        if hasattr(config, 'lte_apn'):
            self.apn_var.set(config.lte_apn or "")
        if hasattr(config, 'lte_username'):
            self.username_var.set(config.lte_username or "")
        if hasattr(config, 'lte_password'):
            if config.lte_password and config.lte_password != "***HIDDEN***":
                self.password_var.set(config.lte_password)
        if hasattr(config, 'lte_comm_type'):
            self.comm_var.set("USB" if config.lte_comm_type == "USB" else "UART")
        if hasattr(config, 'lte_reconnect'):
            self.reconnect_var.set(config.lte_reconnect)
        if hasattr(config, 'lte_timeout'):
            self.timeout_var.set(str(config.lte_timeout))
        if hasattr(config, 'lte_retry'):
            self.retry_var.set(str(config.lte_retry))
    
    def get_config(self) -> dict:
        """Get config as dict"""
        return {
            'lte_apn': self.apn_var.get(),
            'lte_username': self.username_var.get(),
            'lte_password': self.password_var.get(),
            'lte_comm_type': self.comm_var.get(),  # UART or USB
            'lte_reconnect': self.reconnect_var.get(),
            'lte_timeout': int(self.timeout_var.get()),
            'lte_retry': int(self.retry_var.get()),
        }
