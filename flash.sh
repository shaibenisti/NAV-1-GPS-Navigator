#!/usr/bin/env bash
# Quick flash of the ready-made NAV-1 firmware on macOS / Linux (no ESP-IDF, no build).
#   ./flash.sh                    # auto-detect the port
#   ./flash.sh /dev/ttyUSB0       # choose the port
set -e
cd "$(dirname "$0")"
IMAGE=$(ls docs/firmware/NAV1-*-full.bin | sort | tail -n 1)
PORT=${1:-}
if [ -z "$PORT" ]; then
  PORT=$(ls /dev/ttyUSB* /dev/ttyACM* /dev/cu.usbserial* /dev/cu.wchusbserial* 2>/dev/null | head -n 1 || true)
fi
[ -n "$PORT" ] || { echo "board not found - connect it with USB-C or pass the port as an argument"; exit 1; }
python3 -m esptool version >/dev/null 2>&1 || python3 -m pip install --user --quiet esptool
echo "flashing $IMAGE to $PORT ..."
python3 -m esptool --chip esp32s3 -p "$PORT" -b 921600 --before default-reset --after hard-reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 "$IMAGE"
echo "done - NAV-1 restarts."
