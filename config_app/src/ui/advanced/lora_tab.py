"""
LoRa Configuration Tab
"""

import tkinter as tk
from tkinter import ttk, messagebox
import threading

import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(__file__)))))

from src.config.protocol import LoraConfig
from src.config.validators import format_hex_byte, format_hex_word, parse_hex


class LoraTab(ttk.Frame):
    """LoRa configuration tab"""
    
    def __init__(self, parent, serial_manager=None, log_callback=None, **kwargs):
        super().__init__(parent, **kwargs)
        self.serial_manager = serial_manager
        self.log = log_callback or (lambda msg, lvl: None)
        self._create_widgets()
    
    def _create_widgets(self):
        """Create LoRa tab widgets"""
        # Container - no expand to avoid whitespace
        container = ttk.Frame(self, padding=10)
        container.pack(fill=tk.X, anchor="nw")
        
        # E32 Modem Settings - compact
        e32_section = ttk.LabelFrame(container, text="E32 Modem Settings", padding=8)
        e32_section.pack(fill=tk.X, pady=5)
        
        # Address row
        addr_frame = ttk.Frame(e32_section)
        addr_frame.pack(pady=2, anchor="w")
        
        ttk.Label(addr_frame, text="ADDH:", width=8).pack(side=tk.LEFT)
        self.addh_var = tk.StringVar(value="0x00")
        ttk.Entry(addr_frame, textvariable=self.addh_var, width=6).pack(side=tk.LEFT, padx=3)
        
        ttk.Label(addr_frame, text="ADDL:").pack(side=tk.LEFT, padx=(10, 0))
        self.addl_var = tk.StringVar(value="0x00")
        ttk.Entry(addr_frame, textvariable=self.addl_var, width=6).pack(side=tk.LEFT, padx=3)
        
        ttk.Label(addr_frame, text="Channel:").pack(side=tk.LEFT, padx=(10, 0))
        self.chan_var = tk.StringVar(value="23")
        ttk.Spinbox(addr_frame, textvariable=self.chan_var, from_=0, to=31, width=5).pack(side=tk.LEFT, padx=3)
        
        # TDMA Network Settings - compact
        tdma_section = ttk.LabelFrame(container, text="TDMA Network", padding=8)
        tdma_section.pack(fill=tk.X, pady=5)
        
        # Row 1: Role and IDs
        row1 = ttk.Frame(tdma_section)
        row1.pack(pady=2, anchor="w")
        
        ttk.Label(row1, text="Role:", width=8).pack(side=tk.LEFT)
        self.role_var = tk.StringVar(value="GATEWAY")
        ttk.Radiobutton(row1, text="Gateway", variable=self.role_var,
                       value="GATEWAY").pack(side=tk.LEFT, padx=3)
        ttk.Radiobutton(row1, text="Node", variable=self.role_var,
                       value="NODE").pack(side=tk.LEFT, padx=3)
        
        ttk.Label(row1, text="Node ID:").pack(side=tk.LEFT, padx=(10, 0))
        self.node_id_var = tk.StringVar(value="0x0001")
        ttk.Entry(row1, textvariable=self.node_id_var, width=8).pack(side=tk.LEFT, padx=3)
        
        # Row 2: Slots
        row2 = ttk.Frame(tdma_section)
        row2.pack(pady=2, anchor="w")
        
        ttk.Label(row2, text="Slots:", width=8).pack(side=tk.LEFT)
        self.num_slots_var = tk.StringVar(value="8")
        ttk.Spinbox(row2, textvariable=self.num_slots_var, from_=1, to=255, width=5).pack(side=tk.LEFT, padx=3)
        
        ttk.Label(row2, text="My Slot:").pack(side=tk.LEFT, padx=(10, 0))
        self.my_slot_var = tk.StringVar(value="0")
        ttk.Spinbox(row2, textvariable=self.my_slot_var, from_=0, to=254, width=5).pack(side=tk.LEFT, padx=3)
        
        ttk.Label(row2, text="Duration(ms):").pack(side=tk.LEFT, padx=(10, 0))
        self.duration_var = tk.StringVar(value="200")
        ttk.Entry(row2, textvariable=self.duration_var, width=6).pack(side=tk.LEFT, padx=3)
        
        # Encryption - compact
        crypto_section = ttk.LabelFrame(container, text="Encryption", padding=8)
        crypto_section.pack(fill=tk.X, pady=5)
        
        crypto_frame = ttk.Frame(crypto_section)
        crypto_frame.pack(pady=2, anchor="w")
        
        ttk.Label(crypto_frame, text="Key Length:", width=10).pack(side=tk.LEFT)
        self.crypto_var = tk.StringVar(value="0")
        combo = ttk.Combobox(crypto_frame, textvariable=self.crypto_var,
                            values=["0 (Disabled)", "16 (AES-128)", "32 (AES-256)"],
                            state="readonly", width=18)
        combo.pack(side=tk.LEFT, padx=5)
        
        # Set Button
        btn_frame = ttk.Frame(container)
        btn_frame.pack(fill=tk.X, pady=10)
        ttk.Button(btn_frame, text="Set LoRa Config", style='Set.TButton',
                  command=self._set_lora_config).pack(anchor="e", padx=5)
    
    def _check_connection(self) -> bool:
        """Check if serial is connected"""
        if not self.serial_manager or not self.serial_manager.is_connected():
            messagebox.showwarning("Warning", "Not connected to gateway")
            return False
        return True
    
    def _send_command(self, cmd: str, description: str):
        """Send command without waiting for response"""
        self.log(f"Sending: {description}", "INFO")
        if self.serial_manager.send(cmd):
            self.log(f"{description} - Sent", "SUCCESS")
        else:
            self.log(f"{description} - Send failed", "ERROR")
    
    def _set_lora_config(self):
        """Set LoRa configuration"""
        if not self._check_connection():
            return
        
        # Parse values
        try:
            addh = parse_hex(self.addh_var.get().split()[0])
            addl = parse_hex(self.addl_var.get().split()[0])
            chan = int(self.chan_var.get())
            sped = 0x1A  # Default 9600bps
            option = 0x44  # Default fixed, push-pull
            header = 0xC0
            
            # CFLR:MODEM:<6 bytes hex> = HEAD ADDH ADDL SPED CHAN OPTION
            modem_bytes = f"{header:02X}{addh:02X}{addl:02X}{sped:02X}{chan:02X}{option:02X}"
            self._send_command(f"CFLR:MODEM:{modem_bytes}", "LoRa Modem Config")
        except Exception as e:
            self.log(f"Invalid modem values: {e}", "ERROR")
            return
        
        # TDMA/HDLC config
        try:
            role = 0 if self.role_var.get() == "GATEWAY" else 1
            node_id = parse_hex(self.node_id_var.get().split()[0])
            gateway_id = node_id if role == 0 else 1
            num_slots = int(self.num_slots_var.get())
            my_slot = int(self.my_slot_var.get())
            duration = int(self.duration_var.get())
            
            # CFLR:HDLCF:<11 bytes hex> = ROLE NODE_ID_H NODE_ID_L GW_ID_H GW_ID_L 
            #                              NUM_SLOTS MY_SLOT DUR_H DUR_M DUR_L (padding)
            hdlc_bytes = f"{role:02X}{(node_id>>8)&0xFF:02X}{node_id&0xFF:02X}"
            hdlc_bytes += f"{(gateway_id>>8)&0xFF:02X}{gateway_id&0xFF:02X}"
            hdlc_bytes += f"{num_slots:02X}{my_slot:02X}"
            hdlc_bytes += f"{(duration>>8)&0xFF:02X}{duration&0xFF:02X}0000"
            
            self._send_command(f"CFLR:HDLCF:{hdlc_bytes}", "LoRa TDMA Config")
        except Exception as e:
            self.log(f"Invalid TDMA values: {e}", "ERROR")
            return
        
        # Encryption
        try:
            crypto_val = self.crypto_var.get()
            key_len = int(crypto_val.split()[0])
            if key_len > 0:
                # CFLR:CRYPT:<len><key>
                key = "00" * key_len  # Placeholder key
                self._send_command(f"CFLR:CRYPT:{key_len:02X}{key}", "LoRa Encryption")
        except:
            pass
    
    def get_config(self) -> LoraConfig:
        """Get LoRa config from UI"""
        config = LoraConfig()
        
        try:
            config.e32_addh = parse_hex(self.addh_var.get().split()[0])
        except:
            config.e32_addh = 0
        
        try:
            config.e32_addl = parse_hex(self.addl_var.get().split()[0])
        except:
            config.e32_addl = 0
        
        try:
            config.e32_chan = int(self.chan_var.get())
        except:
            config.e32_chan = 23
        
        config.role = self.role_var.get()
        
        try:
            config.node_id = parse_hex(self.node_id_var.get().split()[0])
        except:
            config.node_id = 1
        
        try:
            config.num_slots = int(self.num_slots_var.get())
        except:
            config.num_slots = 8
        
        try:
            config.my_slot = int(self.my_slot_var.get())
        except:
            config.my_slot = 0
        
        try:
            config.slot_duration_ms = int(self.duration_var.get())
        except:
            config.slot_duration_ms = 200
        
        try:
            crypto = self.crypto_var.get()
            config.crypto_key_len = int(crypto.split()[0])
        except:
            config.crypto_key_len = 0
        
        return config
    
    def set_config(self, config: LoraConfig):
        """Set LoRa config to UI"""
        self.addh_var.set(format_hex_byte(config.e32_addh))
        self.addl_var.set(format_hex_byte(config.e32_addl))
        self.chan_var.set(str(config.e32_chan))
        self.role_var.set(config.role)
        self.node_id_var.set(format_hex_word(config.node_id))
        self.num_slots_var.set(str(config.num_slots))
        self.my_slot_var.set(str(config.my_slot))
        self.duration_var.set(str(config.slot_duration_ms))
        
        crypto_text = {0: "0 (Disabled)", 16: "16 (AES-128)", 32: "32 (AES-256)"}
        self.crypto_var.set(crypto_text.get(config.crypto_key_len, "0 (Disabled)"))
