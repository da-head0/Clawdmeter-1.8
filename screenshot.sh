#!/bin/bash
# Take a screenshot from the Waveshare AMOLED display via LVGL snapshot.
# Usage: ./screenshot.sh [output.png] [port]
#
# Pure Python (Pillow) — no ffmpeg dependency. Reads w/h from the device
# so the same script works on both 2.16" (480×480) and 1.8" (368×448).

OUTPUT="${1:-screenshot.png}"
PORT="${2:-/dev/ttyACM0}"

echo "Taking screenshot from $PORT..."

python3 - "$PORT" "$OUTPUT" << 'PYEOF'
import sys, struct, serial
from PIL import Image

port_path, out_path = sys.argv[1], sys.argv[2]

# Open without toggling DTR/RTS — those lines are wired to ESP32-S3's
# EN/BOOT, and the default pyserial open pulses them, causing a board
# reset that wipes UsageData and disconnects BLE. Setting both False
# BEFORE open() keeps the board running.
port = serial.Serial()
port.port = port_path
port.baudrate = 115200
port.timeout = 10
port.dtr = False
port.rts = False
port.open()
port.reset_input_buffer()
port.write(b"screenshot\n")
port.flush()

w = h = raw_size = None
while True:
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line.startswith("SCREENSHOT_START"):
        parts = line.split()
        w, h, raw_size = int(parts[1]), int(parts[2]), int(parts[3])
        break
    if line == "SCREENSHOT_ERR":
        print("Device reported screenshot error", file=sys.stderr)
        sys.exit(1)

data = bytearray()
while len(data) < raw_size:
    chunk = port.read(min(4096, raw_size - len(data)))
    if not chunk:
        print(f"Timeout: got {len(data)} of {raw_size} bytes", file=sys.stderr)
        sys.exit(1)
    data += chunk

for _ in range(10):
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line == "SCREENSHOT_END":
        break
port.close()
print(f"Captured {w}x{h} ({len(data)} bytes)")

# RGB565 LE → RGB888. struct.unpack is ~10× faster than byte indexing.
pixels = struct.unpack(f"<{w*h}H", bytes(data))
rgb = bytearray(w * h * 3)
for i, pix in enumerate(pixels):
    r = (pix >> 11) & 0x1F
    g = (pix >> 5)  & 0x3F
    b = pix         & 0x1F
    # 5/6-bit → 8-bit with low-bit replication so 0x1F maps cleanly to 0xFF.
    rgb[i*3    ] = (r << 3) | (r >> 2)
    rgb[i*3 + 1] = (g << 2) | (g >> 4)
    rgb[i*3 + 2] = (b << 3) | (b >> 2)

Image.frombytes("RGB", (w, h), bytes(rgb)).save(out_path)
print(f"Saved: {out_path}")
PYEOF
