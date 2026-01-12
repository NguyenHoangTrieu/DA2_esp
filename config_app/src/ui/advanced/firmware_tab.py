"""
Firmware Update Tab for ESP32 Gateway Configuration Tool
"""

import tkinter as tk
from tkinter import ttk, messagebox
import subprocess
import threading
from pathlib import Path
from typing import Callable, Optional, List, Tuple


class FirmwareTab(ttk.Frame):
    """Firmware update tab"""
    
    def __init__(self, parent, log_callback: Optional[Callable] = None, **kwargs):
        super().__init__(parent, **kwargs)
        
        self.log_callback = log_callback
        self.flashing = False
        self._create_widgets()
    
    def _create_widgets(self):
        """Create firmware tab widgets"""
        # Container - no expand to avoid whitespace
        container = ttk.Frame(self, padding=10)
        container.pack(fill=tk.X, anchor="nw")
        
        # Info section - compact
        info_section = ttk.LabelFrame(container, text="Information", padding=8)
        info_section.pack(fill=tk.X, pady=5)
        
        ttk.Label(info_section, text="Flash both WAN and LAN MCU firmware",
                 font=("Segoe UI", 9)).pack(anchor="w")
        ttk.Label(info_section, text="⚠️ Requires flash_WAN.sh (Linux/macOS) or flash_WAN.bat (Windows) in bin/",
             font=("Segoe UI", 9), foreground="#FF9800").pack(anchor="w")
        
        # COM Port selection - compact
        port_section = ttk.LabelFrame(container, text="COM Port", padding=8)
        port_section.pack(fill=tk.X, pady=5)
        
        port_frame = ttk.Frame(port_section)
        port_frame.pack(fill=tk.X, pady=2)
        
        ttk.Label(port_frame, text="Port:", width=8).pack(side=tk.LEFT)
        
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(port_frame, textvariable=self.port_var,
                                        width=12, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=5)
        
        self.refresh_btn = ttk.Button(port_frame, text="Refresh", width=8,
                                       command=self._refresh_ports)
        self.refresh_btn.pack(side=tk.LEFT, padx=5)
        
        # Update button
        btn_frame = ttk.Frame(container)
        btn_frame.pack(fill=tk.X, pady=10)
        
        self.update_btn = ttk.Button(btn_frame, text="Update Firmware", style='Set.TButton',
                                      command=self._on_update_click)
        self.update_btn.pack(anchor="e", padx=5)
        
        # Flash output log - smaller
        log_section = ttk.LabelFrame(container, text="Flash Log", padding=5)
        log_section.pack(fill=tk.X, pady=5)
        
        log_scroll = ttk.Scrollbar(log_section)
        log_scroll.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.log_text = tk.Text(log_section, height=6, wrap=tk.WORD,
                                font=("Consolas", 8),
                                yscrollcommand=log_scroll.set,
                                bg="#1E1E1E", fg="#CCCCCC")
        self.log_text.pack(fill=tk.X)
        log_scroll.config(command=self.log_text.yview)
        
        # Configure tags
        self.log_text.tag_configure('INFO', foreground='#2196F3')
        self.log_text.tag_configure('SUCCESS', foreground='#4CAF50')
        self.log_text.tag_configure('ERROR', foreground='#F44336')
        self.log_text.tag_configure('DEBUG', foreground='#888888')
    
    def _refresh_ports(self):
        """Refresh port list"""
        import serial.tools.list_ports
        ports = []
        for port in serial.tools.list_ports.comports():
            ports.append(f"{port.device} - {port.description}")
        
        self.port_combo['values'] = ports
        if ports:
            self.port_combo.current(0)
    
    def refresh_ports(self, ports: List[Tuple[str, str]]):
        """Refresh port list from external source - show only COM name"""
        port_list = [p[0] for p in ports]
        self.port_combo['values'] = port_list
        if port_list:
            self.port_combo.current(0)
    
    def _on_update_click(self):
        """Handle update button click"""
        if self.flashing:
            return
        
        port = self.port_var.get()
        if not port:
            messagebox.showerror("Error", "Please select a COM port")
            return
        
        # Confirm
        result = messagebox.askyesno("Confirm", 
            f"Update firmware on {port}?\n\n"
            "This will flash both WAN and LAN MCU.")
        
        if result:
            self._run_flash(port)
    
    def _run_flash(self, port: str):
        """Run flash process"""
        # Extract COM port name only (e.g., "COM47" from "COM47 - USB-Enhanced-SERIAL CH343 (COM47)")
        com_port = port.split(" - ")[0].strip() if " - " in port else port.strip()
        
        # Choose script by platform
        import sys
        is_windows = sys.platform.startswith("win")
        script_name = "flash_WAN.bat" if is_windows else "flash_WAN.sh"

        # Resolve base path (works for PyInstaller onedir/onefile and source)
        if getattr(sys, "frozen", False):
            base_path = Path(getattr(sys, "_MEIPASS", Path(sys.executable).parent))
        else:
            base_path = Path(__file__).resolve().parent.parent.parent.parent

        flash_script = base_path / "bin" / script_name
        
        if not flash_script.exists():
            self._log(f"{script_name} not found at {flash_script}", "ERROR")
            messagebox.showerror("Error", f"{script_name} not found!")
            return
        
        self.flashing = True
        self.update_btn.config(state=tk.DISABLED)
        self._log("=" * 60, "DEBUG")
        self._log(f"Starting firmware update: BOTH WAN and LAN", "INFO")

        script_dir = flash_script.parent

        if is_windows:
            cmd = f'"{flash_script}" {com_port}'
            shell = True
            display_cmd = cmd
        else:
            cmd = ["bash", str(flash_script), com_port]
            shell = False
            display_cmd = " ".join(cmd)

        self._log(f"Command: {display_cmd}", "DEBUG")

        # Run in thread
        thread = threading.Thread(target=self._flash_thread, args=(cmd, shell, script_dir))
        thread.daemon = True
        thread.start()
    
    def _flash_thread(self, cmd, shell: bool, cwd: Path):
        """Flash thread"""
        try:
            process = subprocess.Popen(
                cmd,
                shell=shell,
                cwd=str(cwd),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding='utf-8',
                errors='ignore',
                bufsize=1
            )
            
            # Read output realtime
            for line in iter(process.stdout.readline, ''):
                if line:
                    line = line.rstrip()
                    if 'ERROR' in line.upper() or 'FAILED' in line.upper():
                        self._log(line, 'ERROR')
                    elif 'SUCCESS' in line.upper() or ' OK' in line:
                        self._log(line, 'SUCCESS')
                    else:
                        self._log(line, 'DEBUG')
            
            return_code = process.wait()
            
            if return_code == 0:
                self._log("Firmware update completed!", 'SUCCESS')
                self.after(0, lambda: messagebox.showinfo("Success", 
                    "Firmware updated for both WAN and LAN!"))
            else:
                self._log(f"Flash failed (code {return_code})", 'ERROR')
                self.after(0, lambda: messagebox.showerror("Error", 
                    "Firmware update failed!"))
            
        except Exception as e:
            self._log(f"Flash error: {e}", 'ERROR')
            self.after(0, lambda: messagebox.showerror("Error", str(e)))
        
        finally:
            self.flashing = False
            self.after(0, lambda: self.update_btn.config(state=tk.NORMAL))
            self._log("=" * 60, "DEBUG")
    
    def _log(self, message: str, level: str = "INFO"):
        """Add log message"""
        def _insert():
            self.log_text.insert(tk.END, f"[{level}] {message}\n", level)
            self.log_text.see(tk.END)
        
        self.after(0, _insert)
        
        # Also call external log callback
        if self.log_callback:
            self.log_callback(message, level)
