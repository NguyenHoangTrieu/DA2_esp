#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32 IoT Gateway Configuration Tool v3.1
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
from typing import Dict, Any, List
from datetime import datetime
from copy import deepcopy
import traceback

# Import the protocol module
try:
    from config_protocol import (
        ConfigCommandBuilder, WiFiConfig, LTEConfig, MQTTConfig,
        CANConfig, CANWhitelist, LoRaModemConfig, LoRaTDMAConfig,
        StackTypeConfig,
        ProtocolValidator, ConfigResponseParser,
    )
    PROTOCOL_AVAILABLE = True
except ImportError:
    print("Warning: config_protocol.py not found, using fallback mode")
    PROTOCOL_AVAILABLE = False

def get_application_path():
    """Get the path where the application/script is located"""
    if getattr(sys, 'frozen', False):
        return Path(sys.executable).parent
    else:
        return Path(__file__).parent

class GatewayConfigTool:
    READ_ONLY_FIELDS = {
        'GATEWAY_INFO': '*',  # All fields read-only
        'LAN_CONFIG': ['can_whitelist_count', 'can_whitelist', 'lora_e32_baud', 'lora_e32_header'],  # Specific read-only fields
    }

    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 Gateway Config Tool v3.1")
        self.root.geometry("1200x800")
        self.serial_port = None
        self.config_data = {}
        self.original_config = {}
        self.reading_config = False
        self.config_buffer = []
        self.config_modified = False
        # CAN whitelist tracking
        self.can_whitelist_ids = []
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

        self.write_btn = ttk.Button(actions_frame, text="Write Changes",
                                    command=self.write_config, state='disabled', width=20)
        self.write_btn.grid(row=0, column=1, padx=5)

        ttk.Separator(actions_frame, orient='vertical').grid(row=0, column=2, sticky='ns', padx=10)

        # NEW: Config Mode button
        self.config_mode_btn = ttk.Button(actions_frame, text="Config Mode",
                                         command=self.enter_config_mode, state='disabled', width=15)
        self.config_mode_btn.grid(row=0, column=3, padx=5)

        # NEW: Normal Mode button
        self.normal_mode_btn = ttk.Button(actions_frame, text="Normal Mode",
                                         command=self.enter_normal_mode, state='disabled', width=15)
        self.normal_mode_btn.grid(row=0, column=4, padx=5)

        ttk.Separator(actions_frame, orient='vertical').grid(row=0, column=5, sticky='ns', padx=10)

        self.save_btn = ttk.Button(actions_frame, text="Save to File",
                                   command=self.save_to_file, state='disabled', width=15)
        self.save_btn.grid(row=0, column=6, padx=5)

        self.load_btn = ttk.Button(actions_frame, text="Load from File",
                                   command=self.load_from_file, width=15)
        self.load_btn.grid(row=0, column=7, padx=5)

        ttk.Separator(actions_frame, orient='vertical').grid(row=0, column=8, sticky='ns', padx=10)

        # UPDATED: Firmware Update button (no more target selection)
        self.firmware_btn = ttk.Button(actions_frame, text="Update Firmware",
                                      command=self.update_firmware, width=20)
        self.firmware_btn.grid(row=0, column=9, padx=5)

        self.clear_btn = ttk.Button(actions_frame, text="Clear",
                                    command=self.clear_config, width=10)
        self.clear_btn.grid(row=0, column=10, padx=5)

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

        tree_frame = ttk.Frame(edit_frame)
        tree_frame.pack(fill=tk.BOTH, expand=True)

        self.tree = ttk.Treeview(tree_frame, columns=('value', 'changed'), show='tree headings')
        self.tree.heading('#0', text='Parameter')
        self.tree.heading('value', text='Value')
        self.tree.heading('changed', text='Status')
        self.tree.column('#0', width=250)
        self.tree.column('value', width=400)
        self.tree.column('changed', width=80)

        tree_scroll = ttk.Scrollbar(tree_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=tree_scroll.set)
        self.tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        tree_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        self.tree.bind('<Double-Button-1>', self.edit_tree_item)

        edit_controls = ttk.Frame(edit_frame)
        edit_controls.pack(fill=tk.X, padx=5, pady=5)

        ttk.Button(edit_controls, text="Edit Selected",
                  command=self.edit_selected_item).pack(side=tk.LEFT, padx=5)
        ttk.Button(edit_controls, text="Revert Changes",
                  command=self.revert_changes).pack(side=tk.LEFT, padx=5)
        self.changes_label = ttk.Label(edit_controls, text="No changes", foreground="green")
        self.changes_label.pack(side=tk.LEFT, padx=20)

        # NEW: CAN Whitelist tab
        can_whitelist_frame = ttk.Frame(self.notebook)
        self.notebook.add(can_whitelist_frame, text="CAN Whitelist")
        self.setup_can_whitelist_tab(can_whitelist_frame)

        # ===== Console Panel =====
        console_frame = ttk.LabelFrame(main_frame, text="Console Log", padding="5")
        console_frame.grid(row=3, column=0, columnspan=2, sticky=(tk.W, tk.E), pady=5)
        self.console = scrolledtext.ScrolledText(console_frame, height=8,
                                                font=('Courier New', 9),
                                                bg='#1e1e1e', fg='#00ff00')
        self.console.pack(fill=tk.BOTH, expand=True)

    def setup_can_whitelist_tab(self, parent):
        """Setup CAN Whitelist management tab"""
        # Top frame with current whitelist
        top_frame = ttk.LabelFrame(parent, text="Current Whitelist", padding="10")
        top_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=5)

        # Listbox to show whitelist IDs
        list_frame = ttk.Frame(top_frame)
        list_frame.pack(fill=tk.BOTH, expand=True)

        self.whitelist_listbox = tk.Listbox(list_frame, font=('Courier New', 10), height=10)
        self.whitelist_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

        list_scroll = ttk.Scrollbar(list_frame, orient="vertical", command=self.whitelist_listbox.yview)
        self.whitelist_listbox.configure(yscrollcommand=list_scroll.set)
        list_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        # Controls frame
        controls_frame = ttk.Frame(parent, padding="5")
        controls_frame.pack(fill=tk.X, padx=5, pady=5)

        # Add ID section
        add_frame = ttk.LabelFrame(controls_frame, text="Add CAN ID", padding="5")
        add_frame.pack(side=tk.LEFT, padx=5)

        ttk.Label(add_frame, text="CAN ID (hex):").grid(row=0, column=0, padx=5)
        self.can_id_entry = ttk.Entry(add_frame, width=12)
        self.can_id_entry.grid(row=0, column=1, padx=5)
        self.can_id_entry.insert(0, "0x")
        ttk.Button(add_frame, text="Add", command=self.add_can_whitelist_id).grid(row=0, column=2, padx=5)

        # Action buttons
        action_frame = ttk.Frame(controls_frame)
        action_frame.pack(side=tk.LEFT, padx=20)

        ttk.Button(action_frame, text="Remove Selected",
                  command=self.remove_can_whitelist_id, width=18).pack(pady=2)
        ttk.Button(action_frame, text="Clear All",
                  command=self.clear_can_whitelist, width=18).pack(pady=2)
        ttk.Button(action_frame, text="Apply to Gateway",
                  command=self.apply_can_whitelist, width=18).pack(pady=2)

    # NEW: Config Mode function
    def enter_config_mode(self):
        """Send CONFIG command to enter config mode"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        try:
            self.serial_port.write(b"CONFIG\r\n")
            self.serial_port.flush()
            self.log("Sent CONFIG command - MCU entering config mode", 'SUCCESS')
            messagebox.showinfo("Success", "Config mode command sent")
        except Exception as e:
            self.log(f"Failed to send CONFIG: {e}", 'ERROR')
            messagebox.showerror("Error", str(e))

    # NEW: Normal Mode function
    def enter_normal_mode(self):
        """Send NORMAL command to return to normal mode"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        try:
            self.serial_port.write(b"NORMAL\r\n")
            self.serial_port.flush()
            self.log("Sent NORMAL command - MCU returning to normal mode", 'SUCCESS')
            messagebox.showinfo("Success", "Normal mode command sent")
        except Exception as e:
            self.log(f"Failed to send NORMAL: {e}", 'ERROR')
            messagebox.showerror("Error", str(e))

    def add_can_whitelist_id(self):
        """Add CAN ID to whitelist"""
        id_str = self.can_id_entry.get().strip()
        if not id_str:
            messagebox.showwarning("Warning", "Please enter a CAN ID")
            return

        try:
            # Parse hex value
            can_id = int(id_str, 16)

            # Validate
            if PROTOCOL_AVAILABLE:
                valid, msg = ProtocolValidator.validate_can_id(can_id, extended=False)
                if not valid:
                    messagebox.showerror("Error", msg)
                    return

            # Check if already exists
            if can_id in self.can_whitelist_ids:
                messagebox.showinfo("Info", f"ID {id_str} already in whitelist")
                return

            # Add to list
            self.can_whitelist_ids.append(can_id)
            self.can_whitelist_ids.sort()
            self.update_whitelist_display()
            self.config_modified = True
            self.update_changes_status()
            self.log(f"Added CAN ID {id_str} to whitelist", 'INFO')

            # Clear entry
            self.can_id_entry.delete(0, tk.END)
            self.can_id_entry.insert(0, "0x")

        except ValueError:
            messagebox.showerror("Error", "Invalid hex format. Use 0xXXX")

    def remove_can_whitelist_id(self):
        """Remove selected CAN ID from whitelist"""
        selection = self.whitelist_listbox.curselection()
        if not selection:
            messagebox.showwarning("Warning", "Please select an ID to remove")
            return

        idx = selection[0]
        removed_id = self.can_whitelist_ids.pop(idx)
        self.update_whitelist_display()
        self.config_modified = True
        self.update_changes_status()
        self.log(f"Removed CAN ID 0x{removed_id:03X} from whitelist", 'INFO')

    def clear_can_whitelist(self):
        """Clear entire whitelist"""
        if not self.can_whitelist_ids:
            messagebox.showinfo("Info", "Whitelist is already empty")
            return

        if messagebox.askyesno("Confirm", "Clear entire whitelist?"):
            self.can_whitelist_ids.clear()
            self.update_whitelist_display()
            self.config_modified = True
            self.update_changes_status()
            self.log("Cleared CAN whitelist", 'INFO')

    def apply_can_whitelist(self):
        """Apply whitelist to gateway"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        if not PROTOCOL_AVAILABLE:
            messagebox.showerror("Error", "config_protocol.py not available")
            return

        try:
            if len(self.can_whitelist_ids) == 0:
                # Clear whitelist on gateway
                cmd = ConfigCommandBuilder.build_can_whitelist_clear()
                self.serial_port.write(f"{cmd}\r\n".encode('utf-8'))
                self.serial_port.flush()
                self.log(f"Sent: {cmd}", 'SUCCESS')
            else:
                # Set entire whitelist
                cmd = ConfigCommandBuilder.build_can_whitelist_set(self.can_whitelist_ids)
                self.serial_port.write(f"{cmd}\r\n".encode('utf-8'))
                self.serial_port.flush()
                self.log(f"Sent: {cmd}", 'SUCCESS')

            messagebox.showinfo("Success", "CAN whitelist applied to gateway")
        except Exception as e:
            self.log(f"Failed to apply whitelist: {e}", 'ERROR')
            messagebox.showerror("Error", str(e))

    def update_whitelist_display(self):
        """Update whitelist listbox"""
        self.whitelist_listbox.delete(0, tk.END)
        for can_id in self.can_whitelist_ids:
            self.whitelist_listbox.insert(tk.END, f"0x{can_id:03X}")

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
            self.serial_port = serial.Serial(
                port=port,
                baudrate=baud,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1,
                write_timeout=2,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False
            )
            self.serial_port.reset_input_buffer()
            self.serial_port.reset_output_buffer()
            threading.Event().wait(0.1)

            self.status_label.config(text=f"Connected to {port}", foreground="green")
            self.connect_btn.config(text="Disconnect")
            self.scan_btn.config(state='normal')
            self.test_btn.config(state='normal')
            self.config_mode_btn.config(state='normal')  # Enable config mode button
            self.normal_mode_btn.config(state='normal')  # Enable normal mode button
            self.log(f"Connected to {port} at {baud} baud", 'SUCCESS')

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
        self.config_mode_btn.config(state='disabled')  # Disable config mode button
        self.normal_mode_btn.config(state='disabled')  # Disable normal mode button
        self.log("Disconnected")

    def test_connection(self):
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        try:
            self.log("="*60, 'DEBUG')
            self.log("Testing connection...", 'INFO')
            test_cmd = b"\r\n"
            bytes_written = self.serial_port.write(test_cmd)
            self.serial_port.flush()
            self.log(f"Sent {bytes_written} bytes (CRLF)", 'SUCCESS')
            self.log("="*60, 'DEBUG')
            messagebox.showinfo("Test", "Test command sent successfully!")
        except Exception as e:
            self.log(f"Test failed: {e}", 'ERROR')
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
        """Parse configuration lines"""
        if PROTOCOL_AVAILABLE:
            self.config_data = ConfigResponseParser.parse_config_dump(lines)
        else:
            # Fallback parsing
            self.config_data = {'GATEWAY_INFO': {}, 'WAN_CONFIG': {}, 'LAN_CONFIG': {}}
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
                    if current_section in self.config_data:
                        self.config_data[current_section][key.strip()] = value.strip()

        # Parse CAN whitelist if present
        if 'can_whitelist' in self.config_data.get('LAN_CONFIG', {}):
            whitelist_str = self.config_data['LAN_CONFIG']['can_whitelist']
            if PROTOCOL_AVAILABLE:
                self.can_whitelist_ids = ConfigResponseParser.parse_can_whitelist(whitelist_str)
                self.update_whitelist_display()

        self.original_config = deepcopy(self.config_data)
        self.config_modified = False
        self.display_config()
        self.save_btn.config(state='normal')
        self.update_changes_status()

    def display_config(self):
        """Display configuration"""
        # Raw view
        self.raw_text.delete('1.0', tk.END)
        raw_content = self.format_config_raw()
        self.raw_text.insert('1.0', raw_content)

        # Tree view
        self.tree.delete(*self.tree.get_children())
        for section, params in self.config_data.items():
            section_node = self.tree.insert('', 'end', text=f'[{section}]',
                                           values=('', ''), open=True, tags=('section',))
            self.tree.tag_configure('section', font=('TkDefaultFont', 10, 'bold'))

            for key, value in params.items():
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
        lines = []
        for section, params in self.config_data.items():
            lines.append(f"[{section}]")
            for key, value in params.items():
                lines.append(f"{key}={value}")
            lines.append("")
        return '\n'.join(lines)

    def edit_tree_item(self, event):
        self.edit_selected_item()

    def edit_selected_item(self):
        """Edit selected item - with READ-ONLY protection"""
        selection = self.tree.selection()
        if not selection:
            messagebox.showwarning("Warning", "Please select an item to edit")
            return

        item = selection[0]
        parent = self.tree.parent(item)
        if not parent:
            return

        key = self.tree.item(item, 'text')
        current_value = self.tree.item(item, 'values')[0]
        section = self.tree.item(parent, 'text').strip('[]')

        # ===== CHECK READ-ONLY =====
        if section in self.READ_ONLY_FIELDS:
            if self.READ_ONLY_FIELDS[section] == '*':  # All fields read-only
                messagebox.showinfo("Info", f"{section} is read-only")
                return
            elif key in self.READ_ONLY_FIELDS[section]:  # Specific field read-only
                messagebox.showinfo("Info", f"'{key}' is read-only. Use CAN Whitelist tab to manage whitelist.")
                return

        # Create edit dialog
        dialog = tk.Toplevel(self.root)
        dialog.title(f"Edit: {key}")
        dialog.geometry("500x200")
        dialog.transient(self.root)
        dialog.grab_set()
        dialog.update_idletasks()
        x = (dialog.winfo_screenwidth() // 2) - (dialog.winfo_width() // 2)
        y = (dialog.winfo_screenheight() // 2) - (dialog.winfo_height() // 2)
        dialog.geometry(f"+{x}+{y}")

        ttk.Label(dialog, text=f"Key: {key}", font=('TkDefaultFont', 10, 'bold')).pack(pady=15)
        ttk.Label(dialog, text="Value:").pack()
        entry = ttk.Entry(dialog, width=60, font=('Courier New', 10))
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
        changes = self.get_changed_count()
        if changes > 0 or self.config_modified:
            self.changes_label.config(text=f"{changes} field(s) modified", foreground="orange")
            self.write_btn.config(state='normal')
        else:
            self.changes_label.config(text="No changes", foreground="green")
            self.write_btn.config(state='disabled')

    def get_changed_count(self) -> int:
        """Count changed fields"""
        count = 0
        for section in ['WAN_CONFIG', 'LAN_CONFIG']:
            if section not in self.config_data or section not in self.original_config:
                continue
            for key, value in self.config_data[section].items():
                orig_value = self.original_config[section].get(key, '')
                if orig_value != value and value not in ['', '***HIDDEN***']:
                    count += 1
        return count

    def revert_changes(self):
        """Revert all changes"""
        if not self.config_modified and self.get_changed_count() == 0:
            messagebox.showinfo("Info", "No changes to revert")
            return

        if messagebox.askyesno("Confirm", "Revert all changes?"):
            self.config_data = deepcopy(self.original_config)
            self.config_modified = False
            self.can_whitelist_ids = []  # Reset whitelist
            self.update_whitelist_display()
            self.display_config()
            self.update_changes_status()
            self.log("Changes reverted", 'INFO')

    def read_config(self):
        """Send CFSC command"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        try:
            self.serial_port.write(b"CFSC\r\n")
            self.serial_port.flush()
            self.log("Sent CFSC command, waiting for response...")
        except Exception as e:
            self.log(f"Failed to send command: {e}", 'ERROR')

    def write_config(self):
        """Write configuration using proper protocol"""
        if not self.serial_port or not self.serial_port.is_open:
            messagebox.showerror("Error", "Not connected")
            return

        if not PROTOCOL_AVAILABLE:
            messagebox.showerror("Error", "config_protocol.py not available")
            return

        try:
            commands = self.generate_config_commands()

            if not commands:
                messagebox.showinfo("Info", "No changes to write")
                return

            # Build summary (handle both str and bytes)
            summary_lines = []
            for cmd in commands[:10]:
                if isinstance(cmd, bytes):
                    # For bytes, show hex representation
                    summary_lines.append(f"[BINARY] {cmd[:20].hex()}{'...' if len(cmd) > 20 else ''}")
                else:
                    summary_lines.append(str(cmd)[:60] + ('...' if len(str(cmd)) > 60 else ''))

            summary = "\n".join(summary_lines)
            if len(commands) > 10:
                summary += f"\n... and {len(commands)-10} more"

            msg = f"Send {len(commands)} command(s)?\n\n{summary}"
            if not messagebox.askyesno("Confirm", msg):
                return

            self.log("="*60, 'INFO')
            self.log(f"Sending {len(commands)} commands...", 'INFO')

            for i, cmd in enumerate(commands, 1):
                try:
                    if isinstance(cmd, bytes):
                        # Binary command (LoRa configs)
                        self.serial_port.write(cmd + b"\r\n")
                        self.serial_port.flush()
                        display_cmd = f"[BINARY {len(cmd)} bytes] {cmd[:30].hex()}{'...' if len(cmd) > 30 else ''}"
                        self.log(f" [{i}/{len(commands)}] {display_cmd}", 'INFO')
                    else:
                        # Text command (WiFi, MQTT, CAN, etc.)
                        self.serial_port.write(f"{cmd}\r\n".encode('utf-8'))
                        self.serial_port.flush()
                        display_cmd = str(cmd)[:60] + '...' if len(str(cmd)) > 60 else str(cmd)
                        self.log(f" [{i}/{len(commands)}] {display_cmd}", 'SUCCESS')

                    threading.Event().wait(0.15)  # Delay between commands

                except Exception as e:
                    self.log(f" [{i}/{len(commands)}] FAILED: {e}", 'ERROR')
                    raise

            self.log("="*60, 'INFO')
            self.log("Configuration written successfully", 'SUCCESS')

            # Update original config after successful write
            self.original_config = deepcopy(self.config_data)
            self.config_modified = False
            self.update_changes_status()

            messagebox.showinfo("Success", f"{len(commands)} command(s) sent successfully")

        except Exception as e:
            self.log(f"Write failed: {e}", 'ERROR')
            self.log(traceback.format_exc(), 'ERROR')
            messagebox.showerror("Error", str(e))

    @staticmethod
    def parse_int_value(value: any, default: int = 0) -> int:
        """
        Parse integer value from various formats:
        - int: return as-is
        - hex string (0xXX): parse as hex
        - decimal string: parse as decimal
        """
        if value is None:
            return default
        if isinstance(value, int):
            return value
        if isinstance(value, str):
            value = value.strip()
            if not value:
                return default
            # Hex format: 0x123 or 0X123
            if value.lower().startswith('0x'):
                try:
                    return int(value, 16)
                except ValueError:
                    return default
            # Decimal format
            try:
                return int(value)
            except ValueError:
                return default
        return default

    def generate_config_commands(self) -> List:
        """Generate ONLY commands for CHANGED fields (incremental update)"""
        commands = []
        wan_cfg = self.config_data.get('WAN_CONFIG', {})
        lan_cfg = self.config_data.get('LAN_CONFIG', {})
        orig_wan = self.original_config.get('WAN_CONFIG', {})
        orig_lan = self.original_config.get('LAN_CONFIG', {})

        # ========================================
        # Detect CHANGED field groups
        # ========================================

        # WiFi changed?
        wifi_changed = any(
            wan_cfg.get(key) != orig_wan.get(key)
            for key in ['wifi_ssid', 'wifi_password', 'wifi_username', 'wifi_auth_mode']
        )

        # MQTT changed?
        mqtt_changed = any(
            wan_cfg.get(key) != orig_wan.get(key)
            for key in ['mqtt_broker', 'mqtt_device_token', 'mqtt_sub_topic',
                       'mqtt_pub_topic', 'mqtt_attribute_topic']
        )

        # LTE changed?
        lte_changed = any(
            wan_cfg.get(key) != orig_wan.get(key)
            for key in ['lte_apn', 'lte_username', 'lte_password', 'lte_comm_type',
                       'lte_auto_reconnect', 'lte_reconnect_timeout_ms', 'lte_max_reconnect_attempts']
        )

        # CAN changed?
        can_changed = any(
            lan_cfg.get(key) != orig_lan.get(key)
            for key in ['can_baud_rate', 'can_mode']
        )

        # LoRa E32 Modem changed?
        lora_e32_changed = any(
            lan_cfg.get(key) != orig_lan.get(key)
            for key in ['lora_e32_header', 'lora_e32_addh', 'lora_e32_addl',
                       'lora_e32_sped', 'lora_e32_chan', 'lora_e32_option']
        )

        # LoRa TDMA Handler changed?
        lora_handler_changed = any(
            lan_cfg.get(key) != orig_lan.get(key)
            for key in ['lora_role', 'lora_node_id', 'lora_gateway_id',
                       'lora_num_slots', 'lora_my_slot', 'lora_slot_duration_ms']
        )

        # LoRa Crypto changed?
        lora_crypto_changed = (
            lan_cfg.get('lora_crypto_key_len') != orig_lan.get('lora_crypto_key_len')
        )

        # ========================================
        # Generate commands ONLY for changed groups
        # ========================================

        # 1. Internet type (if changed)
        if wan_cfg.get('internet_type') != orig_wan.get('internet_type'):
            commands.append(ConfigCommandBuilder.build_internet_type(wan_cfg['internet_type']))

        # 2. WiFi - ONLY if WiFi fields changed
        if wifi_changed and wan_cfg.get('wifi_ssid'):
            wifi = WiFiConfig(
                ssid=wan_cfg['wifi_ssid'],
                password=wan_cfg.get('wifi_password', ''),
                username=wan_cfg.get('wifi_username', ''),
                auth_mode=wan_cfg.get('wifi_auth_mode', 'PERSONAL')
            )
            commands.append(ConfigCommandBuilder.build_wifi_config(wifi))

        # 3. LTE - ONLY if LTE fields changed
        if lte_changed and wan_cfg.get('lte_apn'):
            lte = LTEConfig(
                comm_type=wan_cfg.get('lte_comm_type', 'UART'),
                apn=wan_cfg['lte_apn'],
                username=wan_cfg.get('lte_username', ''),
                password=wan_cfg.get('lte_password', ''),
                auto_reconnect=wan_cfg.get('lte_auto_reconnect', 'false') == 'true',
                reconnect_timeout_ms=int(wan_cfg.get('lte_reconnect_timeout_ms', 30000)),
                max_reconnect_attempts=int(wan_cfg.get('lte_max_reconnect_attempts', 0))
            )
            commands.append(ConfigCommandBuilder.build_lte_config(lte))

        # 4. MQTT - ONLY if MQTT fields changed
        if mqtt_changed and wan_cfg.get('mqtt_broker'):
            mqtt = MQTTConfig(
                broker_uri=wan_cfg['mqtt_broker'],
                device_token=wan_cfg.get('mqtt_device_token', ''),
                subscribe_topic=wan_cfg.get('mqtt_sub_topic', ''),
                publish_topic=wan_cfg.get('mqtt_pub_topic', ''),
                attribute_topic=wan_cfg.get('mqtt_attribute_topic', '')
            )
            commands.append(ConfigCommandBuilder.build_mqtt_config(mqtt))

        # 5. Server type (if changed)
        if wan_cfg.get('server_type') != orig_wan.get('server_type'):
            commands.append(ConfigCommandBuilder.build_server_type(wan_cfg['server_type']))

        # 6. CAN - ONLY if CAN fields changed
        if can_changed:
            can = CANConfig(
                baud_rate=int(lan_cfg.get('can_baud_rate', 500000)),
                mode=lan_cfg.get('can_mode', 'NORMAL')
            )
            commands.extend(ConfigCommandBuilder.build_can_config(can))

        # 7. LoRa E32 Modem - ONLY if changed (FIXED PARSING)
        if lora_e32_changed:
            try:
                modem_cfg = LoRaModemConfig(
                    head=self.parse_int_value(lan_cfg.get('lora_e32_header', 0x00)),
                    addh=self.parse_int_value(lan_cfg.get('lora_e32_addh', 0x00)),
                    addl=self.parse_int_value(lan_cfg.get('lora_e32_addl', 0x00)),
                    sped=self.parse_int_value(lan_cfg.get('lora_e32_sped', 0x00)),
                    chan=self.parse_int_value(lan_cfg.get('lora_e32_chan', 0x00)),
                    option=self.parse_int_value(lan_cfg.get('lora_e32_option', 0x00))
                )

                # Validate byte range (0-255)
                for field_name, field_value in [
                    ('head', modem_cfg.head), ('addh', modem_cfg.addh), ('addl', modem_cfg.addl),
                    ('sped', modem_cfg.sped), ('chan', modem_cfg.chan), ('option', modem_cfg.option)
                ]:
                    if not (0 <= field_value <= 255):
                        raise ValueError(f"LoRa E32 {field_name}={field_value} out of range (0-255)")

                commands.append(ConfigCommandBuilder.build_lora_modem_command(modem_cfg))
                self.log(f"LoRa E32: head={modem_cfg.head:02X} addh={modem_cfg.addh:02X} addl={modem_cfg.addl:02X} "
                        f"sped={modem_cfg.sped:02X} chan={modem_cfg.chan:02X} option={modem_cfg.option:02X}", 'INFO')

            except Exception as e:
                self.log(f"Failed to build LoRa E32 command: {e}", 'ERROR')

        # 8. LoRa TDMA Handler - ONLY if changed (FIXED PARSING)
        if lora_handler_changed:
            try:
                # Parse role (string to int)
                role_str = lan_cfg.get('lora_role', 'GATEWAY')
                role = 0 if role_str == 'GATEWAY' else 1

                handler_cfg = LoRaTDMAConfig(
                    role=role,
                    node_id=self.parse_int_value(lan_cfg.get('lora_node_id', 0x0001)),
                    gateway_id=self.parse_int_value(lan_cfg.get('lora_gateway_id', 0x0001)),
                    num_slots=self.parse_int_value(lan_cfg.get('lora_num_slots', 8)),
                    my_slot=self.parse_int_value(lan_cfg.get('lora_my_slot', 0)),
                    slot_duration_ms=self.parse_int_value(lan_cfg.get('lora_slot_duration_ms', 200))
                )

                commands.append(ConfigCommandBuilder.build_lora_tdma_command(handler_cfg))
                self.log(f"LoRa TDMA: role={role} node_id=0x{handler_cfg.node_id:04X} "
                        f"gateway_id=0x{handler_cfg.gateway_id:04X} slots={handler_cfg.num_slots} "
                        f"my_slot={handler_cfg.my_slot} duration={handler_cfg.slot_duration_ms}ms", 'INFO')

            except Exception as e:
                self.log(f"Failed to build LoRa TDMA command: {e}", 'ERROR')

        # 9. LoRa Crypto - ONLY if changed
        if lora_crypto_changed:
            try:
                key_len = self.parse_int_value(lan_cfg.get('lora_crypto_key_len', 0))
                if key_len > 0:
                    # Generate dummy key (in real app, should get from secure storage)
                    key = bytes([0xFF] * key_len)
                    commands.append(ConfigCommandBuilder.build_lora_crypto_command(key))
                    self.log(f"LoRa Crypto: key_len={key_len}", 'INFO')
            except Exception as e:
                self.log(f"Failed to build LoRa crypto command: {e}", 'ERROR')
                
        # 10. Stack Type Configuration
        if lan_cfg.get('stack_1_type') != orig_lan.get('stack_1_type'):
            try:
                stack_type = lan_cfg.get('stack_1_type', 'NONE').upper()
                if StackTypeConfig.validate_stack_type(stack_type):
                    cmd = ConfigCommandBuilder.build_stack_type(0, stack_type)
                    commands.append(cmd)
                    self.log(f"Stack 1 type: {stack_type}", 'INFO')
                else:
                    self.log(f"Invalid Stack 1 type: {stack_type}", 'WARN')
            except Exception as e:
                self.log(f"Failed to build Stack 1 type command: {e}", 'ERROR')
        
        # Stack 2 type changed?
        if lan_cfg.get('stack_2_type') != orig_lan.get('stack_2_type'):
            try:
                stack_type = lan_cfg.get('stack_2_type', 'NONE').upper()
                if StackTypeConfig.validate_stack_type(stack_type):
                    cmd = ConfigCommandBuilder.build_stack_type(1, stack_type)
                    commands.append(cmd)
                    self.log(f"Stack 2 type: {stack_type}", 'INFO')
                else:
                    self.log(f"Invalid Stack 2 type: {stack_type}", 'WARN')
            except Exception as e:
                self.log(f"Failed to build Stack 2 type command: {e}", 'ERROR')
                
         # 11. RS485 Baud Rate Configuration
        if lan_cfg.get('rs485_baud_rate') != orig_lan.get('rs485_baud_rate'):
            try:
                baud_rate = int(lan_cfg.get('rs485_baud_rate', 115200))
                # Validate baud rate (RS485 max 115200)
                valid_bauds = [9600, 19200, 38400, 57600, 115200]
                if baud_rate not in valid_bauds:
                    self.log(f"Invalid RS485 baud rate: {baud_rate} (valid: 9600-115200)", 'WARN')
                else:
                    cmd = f"CFML:CFRS:BR:{baud_rate}"
                    commands.append(cmd)
                    self.log(f"RS485 baud rate: {baud_rate}", 'INFO')
            except Exception as e:
                self.log(f"Failed to build RS485 baud command: {e}", 'ERROR')

        return commands

    def save_to_file(self):
        """Save to JSON file"""
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
                # Include whitelist in save
                save_data = deepcopy(self.config_data)
                if self.can_whitelist_ids:
                    if 'LAN_CONFIG' not in save_data:
                        save_data['LAN_CONFIG'] = {}
                    save_data['LAN_CONFIG']['can_whitelist_ids'] = self.can_whitelist_ids

                with open(filename, 'w', encoding='utf-8') as f:
                    json.dump(save_data, f, indent=2, ensure_ascii=False)

                self.log(f"Configuration saved to {filename}", 'SUCCESS')
                messagebox.showinfo("Success", "Configuration saved")
            except Exception as e:
                self.log(f"Save failed: {e}", 'ERROR')
                messagebox.showerror("Error", str(e))

    def load_from_file(self):
        """Load from JSON file"""
        filename = filedialog.askopenfilename(
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )

        if filename:
            try:
                with open(filename, 'r', encoding='utf-8') as f:
                    loaded_config = json.load(f)

                # Merge
                for section, params in loaded_config.items():
                    if section not in self.config_data:
                        self.config_data[section] = {}
                    self.config_data[section].update(params)

                # Load whitelist if present
                if 'can_whitelist_ids' in loaded_config.get('LAN_CONFIG', {}):
                    self.can_whitelist_ids = loaded_config['LAN_CONFIG']['can_whitelist_ids']
                    self.update_whitelist_display()

                self.config_modified = True
                self.display_config()
                self.save_btn.config(state='normal')
                self.update_changes_status()
                self.log(f"Configuration loaded from {filename}", 'SUCCESS')
                messagebox.showinfo("Success", "Configuration loaded")
            except Exception as e:
                self.log(f"Load failed: {e}", 'ERROR')
                messagebox.showerror("Error", str(e))

    # UPDATED: Firmware update - always update both WAN and LAN
    def update_firmware(self):
        """Update firmware for both WAN and LAN (no target selection)"""
        flash_script = self.app_path / "flash_WAN.bat"
        if not flash_script.exists():
            error_msg = (
                f"flash_WAN.bat not found!\n\n"
                f"Expected: {flash_script}\n\n"
                f"Place flash_WAN.bat and .bin files in:\n{self.app_path}"
            )
            messagebox.showerror("Error", error_msg)
            return

        # Disconnect if connected
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

        # Confirm firmware update
        if not messagebox.askyesno("Confirm Firmware Update", 
                                   f"Update firmware for both WAN and LAN MCU?\n\nPort: {port}"):
            return

        # Start firmware update (no target parameter = both)
        self.run_flash_script(port, target="")

    def run_flash_script(self, port, target):
        """Run flash script with logging"""
        target_name = target if target else 'BOTH WAN and LAN'
        self.log(f"Starting firmware update: {target_name}", 'INFO')

        def flash_thread():
            try:
                flash_script = self.app_path / "flash_WAN.bat"
                cmd = f'"{flash_script}" {port}'
                if target:
                    cmd += f' {target}'

                self.log("="*70, 'INFO')
                self.log(f"Command: {cmd}", 'DEBUG')

                process = subprocess.Popen(
                    cmd,
                    shell=True,
                    cwd=str(self.app_path),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding='utf-8',
                    errors='ignore',
                    bufsize=1
                )

                for line in iter(process.stdout.readline, ''):
                    if line:
                        line = line.rstrip()
                        if 'ERROR' in line.upper() or 'FAILED' in line.upper():
                            self.log(line, 'ERROR')
                        elif 'SUCCESS' in line.upper() or ' OK' in line:
                            self.log(line, 'SUCCESS')
                        else:
                            self.log(line, 'DEBUG')

                return_code = process.wait()
                self.log("="*70, 'INFO')

                if return_code == 0:
                    self.log("Firmware update completed!", 'SUCCESS')
                    self.root.after(0, lambda: messagebox.showinfo(
                        "Success", "Firmware updated for both WAN and LAN!\n\nPlease reset the device."))
                else:
                    self.log(f"Flash failed (code {return_code})", 'ERROR')
                    self.root.after(0, lambda: messagebox.showerror(
                        "Error", "Firmware update failed\n\nCheck console log."))

            except Exception as e:
                self.log(f"Flash exception: {e}", 'ERROR')
                self.root.after(0, lambda: messagebox.showerror("Error", str(e)))

        threading.Thread(target=flash_thread, daemon=True).start()

    def clear_config(self):
        """Clear all data"""
        if self.config_modified or self.get_changed_count() > 0:
            if not messagebox.askyesno("Confirm", "Clear all data? Unsaved changes will be lost."):
                return

        self.config_data = {}
        self.original_config = {}
        self.config_modified = False
        self.can_whitelist_ids = []
        self.update_whitelist_display()
        self.raw_text.delete('1.0', tk.END)
        self.tree.delete(*self.tree.get_children())
        self.save_btn.config(state='disabled')
        self.write_btn.config(state='disabled')
        self.update_changes_status()
        self.log("Configuration cleared")

def main():
    root = tk.Tk()
    root.option_add("*Font", ('Segoe UI', 9))
    app = GatewayConfigTool(root)

    def on_closing():
        if app.serial_port and app.serial_port.is_open:
            app.disconnect()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_closing)
    root.mainloop()

if __name__ == '__main__':
    main()
