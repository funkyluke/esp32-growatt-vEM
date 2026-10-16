"""Capture the ESP32 console for N seconds and write it to a UTF-8 text file.

Usage (inside the ESP-IDF environment):
    python tools/serial_capture.py COM10 45 serial.log

It pulses RTS to reset the chip first, so the log includes the whole boot.
Close any running ESP-IDF Monitor before using it (it holds the port).
"""
import re
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 40
out_path = sys.argv[3] if len(sys.argv) > 3 else "serial.log"

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()

# hard reset via RTS (EN), same as esptool/idf_monitor
ser.rts = True
time.sleep(0.1)
ser.rts = False

ansi = re.compile(rb"\x1b\[[0-9;]*m")
buf = bytearray()
end = time.time() + seconds
while time.time() < end:
    buf += ser.read(4096)
ser.close()

text = ansi.sub(b"", bytes(buf)).decode("utf-8", errors="replace")
with open(out_path, "w", encoding="utf-8") as f:
    f.write(text)
print(f"captured {len(text)} chars -> {out_path}")
