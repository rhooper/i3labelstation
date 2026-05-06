# Label Scan Station

## Project Overview
ESP32-S3 based label printer (Brother QL series) + RFID card reader for i3 Detroit makerspace. Scans member RFID cards, looks up names, and prints name labels.

## Current State (2026-05-05)
Pure ESP-IDF + PlatformIO firmware at `esp32/labelscanstation/`. ESPHome approach abandoned.

### What Works
- RFID card scanning (125kHz via UART)
- Card lookup from compiled-in database (`cards.tsv` → `card_db.h`)
- Brother QL USB printing with multi-model support (hotplug detection)
- Supported models: QL-500, QL-550, QL-560, QL-570, QL-580N, QL-650TD, QL-700, QL-710W, QL-720NW, QL-800, QL-810W, QL-820NWB
- Up to 3 printers via USB hub (best printer auto-selected by highest USB PID)
- 16x2 HD44780 LCD status display with clock (America/Detroit timezone, auto DST)
- Buzzer feedback: good beep (C7), bad buzz (C4→A♭4), sad beep (C5→G4→E4 when no printer)
- WiFi + SNTP with nightly resync at random time (midnight–5AM)
- 3-position label-mode switch on GPIO 36/37 (NORMAL / SHORT / TODO) — see "Label Modes" below

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
| 35 | Option button (normally open, grounded when pressed) |
| 36 | Mode switch A (gnd in mode 3 only) |
| 37 | Mode switch B (gnd in modes 2 & 3) |
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
| `brother_ql.*` | Brother QL raster protocol encoding (model-aware) |
| `ql_models.*` | Printer model registry (PID → capabilities lookup) |
| `label_renderer.*` | TTF label rendering via stb_truetype (306×991 1-bit) |
| `card_lookup.*` | Card ID → name lookup from compiled-in DB |
| `card_db.h` | Auto-generated from `cards.tsv` (do not edit) |
| `lcd_display.*` | HD44780 LCD driver via PCF8574 I2C, custom chars |
| `buzzer.*` | Passive piezo buzzer via LEDC PWM |
| `rfid_reader.*` | 125kHz RFID reader via UART |
| `button.*` | Boot button driver (ISR-based, unused — polling in main) |
| `status_led.*` | WS2812 RGB status LED |
| `mode_switch.*` | 3-state mode switch on GPIO36/37 |

### LCD Display (16×2)
- Ready: `SCAN CARD <MODE>` / `May  5  20:41:45` — line 0 cycles `SCAN CARD` / `SCAN FOB` every 3s; the right-hand 6 chars hold the current label mode (`NORMAL`, ` SHORT`, `  TODO`).
- Errors (precedence): `NETWORKING...` → `ERR: NO WIFI` → `ERR: NO NTP` → `ERR: MEMBERDB` → `ERR: NO PRINTER` (no mode indicator on error lines)
- Card scan: shows name (3s) or `ERR:UNKNOWN CARD`
- Mode 3 scan: `MODE 3: TODO` / member-name (2s), no print
- Easter egg: "SCAN HAND" (1s every 392s)

### Label Modes

Driven by the GPIO 36/37 mode switch (`mode_switch.cpp`). Mode is captured at scan time so a switch flip during a print does not change the in-flight job.

| Mode | LCD     | Layout |
|------|---------|--------|
| 1    | NORMAL  | Logo + name + email (above date) + date + phone (above time) + time. Date format `Mon-D-YYYY` (e.g. `May-5-2026`). |
| 2    | SHORT   | No logo. Name in date-font (wrapped, top-aligned) + date + time + phone. Print quantity is media-aware (see below). |
| 3    | TODO    | No print — placeholder. Scan is acknowledged with beep + LCD override. |

Mode 2 print quantity (`label_renderer_render` dispatcher):

- Continuous tape: one short layout, ~45 mm of feed (`req.fb_h = render_h / 2` in `main.cpp`).
- Die-cut ≥ 90mm: two stacked short layouts on a single piece, with a dotted cut guide at the midpoint.
- Die-cut < 90mm: one short layout filling the piece.

Switch encoding (each pin pulled up internally; switch contact pulls to ground):

| Mode | A (GPIO 36) | B (GPIO 37) |
|------|-------------|-------------|
| 1    | open        | open        |
| 2    | open        | gnd         |
| 3    | gnd         | gnd         |

### Card Database
- Source: `cards.tsv` (decimal ID + tab + name)
- Generated: `src/card_db.h` via `scripts/gen_card_db.py`
- Runs automatically on each build (PlatformIO pre-build script)
- Only known cards trigger printing; unknown cards get error buzz

### Brother QL Protocol (multi-model)
Command sequence adapts per model: invalidate(200 or 400 × 0x00) → init(0x1B 0x40) → status_request(0x1B 0x69 0x53) → [mode_setting if supported] → [expanded_mode if supported] → media_quality(0x1B 0x69 0x7A + 10 bytes) → [auto_cut if supported] → margins(0x1B 0x69 0x64 + 2 bytes) → raster_rows(0x67 0x00 + 90 bytes each) → print(0x1A)

## Conventions
- Use `.venv/bin/python` and `.venv/bin/platformio` — never `source activate`
- WiFi creds in `src/secrets.h` (gitignored)
- Serial port: `/dev/tty.usbmodem5AE60214651`
- `sdkconfig.defaults` applied via `board_build.cmake_extra_args`; `fullclean` after changes
