"""
ESP32 Gateway Configuration Tool v4.0
Main Application Entry Point
"""

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import json
from pathlib import Path
from datetime import datetime
import threading

import sys
import os
sys.path.insert(0, os.path.dirname(__file__))

from src.serial.manager import SerialManager
from src.config.protocol import (
    GatewayConfig, ConfigParser, 
    CFSC_COMMAND, CFSC_RESP_START, CFSC_RESP_END
)
from src.ui.connection_bar import ConnectionBar
from src.ui.console_panel import ConsolePanel
from src.ui.uart_log_panel import UartLogPanel
from src.ui.basic.basic_panel import BasicPanel
from src.ui.advanced.advanced_panel import AdvancedPanel


class GatewayConfigApp:
    """Main application class"""
    
    VERSION = "4.0.0"
    WINDOW_TITLE = "🔌 ESP32 Gateway Configuration Tool"
    WINDOW_SIZE = "1300x800"
    
    def __init__(self):
        self.root = tk.Tk()
        self.root.title(self.WINDOW_TITLE)
        self.root.geometry(self.WINDOW_SIZE)
        self.root.minsize(1100, 700)
        
        # Initialize managers
        self.serial_manager = SerialManager(
            on_data_callback=self._on_serial_data,
            on_log_callback=self._log,
            on_tx_callback=self._on_serial_tx
        )
        
        # Current config
        self.current_config: GatewayConfig = GatewayConfig()
        self.raw_response: str = ""
        
        # Current mode (False = basic, True = advanced)
        self.advanced_mode = tk.BooleanVar(value=False)
        
        # Build UI
        self._create_styles()
        self._create_ui()
        
        # Initial port refresh
        self.root.after(100, self._refresh_ports)
    
    def _create_styles(self):
        """Create custom styles"""
        style = ttk.Style()
        
        # Try to use a modern theme
        available_themes = style.theme_names()
        if 'clam' in available_themes:
            style.theme_use('clam')
        
        # Custom button styles
        style.configure('Action.TButton', font=('Segoe UI', 10), padding=5)
        style.configure('Set.TButton', font=('Segoe UI', 10, 'bold'), padding=8)
        
        # Configure colors - match background for all widgets
        bg_color = '#F5F5F5'
        style.configure('TLabelframe', background=bg_color)
        style.configure('TLabelframe.Label', font=('Segoe UI', 10, 'bold'), background=bg_color)
        style.configure('TFrame', background=bg_color)
        style.configure('TLabel', background=bg_color)
        style.configure('TCheckbutton', background=bg_color)
        style.configure('TNotebook', background=bg_color)
        style.configure('TNotebook.Tab', padding=[10, 5])
    
    def _create_ui(self):
        """Create main UI with 3-Panel layout: Config | UART Log + Debug Log below"""
        # Main container
        main_frame = ttk.Frame(self.root)
        main_frame.pack(fill=tk.BOTH, expand=True)
        
        # Connection bar
        self.connection_bar = ConnectionBar(
            main_frame,
            on_connect=self._on_connect,
            on_disconnect=self._on_disconnect,
            on_refresh=self._get_ports
        )
        self.connection_bar.pack(fill=tk.X, padx=10, pady=5)
        
        # Separator
        ttk.Separator(main_frame, orient='horizontal').pack(fill=tk.X, padx=10)
        
        # Mode selector and action buttons row
        controls_frame = ttk.Frame(main_frame)
        controls_frame.pack(fill=tk.X, padx=10, pady=8)
        
        # Advanced Mode checkbox (left side)
        mode_frame = ttk.Frame(controls_frame)
        mode_frame.pack(side=tk.LEFT)
        
        self.advanced_check = ttk.Checkbutton(
            mode_frame, 
            text="Advanced Mode",
            variable=self.advanced_mode,
            command=self._on_mode_change
        )
        self.advanced_check.pack(side=tk.LEFT, padx=5)
        
        # Action buttons (right side)
        action_frame = ttk.Frame(controls_frame)
        action_frame.pack(side=tk.RIGHT)
        
        ttk.Button(action_frame, text="📖 Read Config",
                  style='Action.TButton',
                  command=self._read_config).pack(side=tk.LEFT, padx=3)
        
        ttk.Button(action_frame, text="💾 Save File",
                  style='Action.TButton',
                  command=self._save_to_file).pack(side=tk.LEFT, padx=3)
        
        ttk.Button(action_frame, text="📂 Load File",
                  style='Action.TButton',
                  command=self._load_from_file).pack(side=tk.LEFT, padx=3)
        
        # Separator
        ttk.Separator(main_frame, orient='horizontal').pack(fill=tk.X, padx=10)
        
        # ═══════════════════════════════════════════════════════════════════
        # 3-Panel Layout using PanedWindows
        # ═══════════════════════════════════════════════════════════════════
        
        # Vertical paned: (Config + UART Log) on top, Debug Log on bottom
        self.vertical_paned = ttk.PanedWindow(main_frame, orient=tk.VERTICAL)
        self.vertical_paned.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)
        
        # Top section: Horizontal paned (Config | UART Log)
        self.horizontal_paned = ttk.PanedWindow(self.vertical_paned, orient=tk.HORIZONTAL)
        
        # Left side - Configuration panel container (fixed width)
        self.config_container = ttk.Frame(self.horizontal_paned, width=550)
        self.config_container.pack_propagate(False)
        
        # Right side - UART Log panel (expandable)
        self.uart_log = UartLogPanel(self.horizontal_paned)
        
        # Add to horizontal paned
        self.horizontal_paned.add(self.config_container, weight=0)
        self.horizontal_paned.add(self.uart_log, weight=1)
        
        # Bottom section: Debug Log (full width, fixed height)
        self.debug_log = ConsolePanel(self.vertical_paned, height=120)
        
        # Add to vertical paned
        self.vertical_paned.add(self.horizontal_paned, weight=1)
        self.vertical_paned.add(self.debug_log, weight=0)
        
        # Create config panels
        self.basic_panel = BasicPanel(
            self.config_container, 
            serial_manager=self.serial_manager,
            log_callback=self._log
        )
        self.advanced_panel = AdvancedPanel(
            self.config_container,
            serial_manager=self.serial_manager,
            log_callback=self._log
        )
        
        # Show basic panel by default (anchor nw to avoid whitespace)
        self.basic_panel.pack(fill=tk.X, anchor="nw")
        
        # Status bar
        status_frame = ttk.Frame(main_frame)
        status_frame.pack(fill=tk.X, padx=10, pady=5)
        
        self.status_label = ttk.Label(status_frame, text="Ready")
        self.status_label.pack(side=tk.LEFT)
        
        version_label = ttk.Label(status_frame, 
                                  text=f"v{self.VERSION} | © 2024",
                                  foreground="#757575")
        version_label.pack(side=tk.RIGHT)
        
        # Welcome log
        self._log("ESP32 Gateway Configuration Tool started", "INFO")
        self._log("Select a COM port and click Connect to begin", "INFO")
    
    def _on_mode_change(self):
        """Handle mode checkbox change"""
        # Hide all panels
        self.basic_panel.pack_forget()
        self.advanced_panel.pack_forget()
        
        # Show selected panel (anchor nw to avoid whitespace)
        if self.advanced_mode.get():
            self.advanced_panel.pack(fill=tk.X, anchor="nw")
            self._log("Switched to ADVANCED mode", "INFO")
        else:
            self.basic_panel.pack(fill=tk.X, anchor="nw")
            self._log("Switched to BASIC mode", "INFO")
    
    def _get_ports(self):
        """Get available serial ports"""
        return self.serial_manager.list_ports()
    
    def _refresh_ports(self):
        """Refresh port list"""
        ports = self._get_ports()
        self.connection_bar._on_refresh()
        self.advanced_panel.refresh_ports(ports)
    
    def _on_connect(self, port: str, baudrate: int):
        """Handle connect request"""
        if self.serial_manager.connect(port, baudrate):
            self.connection_bar.set_connected(True, port)
            self.uart_log.set_port(port)  # Update UART log header
            self._set_status(f"Connected to {port}")
    
    def _on_disconnect(self):
        """Handle disconnect request"""
        self.serial_manager.disconnect()
        self.connection_bar.set_connected(False)
        self.uart_log.set_disconnected()  # Update UART log header
        self._set_status("Disconnected")
    
    def _on_serial_data(self, data: str):
        """Handle incoming serial data - display in UART Log"""
        # Log to UART panel (raw data)
        if hasattr(self, 'uart_log'):
            self.uart_log.log_rx(data)
    
    def _on_serial_tx(self, data: str):
        """Handle outgoing serial data - display in UART Log"""
        if hasattr(self, 'uart_log'):
            self.uart_log.log_tx(data)
    
    def _log(self, message: str, level: str = "INFO"):
        """Log to debug console"""
        self.debug_log.log(message, level)
    
    def _set_status(self, message: str):
        """Set status bar message"""
        self.status_label.config(text=message)
    
    def _read_config(self):
        """Read config from gateway"""
        if not self.serial_manager.is_connected():
            messagebox.showwarning("Warning", "Not connected to gateway")
            return
        
        self._log("Reading configuration from gateway...", "INFO")
        self._set_status("Reading config...")
        
        # Send CFSC command
        def read_thread():
            response = self.serial_manager.send_command(CFSC_COMMAND, timeout=5.0)
            
            if response:
                self.raw_response = response
                config = ConfigParser.parse_response(response)
                
                if config:
                    self.current_config = config
                    self.root.after(0, self._update_ui_from_config)
                    self.root.after(0, lambda: self._log("Configuration read successfully", "SUCCESS"))
                    self.root.after(0, lambda: self._set_status("Config loaded"))
                else:
                    self.root.after(0, lambda: self._log("Failed to parse config response", "ERROR"))
                    self.root.after(0, lambda: self._set_status("Parse error"))
            else:
                self.root.after(0, lambda: self._log("No response from gateway", "ERROR"))
                self.root.after(0, lambda: self._set_status("Read failed"))
        
        thread = threading.Thread(target=read_thread)
        thread.daemon = True
        thread.start()
    
    def _update_ui_from_config(self):
        """Update UI panels from current config"""
        self.basic_panel.set_config(self.current_config)
        self.advanced_panel.set_config(self.current_config)
    
    def _save_to_file(self):
        """Save config to file"""
        filename = filedialog.asksaveasfilename(
            defaultextension=".json",
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")],
            initialfile=f"gateway_config_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json"
        )
        
        if not filename:
            return
        
        # Convert to dict
        config_dict = {
            "wan": {
                "internet_type": self.current_config.wan.internet_type,
                "wifi_ssid": self.current_config.wan.wifi_ssid,
                "lte_apn": self.current_config.wan.lte_apn,
                "mqtt_broker": self.current_config.wan.mqtt_broker,
            },
            "lan": {
                "stack_1_type": self.current_config.lan.stack.stack_1_type,
                "stack_2_type": self.current_config.lan.stack.stack_2_type,
            }
        }
        
        try:
            with open(filename, 'w', encoding='utf-8') as f:
                json.dump(config_dict, f, indent=2)
            
            self._log(f"Config saved to {filename}", "SUCCESS")
            messagebox.showinfo("Success", f"Configuration saved to:\n{filename}")
        except Exception as e:
            self._log(f"Save failed: {e}", "ERROR")
            messagebox.showerror("Error", f"Failed to save: {e}")
    
    def _load_from_file(self):
        """Load config from file"""
        filename = filedialog.askopenfilename(
            filetypes=[("JSON files", "*.json"), ("All files", "*.*")]
        )
        
        if not filename:
            return
        
        try:
            with open(filename, 'r', encoding='utf-8') as f:
                config_dict = json.load(f)
            
            # Update current config
            if "wan" in config_dict:
                wan = config_dict["wan"]
                self.current_config.wan.internet_type = wan.get("internet_type", "WIFI")
                self.current_config.wan.wifi_ssid = wan.get("wifi_ssid", "")
                self.current_config.wan.lte_apn = wan.get("lte_apn", "internet")
                self.current_config.wan.mqtt_broker = wan.get("mqtt_broker", "mqtt.thingsboard.cloud")
            
            if "lan" in config_dict:
                lan = config_dict["lan"]
                self.current_config.lan.stack.stack_1_type = lan.get("stack_1_type", "NONE")
                self.current_config.lan.stack.stack_2_type = lan.get("stack_2_type", "NONE")
            
            self._update_ui_from_config()
            self._log(f"Config loaded from {filename}", "SUCCESS")
            messagebox.showinfo("Success", "Configuration loaded successfully")
            
        except Exception as e:
            self._log(f"Load failed: {e}", "ERROR")
            messagebox.showerror("Error", f"Failed to load: {e}")
    
    def run(self):
        """Run the application"""
        self.root.mainloop()
        # Cleanup on exit
        self.serial_manager.disconnect()


def main():
    """Main entry point"""
    app = GatewayConfigApp()
    app.run()


if __name__ == "__main__":
    main()
