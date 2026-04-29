# Label Scan Station

## Project Overview
ESP32-S3 based label printer (Brother QL series) + RFID card reader for i3 Detroit makerspace. Scans member RFID cards, looks up names, and prints name labels.

## Current State (2026-04-29)
Pure ESP-IDF + PlatformIO firmware at `esp32/labelscanstation/`. ESPHome approach abandoned.

### What Works
- RFID card scanning (125kHz via UART)
- Runtime member database from HelloClub API (fetched over WiFi, refreshed at 10am/10pm)
- Compiled-in fallback cards via `extra_cards.h` (works before WiFi connects)
- Brother QL USB printing with multi-model support (hotplug detection)
- Supported models: QL-500, QL-550, QL-560, QL-570, QL-580N, QL-650TD, QL-700, QL-710W, QL-720NW, QL-800, QL-810W, QL-820NWB
- Up to 3 printers via USB hub
- 16x2 HD44780 LCD status display with clock (America/Detroit timezone, auto DST)
- Buzzer feedback: good beep (C7), bad buzz (C4→A♭4), sad beep (C5→G4→E4 when no printer)
- WiFi + SNTP with nightly resync at random time (midnight–5AM)

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

### Brother QL Printers
- USB VID: 0x04F9 (all models), PIDs vary per model (see `ql_models.cpp`)
- Self-powered, needs VBUS 5V for USB pull-up
- Model capabilities (invalidate count, mode setting, cutting, etc.) auto-detected from PID

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

Pre-build scripts (run automatically):
- `scripts/gen_secrets.py` — generates `src/secrets.h` from `apicreds.yaml` (WiFi + API key)
- `scripts/gen_extra_cards.py` — generates `src/extra_cards.h` from `codes.csv` (fallback cards)

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
| `brother_ql.*` | Brother QL raster protocol encoding (model-aware) |
| `ql_models.*` | Printer model registry (PID → capabilities lookup) |
| `label_renderer.*` | TTF label rendering via stb_truetype (306×991 1-bit) |
| `card_lookup.*` | Card ID → name lookup, runtime API fetch + mutex-protected DB |
| `extra_cards.h` | Auto-generated from `codes.csv` (fallback cards, do not edit) |
| `secrets.h` | Auto-generated from `apicreds.yaml` (WiFi + API key, do not edit) |
| `lcd_display.*` | HD44780 LCD driver via PCF8574 I2C |
| `buzzer.*` | Passive piezo buzzer via LEDC PWM |
| `rfid_reader.*` | 125kHz RFID reader via UART |
| `status_led.*` | WS2812 RGB status LED |
| `app_config.h` | GPIO assignments, label dimensions, shared constants |

### LCD Display (16×2)
- Ready: `SCAN CARD` / `Apr 11  20:41:45`
- Errors (precedence): `NETWORKING...` → `ERR: NO WIFI` → `ERR: NO NTP` → `ERR: NO PRINTER`
- Card scan: shows name (3s) or `ERR:UNKNOWN CARD`
- Easter egg: "SCAN HAND" (1s every 392s)

### Label Layout (29×90mm die-cut, rotated 90° CCW)
- i3 logo (2× scaled) at top
- Name in large text (111px, ~10.5mm) near top
- Date in medium text (63px, ~6mm) bottom-aligned, left-justified

### Card Database
- **Runtime**: Fetched from HelloClub API over WiFi on boot and at 10am/10pm daily
- **Fallback**: Compiled-in `extra_cards.h` (from `codes.csv`) works before WiFi connects
- **Thread safety**: `s_db_mutex` in `card_lookup.cpp` protects all reads/writes; refresh builds
  a new array locally, then swaps it atomically under the lock
- HTTP response capped at 512KB per page to prevent OOM from bad API responses
- Only known cards trigger printing; unknown cards get error buzz
- LCD shows `ERR: MEMBERDB` if DB is empty, `DB OUT OF DATE` if stale (>16h)

### USB Printing — Thread Safety
- `s_printer_mutex` in `main.cpp` protects `s_printers[]` during connect/disconnect
- `print_task` holds the mutex only briefly to get the printer pointer, then releases
  before doing USB I/O — prevents deadlock when a printer disconnects mid-print
- Each `PrinterState` owns its own `xfer_done` semaphore and transfer result fields —
  avoids corruption when multiple printers submit transfers concurrently

### Brother QL Protocol (multi-model)
Command sequence adapts per model: invalidate(200 or 400 × 0x00) → init(0x1B 0x40) → status_request(0x1B 0x69 0x53) → [mode_setting if supported] → [expanded_mode if supported] → media_quality(0x1B 0x69 0x7A + 10 bytes) → [auto_cut if supported] → margins(0x1B 0x69 0x64 + 2 bytes) → raster_rows(0x67 0x00 + 90 bytes each) → print(0x1A)

## Conventions
- Use `.venv/bin/python` and `.venv/bin/platformio` — never `source activate`
- WiFi creds in `src/secrets.h` (gitignored)
- Serial port: `/dev/tty.usbmodem5AE60214651`
- `sdkconfig.defaults` applied via `board_build.cmake_extra_args`; `fullclean` after changes
