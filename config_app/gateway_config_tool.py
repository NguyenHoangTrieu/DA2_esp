#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32 IoT Gateway Configuration Tool v2.2
- Fixed serial communication (added CRLF, flush, proper config)
- Incremental config updates (only changed fields)
- Firmware update with real-time logging
- Config comparison and editing
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox, filedialog
import serial
import serial.tools.list_ports
import threading
import json
import subprocess
import sys
import os
from pathlib import Path
from typing import Dict, Any
from datetime import datetime
from copy import deepcopy
import traceback

# ===== CONFIG COMMAND MAPPINGS =====
WAN_CONFIG_MAP = {
    'internet_type': 'CFIN',
    'wifi_ssid': 'CFWS',
    'wifi_password': 'CFWP',
    'wifi_username': 'CFWU',
    'wifi_auth_mode': 'CFWA',
    'lte_apn': 'CFLA',
    'lte_username': 'CFLU',
    'lte_password': 'CFLP',
    'lte_comm_type': 'CFLC',
    'lte_auto_reconnect': 'CFLR',
    'mqtt_broker': 'CFMB',
    'mqtt_pub_topic': 'CFMP',
    'mqtt_sub_topic': 'CFMS',
    'mqtt_device_token': 'CFMT',
    'mqtt_attribute_topic': 'CFMA',
    'server_type': 'CFST',
}

LAN_CONFIG_MAP = {
    'can_baud_rate': 'CFCB',
    'can_mode': 'CFCM',
}

def get_application_path():
    """Get the path where the application/script is located"""
    if getattr(sys, 'frozen', False):
        # Running as compiled executable
        return Path(sys.executable).parent
    else:
        # Running as script
        return Path(__file__).parent

class GatewayConfigTool:
    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 Gateway Config Tool v2.2")
        self.root.geometry("1100x750")
        
        self.serial_port = None
        self.config_data = {}
        self.original_config = {}
        self.reading_config = False
        self.config_modified = False
        
        # Get application path (works for both .py and .exe)
        self.app_path = get_application_path()
        
        self.setup_ui()
        self.refresh_ports()
        
    def setup_ui(self):
        # Main container
        main_frame = ttk.Frame(self.root, padding="10")
        main_frame.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)
        main_frame.columnconfigure(1, weight=1)
        main_frame.rowconfigure(2, weight=1)
        
        # ===== Serial Connection Panel =====
        conn_frame = ttk.LabelFrame(main_frame, text="Serial Connection", padding="5")
        conn_frame.grid(row=0, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)
        
        ttk.Label(conn_frame, text="Port:").grid(row=0, column=0, padx=5)
        self.port_combo = ttk.Combobox(conn_frame, width=15, state='readonly')
        self.port_combo.grid(row=0, column=1, padx=5)
        
        ttk.Label(conn_frame, text="Baud:").grid(row=0, column=2, padx=5)
        self.baud_combo = ttk.Combobox(conn_frame, width=10, state='readonly',
                                       values=['9600', '115200', '230400', '460800', '921600'])
        self.baud_combo.set('115200')
        self.baud_combo.grid(row=0, column=3, padx=5)
        
        self.refresh_btn = ttk.Button(conn_frame, text="Refresh", command=self.refresh_ports)
        self.refresh_btn.grid(row=0, column=4, padx=5)
        
        self.connect_btn = ttk.Button(conn_frame, text="Connect", command=self.toggle_connection)
        self.connect_btn.grid(row=0, column=5, padx=5)
        
        self.test_btn = ttk.Button(conn_frame, text="Test", command=self.test_connection, state='disabled')
        self.test_btn.grid(row=0, column=6, padx=5)
        
        self.status_label = ttk.Label(conn_frame, text="Disconnected", foreground="red")
        self.status_label.grid(row=0, column=7, padx=10)
        
        # ===== Actions Panel =====
        actions_frame = ttk.LabelFrame(main_frame, text="Actions", padding="5")
        actions_frame.grid(row=1, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)
        
        self.scan_btn = ttk.Button(actions_frame, text="Read Config (CFSC)", 
                                   command=self.read_config, state='disabled', width=20)
        self.scan_btn.grid(row=0, column=0, padx=5)
        
        self.write_btn = ttk.Button(actions_frame, text="Write Changes Only", 
                                    command=self.write_incremental_config, state='disabled', width=20)
        self.write_btn.grid(row=0, column=1, padx=5)
        
        ttk.Separator(actions_frame, orient='vertical').grid(row=0, column=2, sticky='ns', padx=10)
        
        self.save_btn = ttk.Button(actions_frame, text="Save to File", 
                                   command=self.save_to_file, state='disabled', width=15)
        self.save_btn.grid(row=0, column=3, padx=5)
        
        self.load_btn = ttk.Button(actions_frame, text="Load from File", 
                                   command=self.load_from_file, width=15)
        self.load_btn.grid(row=0, column=4, padx=5)
        
        ttk.Separator(actions_frame, orient='vertical').grid(row=0, column=5, sticky='ns', padx=10)
        
        self.firmware_btn = ttk.Button(actions_frame, text="Update Firmware", 
                                       command=self.update_firmware, width=15)
        self.firmware_btn.grid(row=0, column=6, padx=5)
        
        self.clear_btn = ttk.Button(actions_frame, text="Clear", 
                                    command=self.clear_config, width=10)
        self.clear_btn.grid(row=0, column=7, padx=5)
        
        # ===== Config Display Panel =====
        config_frame = ttk.LabelFrame(main_frame, text="Configuration Data", padding="5")
        config_frame.grid(row=2, column=0, columnspan=2, sticky=(tk.W, tk.E, tk.N, tk.S), pady=5)
        config_frame.columnconfigure(0, weight=1)
        config_frame.rowconfigure(0, weight=1)
        
        # Create notebook for tabs
        self.notebook = ttk.Notebook(config_frame)
        self.notebook.grid(row=0, column=0, sticky=(tk.W, tk.E, tk.N, tk.S))
        
        # Raw view tab
        raw_frame = ttk.Frame(self.notebook)
        self.notebook.add(raw_frame, text="Raw View")
        
        self.raw_text = scrolledtext.ScrolledText(raw_frame, wrap=tk.WORD, 
                                                  font=('Courier New', 10))
        self.raw_text.pack(fill=tk.BOTH, expand=True)
        
        # Editable view tab
        edit_frame = ttk.Frame(self.notebook)
        self.notebook.add(edit_frame, text="Edit Config")
        
        # Create tree with edit capability
        tree_frame = ttk.Frame(edit_frame)
        tree_frame.pack(fill=tk.BOTH, expand=True)
        
        self.tree = ttk.Treeview(tree_frame, columns=('value', 'changed'), show='tree headings')
        self.tree.heading('#0', text='Parameter')
        self.tree.heading('value', text='Value')
        self.tree.heading('changed', text='Status')
        self.tree.column('#0', width=250)
        self.tree.column('value', width=350)
        self.tree.column('changed', width=80)
        
        tree_scroll = ttk.Scrollbar(tree_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=tree_scroll.set)
        
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        tree_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        # Bind double-click to edit
        self.tree.bind('<Double-1>', self.edit_tree_item)
        
        # Edit controls
        edit_controls = ttk.Frame(edit_frame)
        edit_controls.pack(fill=tk.X, padx=5, pady=5)
        
        ttk.Button(edit_controls, text="Edit Selected", 
                  command=self.edit_selected_item).pack(side=tk.LEFT, padx=5)
        ttk.Button(edit_controls, text="Revert Changes", 
                  command=self.revert_changes).pack(side=tk.LEFT, padx=5)
        
        self.changes_label = ttk.Label(edit_controls, text="No changes", foreground="green")
        self.changes_label.pack(side=tk.LEFT, padx=20)
        
        # ===== Console Panel =====
        console_frame = ttk.LabelFrame(main_frame, text="Console Log", padding="5")
        console_frame.grid(row=3, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)
        
        self.console = scrolledtext.ScrolledText(console_frame, height=8, 
                                                 font=('Courier New', 9), 
                                                 bg='#1e1e1e', fg='#00ff00')
        self.console.pack(fill=tk.BOTH, expand=True)
        console_frame.columnconfigure(0, weight=1)
        
    def log(self, message, level='INFO'):
        """Thread-safe logging"""
        def _log():
            timestamp = datetime.now().strftime('%H:%M:%S')
            color = {'INFO': '#00ff00', 'WARN': '#ffaa00', 'ERROR': '#ff0000', 
                    'SUCCESS': '#00ffff', 'DEBUG': '#888888'}.get(level, '#ffffff')
            
            self.console.tag_config(level, foreground=color)
            self.console.insert(tk.END, f"[{timestamp}] ", 'INFO')
            self.console.insert(tk.END, f"{message}\n", level)
            self.console.see(tk.END)
        
        if threading.current_thread() != threading.main_thread():
            self.root.after(0, _log)
        else:
            _log()
        
    def refresh_ports(self):
        ports = serial.tools.list_ports.comports()
        port_list = [p.device for p in ports]
        self.port_combo['values'] = port_list
        if port_list:
            self.port_combo.current(0)
            self.log(f"Found {len(port_list)} serial port(s)")
        else:
            self.log("No serial ports found", 'WARN')
            
    def toggle_connection(self):
        if self.serial_port and self.serial_port.is_open:
            self.disconnect()
        else:
            self.connect()
            
    def connect(self):
        port = self.port_combo.get()
        baud = int(self.baud_combo.get())
        
        if not port:
            messagebox.showerror("Error", "Please select a serial port")
            return
            
        try:
            # Open serial with proper settings
            self.serial_port = serial.Serial(
                port=port,
                baudrate=baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1,
                write_timeout=2,
                xonxoff=False,       # Disable software flow control
                rtscts=False,        # Disable RTS/CTS flow control
                dsrdtr=False         # Disable DSR/DTR flow control
            )
            
            # Clear buffers
            self.serial_port.reset_input_buffer()
            self.serial_port.reset_output_buffer()
            
            # Small delay after opening
            threading.Event().wait(0.1)
            
            self.status_label.config(text=f"Connected to {port}", foreground="green")
            self.connect_btn.config(text="Disconnect")
            self.scan_btn.config(state='normal')
            self.test_btn.config(state='normal')
            self.log(f"Connected to {port} at {baud} baud", 'SUCCESS')
            self.log(f"Flow control: OFF, Timeout: 1s", 'DEBUG')
            
            # Start reading thread
            self.reading_thread = threading.Thread(target=self.read_serial, daemon=True)
            self.reading_thread.start()
            
        except Exception as e:
            messagebox.showerror("Connection Error", str(e))
            self.log(f"Connection failed: {e}", 'ERROR')
            
    def disconnect(self):
        if self.serial_port:
            try:
                self.serial_port.close()
            except:
                pass
            self.serial_port = None
        self.status_label.config(text="Disconnected", foreground="red")
        self.connect_btn.config(text="Connect")
        self.scan_btn.config(state='disabled')
        self.test_btn.config(state='disabled')
        self.log("Disconnected")
    
    def test_connection(self):
        """Test serial connection"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return
            
        try:
            self.log("="*60, 'DEBUG')
            self.log("Testing connection...", 'INFO')
            self.log(f"Port: {self.serial_port.port}", 'DEBUG')
            self.log(f"Baud: {self.serial_port.baudrate}", 'DEBUG')
            self.log(f"Is Open: {self.serial_port.is_open}", 'DEBUG')
            self.log(f"In Waiting: {self.serial_port.in_waiting}", 'DEBUG')
            
            # Send test
            test_cmd = b"\r\n"
            bytes_written = self.serial_port.write(test_cmd)
            self.serial_port.flush()
            self.log(f"Sent {bytes_written} bytes (CRLF)", 'SUCCESS')
            self.log("="*60, 'DEBUG')
            
            messagebox.showinfo("Test", "Test command sent successfully!\nCheck console for details.")
            
        except Exception as e:
            self.log(f"Connection test failed: {e}", 'ERROR')
            self.log(traceback.format_exc(), 'ERROR')
            messagebox.showerror("Error", f"Test failed: {e}")
        
    def read_serial(self):
        buffer = ""
        while self.serial_port and self.serial_port.is_open:
            try:
                if self.serial_port.in_waiting:
                    data = self.serial_port.read(self.serial_port.in_waiting).decode('utf-8', errors='ignore')
                    buffer += data
                    
                    while '\n' in buffer:
                        line, buffer = buffer.split('\n', 1)
                        line = line.strip()
                        
                        if line:
                            self.process_line(line)
                            
            except Exception as e:
                self.log(f"Read error: {e}", 'ERROR')
                break
                
    def process_line(self, line):
        # Skip ESP-IDF log messages
        if line.startswith('I (') or line.startswith('W (') or line.startswith('E ('):
            return
            
        if line == "CFSC_RESP:START":
            self.reading_config = True
            self.config_buffer = []
            self.log("Started reading config...", 'INFO')
            return
        elif line == "CFSC_RESP:END":
            self.reading_config = False
            self.parse_config(self.config_buffer)
            self.log("Config reading completed", 'SUCCESS')
            return
            
        if self.reading_config:
            self.config_buffer.append(line)
            
    def parse_config(self, lines):
        """Parse configuration lines into structured data"""
        self.config_data = {
            'GATEWAY_INFO': {},
            'WAN_CONFIG': {},
            'LAN_CONFIG': {}
        }
        
        current_section = None
        
        for line in lines:
            line = line.strip()
            if not line:
                continue
                
            if line.startswith('[') and line.endswith(']'):
                current_section = line[1:-1]
                continue
                
            if '=' in line and current_section:
                key, value = line.split('=', 1)
                key = key.strip()
                value = value.strip()
                
                if current_section in self.config_data:
                    self.config_data[current_section][key] = value
        
        # Store original for comparison
        self.original_config = deepcopy(self.config_data)
        self.config_modified = False
        
        self.display_config()
        self.save_btn.config(state='normal')
        self.update_changes_status()
        
    def display_config(self):
        """Display configuration in both views"""
        # Raw view
        self.raw_text.delete('1.0', tk.END)
        raw_content = self.format_config_raw()
        self.raw_text.insert('1.0', raw_content)
        
        # Structured tree view
        self.tree.delete(*self.tree.get_children())
        
        for section, params in self.config_data.items():
            section_node = self.tree.insert('', 'end', text=f'[{section}]', 
                                           values=('', ''), open=True,
                                           tags=('section',))
            self.tree.tag_configure('section', font=('TkDefaultFont', 10, 'bold'))
            
            for key, value in params.items():
                # Check if changed
                status = ''
                tag = ''
                if section in self.original_config:
                    orig_value = self.original_config[section].get(key, '')
                    if orig_value != value:
                        status = 'Changed'
                        tag = 'changed'
                
                self.tree.insert(section_node, 'end', text=key, 
                               values=(value, status), tags=(tag,))
        
        self.tree.tag_configure('changed', foreground='blue', font=('TkDefaultFont', 9, 'bold'))
        
    def format_config_raw(self):
        """Format config data as raw text"""
        lines = []
        for section, params in self.config_data.items():
            lines.append(f"[{section}]")
            for key, value in params.items():
                lines.append(f"{key}={value}")
            lines.append("")
        return '\n'.join(lines)
    
    def edit_tree_item(self, event):
        """Edit item on double-click"""
        self.edit_selected_item()
        
    def edit_selected_item(self):
        """Edit selected tree item"""
        selection = self.tree.selection()
        if not selection:
            messagebox.showwarning("Warning", "Please select an item to edit")
            return
            
        item = selection[0]
        parent = self.tree.parent(item)
        
        if not parent:  # Section header
            return
            
        # Get current values
        key = self.tree.item(item, 'text')
        current_value = self.tree.item(item, 'values')[0]
        
        # Get section name
        section = self.tree.item(parent, 'text').strip('[]')
        
        # Skip read-only fields
        if section == 'GATEWAY_INFO':
            messagebox.showinfo("Info", "Gateway info is read-only")
            return
            
        # Create edit dialog
        dialog = tk.Toplevel(self.root)
        dialog.title(f"Edit: {key}")
        dialog.geometry("450x180")
        dialog.transient(self.root)
        dialog.grab_set()
        
        # Center dialog
        dialog.update_idletasks()
        x = (dialog.winfo_screenwidth() // 2) - (dialog.winfo_width() // 2)
        y = (dialog.winfo_screenheight() // 2) - (dialog.winfo_height() // 2)
        dialog.geometry(f"+{x}+{y}")
        
        ttk.Label(dialog, text=f"Key: {key}", font=('TkDefaultFont', 10, 'bold')).pack(pady=15)
        
        ttk.Label(dialog, text="Value:").pack()
        entry = ttk.Entry(dialog, width=50, font=('Courier New', 10))
        entry.insert(0, current_value)
        entry.pack(pady=10, padx=20)
        entry.focus()
        entry.select_range(0, tk.END)
        
        def save_edit():
            new_value = entry.get()
            if new_value != current_value:
                self.config_data[section][key] = new_value
                self.config_modified = True
                self.display_config()
                self.update_changes_status()
                self.log(f"Modified: {section}.{key} = {new_value}", 'INFO')
            dialog.destroy()
            
        def cancel_edit():
            dialog.destroy()
            
        btn_frame = ttk.Frame(dialog)
        btn_frame.pack(pady=15)
        ttk.Button(btn_frame, text="Save", command=save_edit, width=12).pack(side=tk.LEFT, padx=5)
        ttk.Button(btn_frame, text="Cancel", command=cancel_edit, width=12).pack(side=tk.LEFT, padx=5)
        
        entry.bind('<Return>', lambda e: save_edit())
        entry.bind('<Escape>', lambda e: cancel_edit())
        
    def update_changes_status(self):
        """Update changes counter"""
        changes = self.get_changed_fields()
        count = sum(len(fields) for fields in changes.values())
        
        if count > 0:
            self.changes_label.config(text=f"{count} field(s) modified", foreground="orange")
            self.write_btn.config(state='normal')
        else:
            self.changes_label.config(text="No changes", foreground="green")
            self.write_btn.config(state='disabled')
            
    def get_changed_fields(self) -> Dict[str, Dict[str, str]]:
        """Get dictionary of changed fields"""
        changes = {'WAN_CONFIG': {}, 'LAN_CONFIG': {}}
        
        for section in ['WAN_CONFIG', 'LAN_CONFIG']:
            if section not in self.config_data or section not in self.original_config:
                continue
                
            for key, value in self.config_data[section].items():
                orig_value = self.original_config[section].get(key, '')
                if orig_value != value and value not in ['', '***HIDDEN***']:
                    changes[section][key] = value
                    
        return changes
    
    def revert_changes(self):
        """Revert all changes"""
        if not self.config_modified:
            messagebox.showinfo("Info", "No changes to revert")
            return
            
        if messagebox.askyesno("Confirm", "Revert all changes?"):
            self.config_data = deepcopy(self.original_config)
            self.config_modified = False
            self.display_config()
            self.update_changes_status()
            self.log("Changes reverted", 'INFO')
        
    def read_config(self):
        """Send CFSC command to read configuration"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return
            
        try:
            # Debug logging
            self.log("="*60, 'DEBUG')
            self.log("Sending CFSC command...", 'INFO')
            self.log(f"Port: {self.serial_port.port}", 'DEBUG')
            self.log(f"Baud: {self.serial_port.baudrate}", 'DEBUG')
            self.log(f"In Waiting: {self.serial_port.in_waiting}", 'DEBUG')
            
            # Clear any pending data
            if self.serial_port.in_waiting > 0:
                junk = self.serial_port.read(self.serial_port.in_waiting)
                self.log(f"Cleared {len(junk)} bytes from buffer", 'WARN')
            
            # Send command with CRLF
            cmd = b"CFSC\r\n"
            self.log(f"Command: {cmd}", 'DEBUG')
            bytes_written = self.serial_port.write(cmd)
            self.serial_port.flush()
            
            self.log(f"Sent {bytes_written} bytes, waiting for response...", 'SUCCESS')
            self.log("="*60, 'DEBUG')
            
        except Exception as e:
            self.log(f"Failed to send command: {e}", 'ERROR')
            self.log(traceback.format_exc(), 'ERROR')
            messagebox.showerror("Error", f"Failed to send command: {e}")
    
    def write_incremental_config(self):
        """Write only changed configuration fields"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return
            
        changes = self.get_changed_fields()
        total_changes = sum(len(fields) for fields in changes.values())
        
        if total_changes == 0:
            messagebox.showinfo("Info", "No changes to write")
            return
            
        # Show confirmation with change summary
        summary = []
        for section, fields in changes.items():
            if fields:
                summary.append(f"{section}:")
                for key, value in fields.items():
                    display_val = value if value != '***HIDDEN***' else '[hidden]'
                    summary.append(f"  - {key} = {display_val}")
                    
        msg = f"Write {total_changes} changed field(s)?\n\n" + '\n'.join(summary)
        
        if not messagebox.askyesno("Confirm Write", msg):
            return
            
        try:
            commands = self.generate_incremental_commands(changes)
            self.log("="*60, 'INFO')
            self.log(f"Writing {len(commands)} configuration command(s)...", 'INFO')
            
            for i, cmd in enumerate(commands, 1):
                # Add CRLF and flush after each command
                full_cmd = f"{cmd}\r\n"
                self.serial_port.write(full_cmd.encode('utf-8'))
                self.serial_port.flush()
                
                display_cmd = cmd[:50] + '...' if len(cmd) > 50 else cmd
                self.log(f"  [{i}/{len(commands)}] {display_cmd}", 'INFO')
                threading.Event().wait(0.15)  # 150ms delay between commands
            
            self.log("="*60, 'INFO')
            self.log("Configuration written successfully", 'SUCCESS')
            
            # Update original config
            self.original_config = deepcopy(self.config_data)
            self.config_modified = False
            self.update_changes_status()
            
            messagebox.showinfo("Success", f"{len(commands)} command(s) sent successfully")
            
        except Exception as e:
            self.log(f"Write failed: {e}", 'ERROR')
            self.log(traceback.format_exc(), 'ERROR')
            messagebox.showerror("Error", f"Failed to write config: {e}")
    
    def generate_incremental_commands(self, changes: Dict[str, Dict[str, str]]) -> list:
        """Generate CF commands only for changed fields"""
        commands = []
        
        # WAN config
        for key, value in changes.get('WAN_CONFIG', {}).items():
            if key in WAN_CONFIG_MAP:
                cmd_prefix = WAN_CONFIG_MAP[key]
                commands.append(f"{cmd_prefix}{value}")
            else:
                self.log(f"Warning: No command mapping for {key}", 'WARN')
        
        # LAN config
        for key, value in changes.get('LAN_CONFIG', {}).items():
            if key in LAN_CONFIG_MAP:
                cmd_prefix = LAN_CONFIG_MAP[key]
                commands.append(f"{cmd_prefix}{value}")
                
        return commands
            
    def save_to_file(self):
        """Save configuration to JSON file"""
        if not self.config_data:
            messagebox.showwarning("Warning", "No configuration data to save")
            return
            
        filename = filedialog.asksaveasfilename(
            defaultextension=".json",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")],
            initialfile=f"gateway_config_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
        )
        
        if filename:
            try:
                with open(filename, 'w', encoding='utf-8') as f:
                    json.dump(self.config_data, f, indent=2, ensure_ascii=False)
                self.log(f"Configuration saved to {filename}", 'SUCCESS')
                messagebox.showinfo("Success", "Configuration saved successfully")
            except Exception as e:
                self.log(f"Save failed: {e}", 'ERROR')
                messagebox.showerror("Error", f"Failed to save file: {e}")
                
    def load_from_file(self):
        """Load configuration from JSON file"""
        filename = filedialog.askopenfilename(
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )
        
        if filename:
            try:
                with open(filename, 'r', encoding='utf-8') as f:
                    loaded_config = json.load(f)
                
                # Merge with current config
                for section, params in loaded_config.items():
                    if section not in self.config_data:
                        self.config_data[section] = {}
                    self.config_data[section].update(params)
                
                self.config_modified = True
                self.display_config()
                self.save_btn.config(state='normal')
                self.update_changes_status()
                self.log(f"Configuration loaded from {filename}", 'SUCCESS')
                messagebox.showinfo("Success", "Configuration loaded successfully")
            except Exception as e:
                self.log(f"Load failed: {e}", 'ERROR')
                messagebox.showerror("Error", f"Failed to load file: {e}")
    
    def update_firmware(self):
        """Update firmware using flash_WAN.bat"""
        flash_script = self.app_path / "flash_WAN.bat"
        
        self.log(f"Looking for flash script in: {self.app_path}", 'DEBUG')
        
        if not flash_script.exists():
            error_msg = (
                f"flash_WAN.bat not found!\n\n"
                f"Expected location:\n{flash_script}\n\n"
                f"Please place flash_WAN.bat and firmware files (.bin)\n"
                f"in the same folder as this tool:\n{self.app_path}"
            )
            messagebox.showerror("Error", error_msg)
            self.log(f"flash_WAN.bat not found in {self.app_path}", 'ERROR')
            return
        
        # Disconnect serial if connected
        port = None
        if self.serial_port and self.serial_port.is_open:
            port = self.serial_port.port
            self.disconnect()
            threading.Event().wait(1)
        else:
            port = self.port_combo.get()
            
        if not port:
            messagebox.showerror("Error", "Please select a COM port")
            return
        
        # Ask which firmware to flash
        dialog = tk.Toplevel(self.root)
        dialog.title("Firmware Update")
        dialog.geometry("380x220")
        dialog.transient(self.root)
        dialog.grab_set()
        
        # Center dialog
        dialog.update_idletasks()
        x = (dialog.winfo_screenwidth() // 2) - (dialog.winfo_width() // 2)
        y = (dialog.winfo_screenheight() // 2) - (dialog.winfo_height() // 2)
        dialog.geometry(f"+{x}+{y}")
        
        ttk.Label(dialog, text="Select firmware to update:", 
                 font=('TkDefaultFont', 11, 'bold')).pack(pady=15)
        
        var = tk.StringVar(value="-WAN")
        
        radio_frame = ttk.Frame(dialog)
        radio_frame.pack(pady=10)
        
        ttk.Radiobutton(radio_frame, text="WAN MCU Only", variable=var, 
                       value="-WAN").pack(anchor=tk.W, padx=50, pady=5)
        ttk.Radiobutton(radio_frame, text="LAN MCU Only", variable=var, 
                       value="-LAN").pack(anchor=tk.W, padx=50, pady=5)
        ttk.Radiobutton(radio_frame, text="Both WAN and LAN", variable=var, 
                       value="").pack(anchor=tk.W, padx=50, pady=5)
        
        btn_frame = ttk.Frame(dialog)
        btn_frame.pack(pady=15)
        
        def start_flash():
            target = var.get()
            dialog.destroy()
            self.run_flash_script(port, target)
        
        def cancel():
            dialog.destroy()
            
        ttk.Button(btn_frame, text="Start Update", 
                  command=start_flash, width=15).pack(side=tk.LEFT, padx=5)
        ttk.Button(btn_frame, text="Cancel", 
                  command=cancel, width=15).pack(side=tk.LEFT, padx=5)
        
    def run_flash_script(self, port, target):
        """Run flash script with real-time logging"""
        target_name = target if target else 'BOTH'
        self.log(f"Starting firmware update: {target_name}", 'INFO')
        
        def flash_thread():
            try:
                flash_script = self.app_path / "flash_WAN.bat"
                
                # Check required files
                required_files = [flash_script]
                if not target or target == "-WAN":
                    required_files.extend([
                        self.app_path / "bootloader.bin",
                        self.app_path / "partition-table.bin",
                        self.app_path / "DA2_esp.bin"
                    ])
                if not target or target == "-LAN":
                    required_files.extend([
                        self.app_path / "bootloaderLAN.bin",
                        self.app_path / "partition-tableLAN.bin",
                        self.app_path / "DA2_espLAN.bin"
                    ])
                
                missing = [f.name for f in required_files if not f.exists()]
                if missing:
                    error_msg = f"Missing files:\n" + "\n".join(f"  - {f}" for f in missing)
                    self.log(error_msg, 'ERROR')
                    self.root.after(0, lambda: messagebox.showerror(
                        "Error", 
                        f"{error_msg}\n\nPlease place all files in:\n{self.app_path}"))
                    return
                
                # Build command
                cmd = f'"{flash_script}" {port}'
                if target:
                    cmd += f' {target}'
                
                self.log("="*70, 'INFO')
                self.log(f"Command: {cmd}", 'DEBUG')
                self.log(f"Working Dir: {self.app_path}", 'DEBUG')
                self.log("="*70, 'INFO')
                
                # Run with real-time output
                process = subprocess.Popen(
                    cmd,
                    shell=True,
                    cwd=str(self.app_path),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding='utf-8',
                    errors='ignore',
                    bufsize=1,
                    universal_newlines=True
                )
                
                # Read output line by line
                output_lines = []
                for line in iter(process.stdout.readline, ''):
                    if line:
                        line = line.rstrip()
                        output_lines.append(line)
                        
                        # Color-coded logging
                        if 'ERROR' in line.upper() or 'FAILED' in line.upper():
                            self.log(line, 'ERROR')
                        elif 'SUCCESS' in line.upper() or ' OK' in line or 'completed successfully' in line.lower():
                            self.log(line, 'SUCCESS')
                        elif line.startswith('[') or 'Hash of data verified' in line:
                            self.log(line, 'INFO')
                        else:
                            self.log(line, 'DEBUG')
                
                # Wait for completion
                return_code = process.wait()
                
                self.log("="*70, 'INFO')
                self.log(f"Process completed with return code: {return_code}", 
                        'SUCCESS' if return_code == 0 else 'ERROR')
                
                # Check for success
                success = return_code == 0
                
                if success:
                    self.log("Firmware update completed successfully!", 'SUCCESS')
                    self.root.after(0, lambda: messagebox.showinfo(
                        "Success", 
                        "Firmware updated successfully!\n\n"
                        "Please reset/power-cycle the device."))
                else:
                    self.log(f"Flash failed with return code: {return_code}", 'ERROR')
                    # Show last error lines
                    error_lines = [l for l in output_lines if 'ERROR' in l.upper() or 'FAILED' in l.upper()]
                    if error_lines:
                        self.log("Last errors:", 'ERROR')
                        for err in error_lines[-5:]:
                            self.log(f"  {err}", 'ERROR')
                    
                    self.root.after(0, lambda: messagebox.showerror(
                        "Error", 
                        f"Firmware update failed (code {return_code})\n\n"
                        "Check console log for details."))
                    
            except Exception as e:
                self.log(f"Flash exception: {e}", 'ERROR')
                self.log(traceback.format_exc(), 'ERROR')
                self.root.after(0, lambda: messagebox.showerror(
                    "Error", 
                    f"Failed to run flash script:\n{e}"))
        
        threading.Thread(target=flash_thread, daemon=True).start()
        
    def clear_config(self):
        """Clear all configuration data"""
        if self.config_modified:
            if not messagebox.askyesno("Confirm", "Clear all data? Unsaved changes will be lost."):
                return
                
        self.config_data = {}
        self.original_config = {}
        self.config_modified = False
        self.raw_text.delete('1.0', tk.END)
        self.tree.delete(*self.tree.get_children())
        self.save_btn.config(state='disabled')
        self.write_btn.config(state='disabled')
        self.update_changes_status()
        self.log("Configuration cleared")

def main():
    root = tk.Tk()
    
    # Set default font to support Unicode
    default_font = ('Segoe UI', 9)
    root.option_add("*Font", default_font)
    
    app = GatewayConfigTool(root)
    
    # Handle window close
    def on_closing():
        if app.serial_port and app.serial_port.is_open:
            app.disconnect()
        root.destroy()
    
    root.protocol("WM_DELETE_WINDOW", on_closing)
    root.mainloop()

if __name__ == '__main__':
    main()
