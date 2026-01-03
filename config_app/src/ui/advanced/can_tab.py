"""
CAN Bus Configuration Tab

Commands are sent with CFML prefix (MCU LAN):
- CFML:CFCB:<baud>      - Set CAN baud rate
- CFML:CFCM:<mode>      - Set CAN mode
- CFML:CFCW:SET:<ids>   - Set CAN whitelist
- CFML:CFCW:CLR         - Clear CAN whitelist
"""

import tkinter as tk
from tkinter import ttk, messagebox
import time
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
        self.whitelist_ids = []  # Store whitelist IDs
        self._create_widgets()
    
    def _create_widgets(self):
        """Create CAN tab widgets - compact layout"""
        # Container - minimal padding
        container = ttk.Frame(self, padding=5)
        container.pack(fill=tk.X, anchor="nw")
        
        # ═══════════════════════════════════════════════════════════════════
        # CAN Settings Section
        # ═══════════════════════════════════════════════════════════════════
        settings_section = ttk.LabelFrame(container, text="CAN Settings", padding=5)
        settings_section.pack(fill=tk.X, pady=3)
        
        # Row 1: Baud Rate and Mode on same row
        row1 = ttk.Frame(settings_section)
        row1.pack(fill=tk.X, pady=2)
        
        ttk.Label(row1, text="Baud Rate:", width=10).pack(side=tk.LEFT)
        self.baud_var = tk.StringVar(value="500000")
        baud_combo = ttk.Combobox(row1, textvariable=self.baud_var,
                                  values=["125000", "250000", "500000", "800000", "1000000"],
                                  state="readonly", width=12)
        baud_combo.pack(side=tk.LEFT, padx=3)
        
        ttk.Label(row1, text="Mode:", width=6).pack(side=tk.LEFT, padx=(15, 0))
        self.mode_var = tk.StringVar(value="NORMAL")
        mode_combo = ttk.Combobox(row1, textvariable=self.mode_var,
                                  values=["NORMAL", "LOOPBACK", "SILENT"],
                                  state="readonly", width=12)
        mode_combo.pack(side=tk.LEFT, padx=3)
        
        # CAN Settings Set Button
        ttk.Button(settings_section, text="✅ Set CAN Settings", style='Set.TButton',
                  command=self._set_can_settings).pack(anchor="e", pady=3)
        
        # ═══════════════════════════════════════════════════════════════════
        # CAN ID Whitelist Section
        # ═══════════════════════════════════════════════════════════════════
        whitelist_section = ttk.LabelFrame(container, text="CAN ID Whitelist", padding=5)
        whitelist_section.pack(fill=tk.X, pady=3)
        
        # Whitelist content - 2 columns
        whitelist_content = ttk.Frame(whitelist_section)
        whitelist_content.pack(fill=tk.X, pady=2)
        
        # Left column - List display
        list_col = ttk.Frame(whitelist_content)
        list_col.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        
        ttk.Label(list_col, text="Allowed CAN IDs:", font=("Segoe UI", 9, "bold")).pack(anchor="w")
        
        list_frame = ttk.Frame(list_col)
        list_frame.pack(fill=tk.X, pady=2)
        
        scrollbar = ttk.Scrollbar(list_frame)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.whitelist_listbox = tk.Listbox(list_frame, height=4, width=12,
                                             yscrollcommand=scrollbar.set,
                                             font=("Consolas", 9),
                                             selectmode=tk.EXTENDED)
        self.whitelist_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.config(command=self.whitelist_listbox.yview)
        
        # Count label
        self.count_label = ttk.Label(list_col, text="Total: 0 IDs", foreground="#757575")
        self.count_label.pack(anchor="w")
        
        # Right column - Controls
        ctrl_col = ttk.Frame(whitelist_content)
        ctrl_col.pack(side=tk.LEFT, padx=(10, 0))
        
        ttk.Label(ctrl_col, text="Add ID (hex):", font=("Segoe UI", 9, "bold")).pack(anchor="w")
        
        # Add ID input
        add_frame = ttk.Frame(ctrl_col)
        add_frame.pack(fill=tk.X, pady=2)
        
        ttk.Label(add_frame, text="0x").pack(side=tk.LEFT)
        self.new_id_var = tk.StringVar()
        self.new_id_entry = ttk.Entry(add_frame, textvariable=self.new_id_var, width=6, font=("Consolas", 9))
        self.new_id_entry.pack(side=tk.LEFT, padx=2)
        ttk.Button(add_frame, text="➕", width=3, command=self._add_id).pack(side=tk.LEFT, padx=2)
        
        # Quick actions
        ttk.Button(ctrl_col, text="🗑️ Remove", width=12,
                  command=self._remove_selected).pack(fill=tk.X, pady=1)
        ttk.Button(ctrl_col, text="🧹 Clear All", width=12,
                  command=self._clear_all).pack(fill=tk.X, pady=1)
        
        # Whitelist Set Button
        ttk.Button(whitelist_section, text="✅ Set CAN Whitelist", style='Set.TButton',
                  command=self._set_can_whitelist).pack(anchor="e", pady=3)
    
    def _add_id(self):
        """Add new CAN ID to whitelist"""
        try:
            id_str = self.new_id_var.get().strip()
            if not id_str:
                return
            
            # Parse hex value
            can_id = int(id_str, 16)
            
            # Validate range (standard CAN ID: 0x000-0x7FF)
            if can_id < 0 or can_id > 0x7FF:
                messagebox.showwarning("Warning", "CAN ID must be between 0x000 and 0x7FF")
                return
            
            # Check duplicate
            id_hex = f"0x{can_id:03X}"
            if id_hex in self.whitelist_ids:
                messagebox.showinfo("Info", f"{id_hex} already in whitelist")
                return
            
            # Add to list
            self.whitelist_ids.append(id_hex)
            self.whitelist_listbox.insert(tk.END, id_hex)
            self._update_count()
            
            # Clear input
            self.new_id_var.set("")
            
        except ValueError:
            messagebox.showerror("Error", "Invalid hex value")
    
    def _remove_selected(self):
        """Remove selected IDs from whitelist"""
        selected = self.whitelist_listbox.curselection()
        if not selected:
            return
        
        # Remove in reverse order to maintain indices
        for idx in reversed(selected):
            del self.whitelist_ids[idx]
            self.whitelist_listbox.delete(idx)
        
        self._update_count()
    
    def _clear_all(self):
        """Clear all IDs from whitelist"""
        if not self.whitelist_ids:
            return
        
        if messagebox.askyesno("Confirm", "Clear all CAN IDs from whitelist?"):
            self.whitelist_ids.clear()
            self.whitelist_listbox.delete(0, tk.END)
            self._update_count()
    
    def _update_count(self):
        """Update count label"""
        self.count_label.config(text=f"Total: {len(self.whitelist_ids)} IDs")
    
    def _check_connection(self) -> bool:
        """Check if serial is connected"""
        if not self.serial_manager or not self.serial_manager.is_connected():
            messagebox.showwarning("Warning", "Not connected to gateway")
            return False
        return True
    
    def _send_command(self, cmd: str, description: str):
        """Send command with CFML prefix"""
        self.log(f"Sending: {description}", "INFO")
        
        # Add CFML prefix for LAN commands
        full_cmd = f"CFML:{cmd}"
        self.log(f"→ {full_cmd}", "DEBUG")
        
        if self.serial_manager.send(full_cmd):
            self.log(f"✓ {description} - Sent", "SUCCESS")
        else:
            self.log(f"✗ {description} - Send failed", "ERROR")
    
    def _set_can_settings(self):
        """Set CAN Settings only (Baud + Mode)"""
        if not self._check_connection():
            return
        
        baud = self.baud_var.get()
        mode = self.mode_var.get()
        
        # CFML:CFCB:baudrate - CAN Baud rate
        self._send_command(f"CFCB:{baud}", f"CAN Baud = {baud}")
        
        # 1s delay between commands
        time.sleep(1.0)
        
        # CFML:CFCM:mode - CAN Mode
        self._send_command(f"CFCM:{mode}", f"CAN Mode = {mode}")
    
    def _set_can_whitelist(self):
        """Set CAN Whitelist only"""
        if not self._check_connection():
            return
        
        if not self.whitelist_ids:
            # Clear whitelist
            self._send_command("CFCW:CLR", "CAN Whitelist Clear")
        else:
            # Set whitelist with comma-separated IDs
            ids_str = ",".join(self.whitelist_ids)
            self._send_command(f"CFCW:SET:{ids_str}", f"CAN Whitelist ({len(self.whitelist_ids)} IDs)")
    
    def get_config(self) -> CanConfig:
        """Get CAN config from UI"""
        config = CanConfig()
        try:
            config.baud_rate = int(self.baud_var.get())
        except:
            config.baud_rate = 500000
        config.mode = self.mode_var.get()
        config.whitelist_count = len(self.whitelist_ids)
        config.whitelist = ",".join(self.whitelist_ids)
        return config
    
    def set_config(self, config: CanConfig):
        """Set CAN config to UI"""
        self.baud_var.set(str(config.baud_rate))
        self.mode_var.set(config.mode)
        
        # Update whitelist
        self.whitelist_ids.clear()
        self.whitelist_listbox.delete(0, tk.END)
        
        if config.whitelist:
            ids = [id.strip() for id in config.whitelist.split(',') if id.strip()]
            for can_id in ids:
                self.whitelist_ids.append(can_id)
                self.whitelist_listbox.insert(tk.END, can_id)
        
        self._update_count()
