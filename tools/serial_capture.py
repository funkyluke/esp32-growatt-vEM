"""Capture the ESP32 console for N seconds and write it to a UTF-8 text file.

Usage (inside the ESP-IDF environment):
    python tools/serial_capture.py COM10 45 serial.log [--reset]

--reset restarts the chip first (via esptool), so the log includes the whole
boot. The board's native USB port disappears for a moment during a reset;
the capture reopens it. DTR/RTS stay released: asserting them on the native
USB-Serial/JTAG port can hold the chip in reset.
Close any running ESP-IDF Monitor first (it holds the port).
"""
import re
import subprocess
import sys
import time

import serial

args = [a for a in sys.argv[1:] if not a.startswith("--")]
port = args[0] if len(args) > 0 else "COM10"
seconds = float(args[1]) if len(args) > 1 else 40
out_path = args[2] if len(args) > 2 else "serial.log"

if "--reset" in sys.argv:
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", port,
                    "--after", "hard-reset", "chip-id"], capture_output=True)

buf = bytearray()
end = time.time() + seconds
ser = None
while time.time() < end:
    if ser is None:
        try:
            ser = serial.Serial()
            ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
            ser.dtr = False
            ser.rts = False
            ser.open()
        except serial.SerialException:
            ser = None
            time.sleep(0.05)
            continue
    try:
        buf += ser.read(4096)
    except serial.SerialException:
        ser = None
if ser:
    ser.close()

text = re.sub(rb"\x1b\[[0-9;]*m", b"", bytes(buf)).decode("utf-8", errors="replace")
with open(out_path, "w", encoding="utf-8") as f:
    f.write(text)
print(f"captured {len(text)} chars -> {out_path}")
