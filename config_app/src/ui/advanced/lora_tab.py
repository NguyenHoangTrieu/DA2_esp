"""
LoRa Configuration Tab - Raw byte input version

Commands are sent with CFML prefix (MCU LAN):
- CFML:CFLR:MODEM:<6 raw bytes>
- CFML:CFLR:HDLCF:<11 raw bytes>  
- CFML:CFLR:CRYPT:<1+N raw bytes>
"""

import tkinter as tk
from tkinter import ttk, messagebox

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(__file__)))))

from src.config.protocol import LoraConfig


class LoraTab(ttk.Frame):
    """LoRa configuration tab with raw byte input"""
    
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def _create_widgets(self):
        """Create LoRa tab widgets - compact layout"""
        # Container - minimal padding
        container = ttk.Frame(self, padding=5)
        container.pack(fill=tk.X, anchor="nw")
        
        # ═══════════════════════════════════════════════════════════════════
        # E32 Modem Settings Section - 6 bytes: HEAD ADDH ADDL SPED CHAN OPTION
        # ═══════════════════════════════════════════════════════════════════
        e32_section = ttk.LabelFrame(container, text="E32 Modem Settings (6 bytes)", padding=5)
        e32_section.pack(fill=tk.X, pady=3)
        
        # Row 1: HEAD, ADDH, ADDL
        row1 = ttk.Frame(e32_section)
        row1.pack(fill=tk.X, pady=1)
        
        ttk.Label(row1, text="HEAD:", width=8).pack(side=tk.LEFT)
        self.head_var = tk.StringVar(value="C0")
        ttk.Entry(row1, textvariable=self.head_var, width=5, font=("Consolas", 9)).pack(side=tk.LEFT, padx=2)
        
        ttk.Label(row1, text="ADDH:", width=8).pack(side=tk.LEFT, padx=(10, 0))
        self.addh_var = tk.StringVar(value="00")
        ttk.Entry(row1, textvariable=self.addh_var, width=5, font=("Consolas", 9)).pack(side=tk.LEFT, padx=2)
        
        ttk.Label(row1, text="ADDL:", width=8).pack(side=tk.LEFT, padx=(10, 0))
        self.addl_var = tk.StringVar(value="00")
        ttk.Entry(row1, textvariable=self.addl_var, width=5, font=("Consolas", 9)).pack(side=tk.LEFT, padx=2)
        
        # Row 2: SPED, CHAN, OPTION
        row2 = ttk.Frame(e32_section)
        row2.pack(fill=tk.X, pady=1)
        
        ttk.Label(row2, text="SPED:", width=8).pack(side=tk.LEFT)
        self.sped_var = tk.StringVar(value="1A")
        ttk.Entry(row2, textvariable=self.sped_var, width=5, font=("Consolas", 9)).pack(side=tk.LEFT, padx=2)
        
        ttk.Label(row2, text="CHAN:", width=8).pack(side=tk.LEFT, padx=(10, 0))
        self.chan_var = tk.StringVar(value="17")
        ttk.Entry(row2, textvariable=self.chan_var, width=5, font=("Consolas", 9)).pack(side=tk.LEFT, padx=2)
        
        ttk.Label(row2, text="OPTION:", width=8).pack(side=tk.LEFT, padx=(10, 0))
        self.option_var = tk.StringVar(value="44")
        ttk.Entry(row2, textvariable=self.option_var, width=5, font=("Consolas", 9)).pack(side=tk.LEFT, padx=2)
        
        # Hint label
        ttk.Label(e32_section, text="(All values in hex, 1 byte each)", 
                 foreground="#757575", font=("Segoe UI", 8)).pack(anchor="w")
        
        # E32 Set Button
        ttk.Button(e32_section, text="✅ Set E32 Modem", style='Set.TButton',
                  command=self._set_e32_config).pack(anchor="e", pady=3)
        
        # ═══════════════════════════════════════════════════════════════════
        # TDMA Network Settings Section - 11 bytes
        # ═══════════════════════════════════════════════════════════════════
        tdma_section = ttk.LabelFrame(container, text="TDMA Network (11 bytes)", padding=5)
        tdma_section.pack(fill=tk.X, pady=3)
        
        # Use grid for better alignment
        tdma_grid = ttk.Frame(tdma_section)
        tdma_grid.pack(fill=tk.X, pady=2)
        
        # Configure column weights for consistent alignment
        tdma_grid.columnconfigure(0, weight=0, minsize=80)   # Left labels
        tdma_grid.columnconfigure(1, weight=0, minsize=60)   # Left entries
        tdma_grid.columnconfigure(2, weight=0, minsize=100)  # Left hints
        tdma_grid.columnconfigure(3, weight=0, minsize=80)   # Right labels
        tdma_grid.columnconfigure(4, weight=0, minsize=70)   # Right entries
        tdma_grid.columnconfigure(5, weight=0)               # Right hints
        
        # Row 0: ROLE, NODE_ID
        ttk.Label(tdma_grid, text="ROLE:", anchor="e").grid(row=0, column=0, sticky="e", padx=2, pady=2)
        self.role_var = tk.StringVar(value="00")
        ttk.Entry(tdma_grid, textvariable=self.role_var, width=6, font=("Consolas", 9)).grid(row=0, column=1, sticky="w", padx=2, pady=2)
        ttk.Label(tdma_grid, text="(00=GW, 01=Node)", foreground="#757575", 
                 font=("Segoe UI", 8)).grid(row=0, column=2, sticky="w", padx=2, pady=2)
        
        ttk.Label(tdma_grid, text="NODE_ID:", anchor="e").grid(row=0, column=3, sticky="e", padx=2, pady=2)
        self.node_id_var = tk.StringVar(value="0001")
        ttk.Entry(tdma_grid, textvariable=self.node_id_var, width=8, font=("Consolas", 9)).grid(row=0, column=4, sticky="w", padx=2, pady=2)
        ttk.Label(tdma_grid, text="(2 bytes hex)", foreground="#757575", 
                 font=("Segoe UI", 8)).grid(row=0, column=5, sticky="w", padx=2, pady=2)
        
        # Row 1: GW_ID, SLOTS
        ttk.Label(tdma_grid, text="GW_ID:", anchor="e").grid(row=1, column=0, sticky="e", padx=2, pady=2)
        self.gw_id_var = tk.StringVar(value="0001")
        ttk.Entry(tdma_grid, textvariable=self.gw_id_var, width=6, font=("Consolas", 9)).grid(row=1, column=1, sticky="w", padx=2, pady=2)
        
        ttk.Label(tdma_grid, text="SLOTS:", anchor="e").grid(row=1, column=3, sticky="e", padx=2, pady=2)
        self.slots_var = tk.StringVar(value="08")
        ttk.Entry(tdma_grid, textvariable=self.slots_var, width=8, font=("Consolas", 9)).grid(row=1, column=4, sticky="w", padx=2, pady=2)
        
        # Row 2: MY_SLOT, DURATION
        ttk.Label(tdma_grid, text="MY_SLOT:", anchor="e").grid(row=2, column=0, sticky="e", padx=2, pady=2)
        self.myslot_var = tk.StringVar(value="00")
        ttk.Entry(tdma_grid, textvariable=self.myslot_var, width=6, font=("Consolas", 9)).grid(row=2, column=1, sticky="w", padx=2, pady=2)
        
        ttk.Label(tdma_grid, text="DURATION:", anchor="e").grid(row=2, column=3, sticky="e", padx=2, pady=2)
        self.duration_var = tk.StringVar(value="200")
        ttk.Entry(tdma_grid, textvariable=self.duration_var, width=8, font=("Consolas", 9)).grid(row=2, column=4, sticky="w", padx=2, pady=2)
        ttk.Label(tdma_grid, text="ms", foreground="#757575", 
                 font=("Segoe UI", 8)).grid(row=2, column=5, sticky="w", padx=2, pady=2)
        
        # TDMA Set Button
        ttk.Button(tdma_section, text="✅ Set TDMA Network", style='Set.TButton',
                  command=self._set_tdma_config).pack(anchor="e", pady=3)
        
        # ═══════════════════════════════════════════════════════════════════
        # Encryption Section - just KEY (key length derived from key string)
        # ═══════════════════════════════════════════════════════════════════
        crypto_section = ttk.LabelFrame(container, text="Encryption", padding=5)
        crypto_section.pack(fill=tk.X, pady=3)
        
        # Single row: KEY
        row_c = ttk.Frame(crypto_section)
        row_c.pack(fill=tk.X, pady=2)
        
        ttk.Label(row_c, text="KEY:", width=8).pack(side=tk.LEFT)
        self.key_var = tk.StringVar()
        self.key_entry = ttk.Entry(row_c, textvariable=self.key_var, width=40, font=("Consolas", 9))
        self.key_entry.pack(side=tk.LEFT, padx=2)
        
        ttk.Label(crypto_section, text="(Empty=Off, hex string 2-64 chars = XOR key 1-32 bytes)", 
                 foreground="#757575", font=("Segoe UI", 8)).pack(anchor="w")
        
        # Encryption Set Button
        ttk.Button(crypto_section, text="✅ Set Encryption", style='Set.TButton',
                  command=self._set_crypto_config).pack(anchor="e", pady=3)
    
    def _parse_hex_byte(self, val: str) -> int:
        """Parse hex string to byte value (0-255)"""
        val = val.strip().replace("0x", "").replace("0X", "")
        byte_val = int(val, 16)
        if byte_val < 0 or byte_val > 255:
            raise ValueError(f"Value {val} out of byte range (00-FF)")
        return byte_val
    
    def _parse_hex_word(self, val: str) -> int:
        """Parse hex string to word value (0-65535)"""
        val = val.strip().replace("0x", "").replace("0X", "")
        word_val = int(val, 16)
        if word_val < 0 or word_val > 65535:
            raise ValueError(f"Value {val} out of word range (0000-FFFF)")
        return word_val
    
    def _parse_hex_dword(self, val: str) -> int:
        """Parse hex string to dword value (0-FFFFFFFF)"""
        val = val.strip().replace("0x", "").replace("0X", "")
        dword_val = int(val, 16)
        if dword_val < 0 or dword_val > 0xFFFFFFFF:
            raise ValueError(f"Value {val} out of dword range")
        return dword_val
    
    def _check_connection(self) -> bool:
        """Check if serial is connected"""
        if not self.serial_manager or not self.serial_manager.is_connected():
            messagebox.showwarning("Warning", "Not connected to gateway")
            return False
        return True
    
    def _send_raw_command(self, prefix: str, raw_bytes: bytes, description: str):
        """Send command with CFML prefix and raw binary bytes"""
        self.log(f"Sending: {description}", "INFO")
        
        # Build command: CFML:<prefix>:<raw bytes>
        cmd_prefix = f"CFML:{prefix}:".encode('ascii')
        full_cmd = cmd_prefix + raw_bytes
        
        self.log(f"→ CFML:{prefix}:{raw_bytes.hex().upper()}", "DEBUG")
        
        if self.serial_manager.send_raw(full_cmd):
            self.log(f"✓ {description} - Sent", "SUCCESS")
        else:
            self.log(f"✗ {description} - Send failed", "ERROR")
    
    def _set_e32_config(self):
        """Set E32 Modem configuration - sends 6 raw bytes"""
        if not self._check_connection():
            return
        
        try:
            # Parse all 6 bytes
            head = self._parse_hex_byte(self.head_var.get())
            addh = self._parse_hex_byte(self.addh_var.get())
            addl = self._parse_hex_byte(self.addl_var.get())
            sped = self._parse_hex_byte(self.sped_var.get())
            chan = self._parse_hex_byte(self.chan_var.get())
            option = self._parse_hex_byte(self.option_var.get())
            
            # Build 6-byte payload
            raw_bytes = bytes([head, addh, addl, sped, chan, option])
            
            self._send_raw_command("CFLR:MODEM", raw_bytes, "LoRa E32 Modem (6 bytes)")
            
        except Exception as e:
            self.log(f"✗ Invalid E32 values: {e}", "ERROR")
            messagebox.showerror("Error", f"Invalid E32 values: {e}")
    
    def _set_tdma_config(self):
        """Set TDMA Network configuration - sends 11 raw bytes"""
        if not self._check_connection():
            return
        
        try:
            # Parse values
            role = self._parse_hex_byte(self.role_var.get())
            node_id = self._parse_hex_word(self.node_id_var.get())
            gw_id = self._parse_hex_word(self.gw_id_var.get())
            slots = self._parse_hex_byte(self.slots_var.get())
            myslot = self._parse_hex_byte(self.myslot_var.get())
            duration = int(self.duration_var.get().strip())  # decimal ms
            
            # Build 11-byte payload
            raw_bytes = bytes([
                role,
                (node_id >> 8) & 0xFF, node_id & 0xFF,
                (gw_id >> 8) & 0xFF, gw_id & 0xFF,
                slots, myslot,
                (duration >> 24) & 0xFF, (duration >> 16) & 0xFF,
                (duration >> 8) & 0xFF, duration & 0xFF
            ])
            
            self._send_raw_command("CFLR:HDLCF", raw_bytes, "LoRa TDMA Network (11 bytes)")
            
        except Exception as e:
            self.log(f"✗ Invalid TDMA values: {e}", "ERROR")
            messagebox.showerror("Error", f"Invalid TDMA values: {e}")
    
    def _set_crypto_config(self):
        """Set Encryption configuration - sends 1+N raw bytes (XOR cipher)"""
        if not self._check_connection():
            return
        
        try:
            key_str = self.key_var.get().strip().replace(" ", "")
            
            if not key_str:
                # Disable encryption - just send key_len=0
                raw_bytes = bytes([0])
                self._send_raw_command("CFLR:CRYPT", raw_bytes, "LoRa XOR Encryption Disabled")
            else:
                # Derive key_len from key string length
                if len(key_str) % 2 != 0:
                    raise ValueError("Key must have even number of hex characters")
                
                key_len = len(key_str) // 2
                
                if key_len < 1 or key_len > 32:
                    raise ValueError("Key must be 2-64 hex chars (1-32 bytes)")
                
                key_bytes = bytes.fromhex(key_str)
                raw_bytes = bytes([key_len]) + key_bytes
                
                self._send_raw_command("CFLR:CRYPT", raw_bytes, f"LoRa XOR Key ({key_len} bytes)")
                
        except Exception as e:
            self.log(f"✗ Invalid encryption values: {e}", "ERROR")
            messagebox.showerror("Error", f"Invalid encryption values: {e}")
    
    def get_config(self) -> LoraConfig:
        """Get LoRa config from UI"""
        config = LoraConfig()
        
        try:
            config.e32_addh = self._parse_hex_byte(self.addh_var.get())
        except:
            config.e32_addh = 0
        
        try:
            config.e32_addl = self._parse_hex_byte(self.addl_var.get())
        except:
            config.e32_addl = 0
        
        try:
            config.e32_sped = self._parse_hex_byte(self.sped_var.get())
        except:
            config.e32_sped = 0x1A
        
        try:
            config.e32_chan = self._parse_hex_byte(self.chan_var.get())
        except:
            config.e32_chan = 0x17
        
        try:
            config.e32_option = self._parse_hex_byte(self.option_var.get())
        except:
            config.e32_option = 0x44
        
        try:
            role = self._parse_hex_byte(self.role_var.get())
            config.role = "GATEWAY" if role == 0 else "NODE"
        except:
            config.role = "GATEWAY"
        
        try:
            config.node_id = self._parse_hex_word(self.node_id_var.get())
        except:
            config.node_id = 1
        
        try:
            config.gateway_id = self._parse_hex_word(self.gw_id_var.get())
        except:
            config.gateway_id = 1
        
        try:
            config.num_slots = self._parse_hex_byte(self.slots_var.get())
        except:
            config.num_slots = 8
        
        try:
            config.my_slot = self._parse_hex_byte(self.myslot_var.get())
        except:
            config.my_slot = 0
        
        try:
            config.slot_duration_ms = int(self.duration_var.get().strip())
        except:
            config.slot_duration_ms = 200
        
        try:
            key_str = self.key_var.get().strip()
            config.crypto_key_len = len(key_str) // 2 if key_str else 0
        except:
            config.crypto_key_len = 0
        
        return config
    
    def set_config(self, config: LoraConfig):
        """Set LoRa config to UI"""
        self.head_var.set("C0")
        self.addh_var.set(f"{config.e32_addh:02X}")
        self.addl_var.set(f"{config.e32_addl:02X}")
        self.sped_var.set(f"{config.e32_sped:02X}")
        self.chan_var.set(f"{config.e32_chan:02X}")
        self.option_var.set(f"{config.e32_option:02X}")
        
        self.role_var.set("00" if config.role == "GATEWAY" else "01")
        self.node_id_var.set(f"{config.node_id:04X}")
        
        gw_id = getattr(config, 'gateway_id', config.node_id)
        self.gw_id_var.set(f"{gw_id:04X}")
        
        self.slots_var.set(f"{config.num_slots:02X}")
        self.myslot_var.set(f"{config.my_slot:02X}")
        self.duration_var.set(str(config.slot_duration_ms))
