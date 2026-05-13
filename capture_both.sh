#!/bin/bash
# Capture both splash and usage screens in a single serial session so the
# board reset that macOS pyserial triggers on open() happens at most ONCE,
# not per-screenshot. Between open and the first capture we wait for the
# daemon to detect the BLE drop and push fresh data back to the board.
#
# Usage: ./capture_both.sh [splash.png] [usage.png] [port] [wait_secs]
#
# Defaults: splash.png usage.png /dev/tty.usbmodem1101 20

SPLASH_OUT="${1:-splash.png}"
USAGE_OUT="${2:-usage.png}"
PORT="${3:-/dev/tty.usbmodem1101}"
WAIT_SECS="${4:-20}"

echo "Opening $PORT (board will reset once)..."

python3 - "$PORT" "$SPLASH_OUT" "$USAGE_OUT" "$WAIT_SECS" << 'PYEOF'
import sys, struct, time, serial
from PIL import Image

port_path, splash_out, usage_out, wait_s = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])

port = serial.Serial()
port.port = port_path
port.baudrate = 115200
port.timeout = 10
port.dtr = False
port.rts = False
port.open()

print(f"Waiting {wait_s}s for daemon to reconnect and push data...")
time.sleep(wait_s)
port.reset_input_buffer()

def capture(label, out_path, screen_name):
    print(f"--- {label} ---")
    port.write(f"screen {screen_name}\n".encode())
    port.flush()
    time.sleep(0.5)
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
            print(f"Device reported screenshot error for {label}", file=sys.stderr)
            return False

    data = bytearray()
    while len(data) < raw_size:
        chunk = port.read(min(4096, raw_size - len(data)))
        if not chunk:
            print(f"Timeout: got {len(data)} of {raw_size} bytes", file=sys.stderr)
            return False
        data += chunk

    for _ in range(10):
        line = port.readline().decode("utf-8", errors="replace").strip()
        if line == "SCREENSHOT_END":
            break

    pixels = struct.unpack(f"<{w*h}H", bytes(data))
    rgb = bytearray(w * h * 3)
    for i, pix in enumerate(pixels):
        r = (pix >> 11) & 0x1F
        g = (pix >> 5)  & 0x3F
        b = pix         & 0x1F
        rgb[i*3    ] = (r << 3) | (r >> 2)
        rgb[i*3 + 1] = (g << 2) | (g >> 4)
        rgb[i*3 + 2] = (b << 3) | (b >> 2)
    Image.frombytes("RGB", (w, h), bytes(rgb)).save(out_path)
    print(f"Saved: {out_path} ({w}x{h})")
    return True

ok_a = capture("splash", splash_out, "splash")
ok_b = capture("usage",  usage_out,  "usage")

port.close()
sys.exit(0 if (ok_a and ok_b) else 1)
PYEOF
