"""
Serial Manager for ESP32 Gateway Configuration Tool
Handles serial port communication
"""

import serial
import serial.tools.list_ports
import threading
import queue
import time
from typing import Optional, Callable, List, Tuple


class SerialManager:
    """Manages serial port connection and communication"""
    
    def __init__(self, on_data_callback: Optional[Callable[[str], None]] = None,
                 on_log_callback: Optional[Callable[[str, str], None]] = None,
                 on_tx_callback: Optional[Callable[[str], None]] = None):
        self.serial_port: Optional[serial.Serial] = None
        self.read_thread: Optional[threading.Thread] = None
        self.running = False
        self.on_data_callback = on_data_callback
        self.on_log_callback = on_log_callback
        self.on_tx_callback = on_tx_callback  # Callback for TX data (for UART log)
        self.response_buffer = ""
        self.response_queue = queue.Queue()
        
    def log(self, message: str, level: str = "INFO"):
        """Log a message"""
        if self.on_log_callback:
            self.on_log_callback(message, level)
    
    @staticmethod
    def list_ports() -> List[Tuple[str, str]]:
        """List available serial ports"""
        ports = []
        for port in serial.tools.list_ports.comports():
            ports.append((port.device, port.description))
        return ports
    
    def connect(self, port: str, baudrate: int = 115200, timeout: float = 1.0) -> bool:
        """Connect to serial port"""
        try:
            if self.is_connected():
                self.disconnect()
            
            self.serial_port = serial.Serial(
                port=port,
                baudrate=baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=timeout
            )
            
            self.running = True
            self.read_thread = threading.Thread(target=self._read_loop, daemon=True)
            self.read_thread.start()
            
            self.log(f"Connected to {port} at {baudrate} baud", "SUCCESS")
            return True
            
        except serial.SerialException as e:
            self.log(f"Connection failed: {e}", "ERROR")
            return False
    
    def disconnect(self):
        """Disconnect from serial port"""
        self.running = False
        
        if self.read_thread and self.read_thread.is_alive():
            self.read_thread.join(timeout=2.0)
        
        if self.serial_port and self.serial_port.is_open:
            port_name = self.serial_port.port
            self.serial_port.close()
            self.log(f"Disconnected from {port_name}", "INFO")
        
        self.serial_port = None
        self.read_thread = None
    
    def is_connected(self) -> bool:
        """Check if connected"""
        return self.serial_port is not None and self.serial_port.is_open
    
    def send(self, data: str) -> bool:
        """Send data to serial port"""
        if not self.is_connected():
            self.log("Not connected", "ERROR")
            return False
        
        try:
            # Add CRLF if not present
            if not data.endswith('\r\n'):
                data += '\r\n'
            
            self.serial_port.write(data.encode('utf-8'))
            self.log(f"→ Sent: {data.strip()}", "DEBUG")
            
            # Call TX callback for UART log
            if self.on_tx_callback:
                self.on_tx_callback(data.strip())
            
            return True
            
        except serial.SerialException as e:
            self.log(f"Send failed: {e}", "ERROR")
            return False
    
    def send_raw(self, data: bytes) -> bool:
        """Send raw binary data to serial port (no encoding, no CRLF)"""
        if not self.is_connected():
            self.log("Not connected", "ERROR")
            return False
        
        try:
            self.serial_port.write(data)
            # Add CRLF terminator
            self.serial_port.write(b'\r\n')
            
            self.log(f"→ Sent raw: {data.hex().upper()}", "DEBUG")
            
            # Call TX callback for UART log (show hex representation)
            if self.on_tx_callback:
                self.on_tx_callback(f"[RAW] {data.hex().upper()}")
            
            return True
            
        except serial.SerialException as e:
            self.log(f"Send raw failed: {e}", "ERROR")
            return False
    
    def send_command(self, command: str, timeout: float = 5.0) -> Optional[str]:
        """Send command and wait for response"""
        if not self.is_connected():
            return None
        
        # Clear queue
        while not self.response_queue.empty():
            try:
                self.response_queue.get_nowait()
            except queue.Empty:
                break
        
        self.response_buffer = ""
        
        if not self.send(command):
            return None
        
        # Wait for response
        start_time = time.time()
        response = ""
        
        while time.time() - start_time < timeout:
            try:
                data = self.response_queue.get(timeout=0.1)
                response += data
                
                # Check for CFSC response completion
                if "CFSC_RESP:END" in response:
                    return response
                    
            except queue.Empty:
                continue
        
        if response:
            return response
        
        self.log("Response timeout", "WARNING")
        return None
    
    def _read_loop(self):
        """Background thread for reading serial data"""
        while self.running and self.serial_port and self.serial_port.is_open:
            try:
                if self.serial_port.in_waiting > 0:
                    data = self.serial_port.read(self.serial_port.in_waiting)
                    decoded = data.decode('utf-8', errors='ignore')
                    
                    self.response_queue.put(decoded)
                    
                    if self.on_data_callback:
                        self.on_data_callback(decoded)
                else:
                    time.sleep(0.01)
                    
            except serial.SerialException:
                break
            except Exception as e:
                self.log(f"Read error: {e}", "ERROR")
                break
    
    @property
    def port(self) -> Optional[str]:
        """Get current port name"""
        if self.serial_port:
            return self.serial_port.port
        return None
