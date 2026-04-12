# Label Scan Station

## Project Overview
ESP32-S3 based label printer (Brother QL-500) + RFID card reader for i3 Detroit makerspace. Scans member RFID cards, looks up names, and prints name labels.

## Current State (2026-04-11)
Pure ESP-IDF + PlatformIO firmware at `esp32/labelscanstation/`. ESPHome approach abandoned.

### What Works
- RFID card scanning (125kHz via UART)
- Card lookup from compiled-in database (`cards.tsv` → `card_db.h`)
- Brother QL-500 USB printing (hotplug detection)
- 16x2 HD44780 LCD status display with clock (America/Detroit timezone, auto DST)
- Buzzer feedback: good beep (C7), bad buzz (C4→A♭4), sad beep (C5→G4→E4 when no printer)
- WiFi + SNTP with nightly resync at random time (midnight–5AM)
- Nyancat easter egg on card 805446808

### What's Left
1. **Boot-time printer detection**: Printer connected before boot isn't detected (hotplug works)
2. **Printer status polling**: OUT OF PAPER, PRINTER ERROR states from Brother QL status reads
3. **OTA updates**: Not yet implemented

## Hardware Setup

### Board: YD-ESP32-23 (2022-V1.3)
- **COM port**: `/dev/tty.usbmodem5AE60214651` — programming + serial logs
- **USB port** (USB-C, GPIO19/GPIO20): USB OTG Host — connect printer here via **USB A-to-C adapter**

### GPIO Assignments
| GPIO | Function |
|------|----------|
| 0 | Boot button (1s hold = test print) |
| 8 | I2C SDA (LCD) — auto-tries swapped |
| 9 | I2C SCL (LCD) — auto-tries swapped |
| 18 | RFID UART RX |
| 19/20 | USB OTG Host |
| 38 | Buzzer (passive piezo, PWM) |
| 43/44 | UART0 (serial logs) |
| 48 | WS2812 RGB LED |

### Brother QL-500
- USB VID: 0x04F9, PID: 0x2015
- Self-powered, needs VBUS 5V for USB pull-up

## Build Instructions

### Prerequisites
```bash
python3.14 -m venv .venv
.venv/bin/pip install platformio pyserial
```

### Compile & Flash
```bash
cd esp32/labelscanstation
../../.venv/bin/platformio run -t upload
```

The build automatically regenerates `src/card_db.h` from `cards.tsv` via `scripts/gen_card_db.py` (PlatformIO pre-build script).

### Monitor Logs
```bash
../../.venv/bin/python -c "
import serial, time
s = serial.Serial('/dev/tty.usbmodem5AE60214651', 115200, timeout=0.5)
while True:
    data = s.read(4096)
    if data: print(data.decode('utf-8', errors='replace'), end='')
"
```

## Key Architecture

### Source Files (`esp32/labelscanstation/src/`)
| File | Purpose |
|------|---------|
| `main.cpp` | App entry, LCD status task, RFID/button event loop |
| `wifi_manager.*` | WiFi STA + SNTP with nightly resync task |
| `usb_host_task.*` | USB host driver with PHY init for OTG mode |
| `usb_printer.*` | USB printer endpoint discovery + bulk transfer |
| `brother_ql.*` | Brother QL raster protocol encoding |
| `label_renderer.*` | TTF label rendering via stb_truetype (306×991 1-bit) |
| `card_lookup.*` | Card ID → name lookup from compiled-in DB |
| `card_db.h` | Auto-generated from `cards.tsv` (do not edit) |
| `lcd_display.*` | HD44780 LCD driver via PCF8574 I2C, custom chars |
| `buzzer.*` | Passive piezo buzzer via LEDC PWM |
| `rfid_reader.*` | 125kHz RFID reader via UART |
| `button.*` | Boot button driver (ISR-based, unused — polling in main) |
| `status_led.*` | WS2812 RGB status LED |

### LCD Display (16×2)
- Ready: `SCAN CARD` / `Apr 11  20:41:45`
- Errors (precedence): `NETWORKING...` → `ERR: NO WIFI` → `ERR: NO NTP` → `ERR: NO PRINTER`
- Card scan: shows name (3s) or `ERR:UNKNOWN CARD`
- Easter eggs: "SCAN HAND" (1s every 392s), nyancat walk animation

### Label Layout (29×90mm die-cut, rotated 90° CCW)
- i3 logo (2× scaled) at top
- Name in large text (111px, ~10.5mm) near top
- Date in medium text (63px, ~6mm) bottom-aligned, left-justified

### Card Database
- Source: `cards.tsv` (decimal ID + tab + name)
- Generated: `src/card_db.h` via `scripts/gen_card_db.py`
- Runs automatically on each build (PlatformIO pre-build script)
- Only known cards trigger printing; unknown cards get error buzz

### Brother QL Protocol (QL-500 — minimal)
No compression, no mode setting, no cutting, no expanded mode.
Command sequence: invalidate(200×0x00) → init(0x1B 0x40) → status_request(0x1B 0x69 0x53) → media_quality(0x1B 0x69 0x7A + 10 bytes) → margins(0x1B 0x69 0x64 + 2 bytes) → raster_rows(0x67 0x00 + 90 bytes each) → print(0x1A)

## Conventions
- Use `.venv/bin/python` and `.venv/bin/platformio` — never `source activate`
- WiFi creds in `src/secrets.h` (gitignored)
- Serial port: `/dev/tty.usbmodem5AE60214651`
- `sdkconfig.defaults` applied via `board_build.cmake_extra_args`; `fullclean` after changes
