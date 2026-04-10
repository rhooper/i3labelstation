#!/bin/bash
# Build ESPHome firmware with USB-Serial-JTAG workaround and flash
set -e
cd "$(dirname "$0")/.."
VENV="$(pwd)/../.venv/bin"
PORT="${1:-/dev/tty.wchusbserial5AE60214651}"

echo "=== Step 1: ESPHome codegen + initial compile (will fail on linker) ==="
$VENV/esphome compile labelscanstation.yaml || true

echo "=== Step 2: Patch USE_LOGGER_USB_SERIAL_JTAG ==="
sed -i '' 's/^#define USE_LOGGER_USB_SERIAL_JTAG$/\/\/ #define USE_LOGGER_USB_SERIAL_JTAG/' \
  .esphome/build/labelscanstation/src/esphome/core/defines.h

echo "=== Step 3: Compile via platformio ==="
cd .esphome/build/labelscanstation
$VENV/platformio run
cd ../../..

echo "=== Step 4: Flash ==="
$VENV/esptool.py --chip esp32s3 --port "$PORT" --baud 460800 \
  write_flash 0x10000 .esphome/build/labelscanstation/.pio/build/labelscanstation/firmware.bin

echo "=== Done! Power cycle the board. ==="
