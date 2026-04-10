# Label Scan Station

## Project Overview
ESP32-S3 based label printer (Brother QL-500) + future RFID card reader for i3 Detroit makerspace.

## Current State (2026-04-10)
USB host detection is **working** with the ESP-IDF test firmware. ESPHome integration is in progress — the ESPHome `usb_host` component needs a patched `usb_host_component.cpp` to initialize the USB PHY for OTG Host mode.

### What Works
- Brother QL-500 detected via ESP-IDF test firmware (`esp32/usb_test/`) — VID: 0x04F9, PID: 0x2015, Full Speed
- Hotplug detection works (unplug/replug printer after boot)
- ESPHome component compiles and runs (WiFi, button, NTP, fonts, images)
- Python brother_ql prints successfully from the Mac
- USB PHY init and `skip_phy_setup` approach proven working in test firmware

### What's Left — Next Steps
1. **ESPHome USB detection not working**: The ESPHome `usb_host` override (`esp32/components/usb_host/usb_host_component.cpp`) has the PHY init code but the printer isn't being detected. The ESP-IDF test firmware works with the same hardware. Need to debug why — likely the override `.cpp` isn't being compiled, or there's an ordering issue. **Compare the sdkconfig and PHY init between the two builds.**
2. **ESPHome print flow**: Once printer is detected, test render → encode → USB send pipeline
3. **USB-Serial-JTAG logger conflict**: ESPHome's logger always defines `USE_LOGGER_USB_SERIAL_JTAG` on ESP32-S3. Workaround: patch `defines.h` after codegen, compile via `platformio run`. Needs upstream fix.
4. **Boot-time detection**: Printer connected before boot isn't detected (hotplug works). May need debounce or delayed re-scan.
5. **RFID reader integration**: Future — RC522 or PN532 via SPI/I2C

### Debugging Plan
Full analysis and three approaches (A/B/C) documented in `.claude/plans/resilient-puzzling-marble.md`.

**Most likely root causes (in order):**
1. `USE_LOGGER_USB_SERIAL_JTAG` define in ESPHome logger claims the USB PHY before our OTG host init
2. `enable_hubs: true` in YAML may interfere with direct device detection
3. Wildcard USBClient (VID=0x0000) from `usb_host: devices:` may compete

**Quick fixes to try first (Plan A):**
- Remove `enable_hubs: true` and `devices:` from `usb_host:` in YAML
- Add `dump_config()` to USBHost override to show PHY status in WiFi API logs
- Verify `defines.h` patch took effect in the flashed binary

**If Plan A fails → Plan C (hybrid):**
Keep ESPHome for WiFi/buttons/RFID but run USB host in a standalone FreeRTOS task using proven ESP-IDF code from `esp32/usb_test/`, bypassing ESPHome's `usb_host` component entirely.

### Debugging Tip
The ESP-IDF test firmware at `esp32/usb_test/` is the known-good reference. Flash it to verify hardware works:
```bash
cd esp32/usb_test && ../../.venv/bin/platformio run -t upload
```
Then hotplug the printer and monitor serial. Compare its sdkconfig and USB init sequence with the ESPHome build.

## Hardware Setup

### Board: YD-ESP32-23 (2022-V1.3)
- **COM port** (CH340 USB-UART): `/dev/tty.wchusbserial5AE60214651` — programming + serial logs
- **USB port** (USB-C, GPIO19/GPIO20): USB OTG Host — connect printer here via **USB A-to-C adapter**
- Direct GPIO wiring to 19/20 did NOT work (signal integrity issues)
- The board's USB-C "USB" port works with a USB A-to-C adapter
- There's also an unpopulated USB-OTG solder pad on the back (not needed if using USB-C port)

### Brother QL-500
- USB VID: 0x04F9, PID: 0x2015
- Has own AC power supply (self-powered USB device)
- Needs VBUS 5V present to activate USB pull-up (provided by the USB-C port or a powered hub)

## Build Instructions

### Prerequisites
```bash
python3.14 -m venv .venv
.venv/bin/pip install esphome brother_ql littlefs-python
```

### Compile & Flash (ESPHome)
```bash
cd esp32

# Normal compile (will fail on USB_SERIAL_JTAG linker error)
../.venv/bin/esphome compile labelscanstation.yaml

# Workaround: patch defines.h and compile via platformio
sed -i '' 's/^#define USE_LOGGER_USB_SERIAL_JTAG$/\/\/ #define USE_LOGGER_USB_SERIAL_JTAG/' \
  .esphome/build/labelscanstation/src/esphome/core/defines.h
cd .esphome/build/labelscanstation
../../.venv/bin/platformio run
cd ../../..

# Flash
../.venv/bin/esptool.py --chip esp32s3 --port /dev/tty.wchusbserial5AE60214651 --baud 460800 \
  write_flash 0x10000 .esphome/build/labelscanstation/.pio/build/labelscanstation/firmware.bin
```

### Compile & Flash (USB test firmware)
```bash
cd esp32/usb_test
../../.venv/bin/platformio run -t upload
```

### Monitor Logs
```bash
# Via WiFi API (preferred — board IP: 10.19.69.121)
../.venv/bin/esphome logs labelscanstation.yaml --device 10.19.69.121

# Via serial (logger must be set to UART0)
../.venv/bin/python -c "
import serial, time, re
s = serial.Serial('/dev/tty.wchusbserial5AE60214651', 115200, timeout=0.5)
..."
```

## Key Architecture Decisions

### USB PHY Initialization
ESP32-S3 has one USB PHY shared between USB-Serial-JTAG and USB-OTG. For USB Host mode:
1. Disable USB-Serial-JTAG via sdkconfig: `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=n`
2. Explicitly init PHY with `usb_new_phy()` for `USB_OTG_MODE_HOST` **before** `usb_host_install()`
3. Call `usb_host_install()` with `skip_phy_setup = true`

This is implemented in `esp32/components/usb_host/usb_host_component.cpp` (local override of ESPHome's built-in).

### ESPHome Component Structure
- `esp32/components/usb_host/` — Override of ESPHome's usb_host with PHY init fix
- `esp32/components/usb_printer/` — New component for Brother QL printing
  - `__init__.py` — Config schema, label definitions, codegen
  - `usb_printer.h` — BrotherQLPrinter + LabelBuffer classes
  - `usb_printer.cpp` — USB endpoint discovery, bulk transfer, status parsing
  - `brother_ql.cpp` — Brother QL raster protocol encoding

### Display/Rendering
- `LabelBuffer` extends `DisplayBuffer` (separate from printer to avoid diamond inheritance with `Component`)
- User draws via ESPHome display lambda (fonts, images, shapes)
- Framebuffer converted to Brother QL raster: right-justify, flip horizontal, emit row-by-row

### Brother QL Protocol (QL-500 — minimal)
No compression, no mode setting, no cutting, no expanded mode.
Command sequence: invalidate(200×0x00) → init(0x1B 0x40) → status_request(0x1B 0x69 0x53) → media_quality(0x1B 0x69 0x7A + 10 bytes) → margins(0x1B 0x69 0x64 + 2 bytes) → raster_rows(0x67 0x00 + 90 bytes each) → print(0x1A)

## Useful Scripts
- `esp32/scripts/serial_monitor.py [seconds]` — read serial output for N seconds (default 30)
- `esp32/scripts/serial_reset_and_monitor.py [seconds]` — pulse RST and capture boot log
- `esp32/scripts/esphome_build_and_flash.sh [port]` — full ESPHome build with USB-JTAG workaround + flash

## Conventions
- Use `.venv/bin/python` and `.venv/bin/esphome` — never `source activate`
- WiFi creds in `wifi.yaml` and `esp32/secrets.yaml`
- Logger must use `hardware_uart: UART0` (CH340 is on UART0 pins 43/44)
- ESP32 IP on WiFi: 10.19.69.121 (hostname: labelscanstation.local)
