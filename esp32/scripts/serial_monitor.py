#!/usr/bin/env python3
"""Simple serial monitor for ESP32-S3 on the CH340 COM port."""
import serial
import time
import re
import sys

PORT = "/dev/tty.wchusbserial5AE60214651"
BAUD = 115200
DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 30

s = serial.Serial(PORT, BAUD, timeout=0.5, dsrdtr=False, rtscts=False)
print(f"Monitoring {PORT} for {DURATION}s...")
data = b""
end_time = time.time() + DURATION
while time.time() < end_time:
    chunk = s.read(4096)
    if chunk:
        data += chunk
s.close()

text = data.decode("utf-8", errors="replace")
text = re.sub(r"\x1b\[[0-9;]*m", "", text)
for line in text.split("\n"):
    if line.strip():
        print(line)
print(f"--- {len(data)} bytes ---")
