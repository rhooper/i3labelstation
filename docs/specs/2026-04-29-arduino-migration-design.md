# v2.0.0 Design: Arduino Framework Migration

**Date:** 2026-04-29  
**Status:** Approved, pending implementation

---

## Context

The current ESP-IDF firmware works but carries ~1,040 lines of infrastructure boilerplate
(WiFi event handling, I2C LCD driver, HTTP event callbacks, LED strip RMT config) that
exist only because ESP-IDF requires manual wiring of every peripheral. The actual domain
logic — USB printer protocol, TTF rendering, card DB, RFID scanning — is unaffected by
framework choice.

Arduino-ESP32 3.x sits on top of ESP-IDF 5.x, meaning all USB host APIs remain
accessible. The migration replaces the infrastructure layer with well-tested libraries
while leaving the custom protocol stack completely unchanged.

This work is versioned as **v2.0.0**. The current ESP-IDF firmware is retroactively v1.x.

---

## Quantitative Analysis

### Before: Current ESP-IDF Codebase

Measured from `esp32/labelscanstation/src/` (binary/vendor headers excluded:
`roboto_bold.h`, `stb_truetype.h`, `i3logo.h`).

| File | Total Lines | Code Lines | Functions | Branches | Includes | Status |
|------|-------------|------------|-----------|----------|----------|--------|
| `app_config.h` | 49 | 24 | 0 | 0 | 2 | STAYS |
| `brother_ql.cpp` | 458 | 356 | 9 | 89 | 8 | STAYS |
| `brother_ql.h` | 31 | 19 | 0 | 0 | 2 | STAYS |
| `buzzer.cpp` | 150 | 118 | 6 | 9 | 7 | SIMPLIFIED |
| `buzzer.h` | 13 | 5 | 0 | 2 | 0 | SIMPLIFIED |
| `card_lookup.cpp` | 344 | 272 | 11 | 57 | 13 | SIMPLIFIED |
| `card_lookup.h` | 30 | 13 | 0 | 1 | 1 | SIMPLIFIED |
| `extra_cards.h` | 10 | 5 | 0 | 0 | 0 | STAYS |
| `label_renderer.cpp` | 303 | 231 | 8 | 46 | 11 | STAYS |
| `label_renderer.h` | 9 | 4 | 0 | 0 | 1 | STAYS |
| `lcd_display.cpp` | 199 | 149 | 15 | 26 | 8 | DELETED |
| `lcd_display.h` | 28 | 8 | 0 | 1 | 1 | DELETED |
| `main.cpp` | 401 | 303 | 12 | 78 | 20 | SIMPLIFIED |
| `ql_models.cpp` | 27 | 23 | 1 | 2 | 2 | STAYS |
| `ql_models.h` | 18 | 14 | 0 | 1 | 1 | STAYS |
| `rfid_reader.cpp` | 85 | 64 | 2 | 7 | 7 | SIMPLIFIED |
| `rfid_reader.h` | 9 | 5 | 0 | 1 | 3 | SIMPLIFIED |
| `status_led.cpp` | 34 | 28 | 2 | 2 | 5 | DELETED |
| `status_led.h` | 7 | 3 | 0 | 0 | 0 | DELETED |
| `usb_host_task.cpp` | 171 | 142 | 6 | 26 | 8 | STAYS |
| `usb_host_task.h` | 16 | 7 | 0 | 1 | 2 | STAYS |
| `usb_printer.cpp` | 246 | 196 | 6 | 44 | 7 | STAYS |
| `usb_printer.h` | 35 | 22 | 0 | 0 | 4 | STAYS |
| `wifi_manager.cpp` | 190 | 146 | 9 | 55 | 17 | DELETED |
| `wifi_manager.h` | 15 | 6 | 0 | 6 | 1 | DELETED |
| **TOTAL** | **2,868** | **2,213** | **87** | **469** | **131** | |

### After: Estimated Post-Refactor

| Category | Files | Before Code Lines | After Code Lines | Δ Lines | Δ Branches |
|----------|-------|-------------------|------------------|---------|------------|
| STAYS | 10 | 1,043 | 1,043 | 0 | 0 |
| SIMPLIFIED | 8 | 780 | ~327 | −453 | −57 |
| DELETED → library calls | 5 | 340 | ~23 | −317 | −61 |
| **TOTAL** | **23** | **2,163** | **~1,393** | **−770 (36%)** | **−118 (25%)** |

### Per-File Reduction Detail

| File | Before | After | Δ | Primary driver |
|------|--------|-------|---|----------------|
| `wifi_manager.cpp` | 190 | deleted | −190 | `WiFi.begin()` + `configTime()` replace 55-branch event state machine |
| `lcd_display.cpp` | 199 | deleted | −199 | `LiquidCrystal_I2C` library replaces manual I2C + HD44780 protocol |
| `status_led.cpp` | 34 | deleted | −34 | `FastLED` replaces RMT driver setup |
| `card_lookup.cpp` | 344 | ~120 | −224 | `HTTPClient` + `ArduinoJson` replace `esp_http_client` event handler + cJSON |
| `main.cpp` | 401 | ~200 | −201 | `setup()`/`loop()` replaces `app_main` + task management boilerplate |
| `buzzer.cpp` | 150 | ~30 | −120 | `ledcWriteTone()` replaces LEDC timer/channel config |
| `rfid_reader.cpp` | 85 | ~40 | −45 | `Serial1` replaces `uart_driver_install()` |

### Summary

| Metric | Before | After | Reduction |
|--------|--------|-------|-----------|
| Code lines | 2,213 | ~1,393 | −820 (37%) |
| Branch count (complexity proxy) | 469 | ~351 | −118 (25%) |
| Function count | 87 | ~57 | −30 (34%) |
| Source files | 25 | ~20 | −5 (20%) |

The "irreducible core" — USB host, Brother QL protocol, TTF rendering, model registry —
is 1,043 lines and doesn't change. The refactor removes code that shouldn't have been
written by hand in the first place.

---

## Architecture Decision

**Framework:** Arduino-ESP32 3.x (via PlatformIO, `framework = arduino`)

Arduino-ESP32 3.x wraps ESP-IDF 5.x. All USB host headers (`usb/usb_host.h`) remain
accessible from Arduino sketches, so the USB printer stack carries over unchanged.

### Libraries

| Library | Version | Replaces |
|---------|---------|---------|
| `marcoschwartz/LiquidCrystal_I2C` | ^1.1.4 | `lcd_display.cpp/.h` |
| `fastled/FastLED` | ^3.7.0 | `status_led.cpp/.h` |
| `bblanchon/ArduinoJson` | ^7.0.0 | cJSON sections of `card_lookup.cpp` |
| `WiFi.h` (built-in) | — | `wifi_manager.cpp/.h` |
| `HTTPClient.h` (built-in) | — | `esp_http_client` sections of `card_lookup.cpp` |

---

## What Stays Unchanged

These files carry over verbatim — framework-agnostic or calling ESP-IDF APIs directly:

- `brother_ql.cpp/.h` — Brother QL raster protocol
- `usb_printer.cpp/.h` — USB bulk transfer
- `usb_host_task.cpp/.h` — USB PHY init + client event task
- `ql_models.cpp/.h` — Printer model registry
- `label_renderer.cpp/.h` — TTF rendering via stb_truetype
- `app_config.h` — GPIO assignments, label dimensions, shared constants
- `extra_cards.h`, `roboto_bold.h`, `i3logo.h`, `stb_truetype.h` — data/vendor headers

---

## What Gets Deleted

| File | Replaced by |
|------|-------------|
| `wifi_manager.cpp/.h` | `WiFi.h` + `configTime()` inline in `main.cpp` |
| `lcd_display.cpp/.h` | `LiquidCrystal_I2C` library |
| `status_led.cpp/.h` | `FastLED` library |

---

## What Gets Rewritten

### `platformio.ini`
```ini
framework = arduino
lib_deps =
    marcoschwartz/LiquidCrystal_I2C @ ^1.1.4
    fastled/FastLED @ ^3.7.0
    bblanchon/ArduinoJson @ ^7.0.0
```
`sdkconfig.defaults` and `board_build.cmake_extra_args` may still be required for USB OTG
PHY configuration — verify during bring-up.

### `src/version.h` (new)
```cpp
#pragma once
#define FIRMWARE_VERSION "2.0.0"
#define FIRMWARE_MAJOR 2
#define FIRMWARE_MINOR 0
#define FIRMWARE_PATCH 0
```

### `main.cpp`
- `app_main()` → `setup()` + `loop()`
- WiFi: `WiFi.begin(WIFI_SSID, WIFI_PASS)`
- NTP: `configTime(0, 0, "pool.ntp.org"); setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);`
- LCD: `LiquidCrystal_I2C lcd(0x27, 16, 2); lcd.init(); lcd.backlight();`
- LED: `FastLED.addLeds<WS2812, RGB_LED_GPIO, GRB>(leds, 1);`
- Status checks: `WiFi.status() == WL_CONNECTED`, `time(nullptr) > 1000000000UL`
- Print task, LCD update task, dedup logic, button handling — unchanged

### `card_lookup.cpp`
- `esp_http_client` event handler (~100 lines) → `HTTPClient::GET()` + `getString()` (~10 lines)
- cJSON tree walk (~80 lines) → `ArduinoJson::deserializeJson()` (~15 lines)
- Pagination loop, mutex-protected DB swap, background refresh task — unchanged

### `rfid_reader.cpp`
- `uart_driver_install()` → `Serial1.begin(9600, SERIAL_8N1, RFID_UART_RX_GPIO, -1)`
- `uart_read_bytes()` → `Serial1.available()` + `Serial1.read()`

### `buzzer.cpp`
- `ledc_timer_config()` + `ledc_channel_config()` → `ledcAttach(BUZZER_GPIO, freq, 10)`
- `ledc_set_duty()` → `ledcWriteTone()` / `ledcWrite(..., 0)`
- Melody arrays (good beep, bad buzz, sad beep) — unchanged

---

## LCD Note

The current `lcd_display.cpp` auto-tries swapped SDA/SCL if the first I2C scan fails.
`LiquidCrystal_I2C` does not do this. Wire LCD to GPIO8=SDA, GPIO9=SCL per `app_config.h`
and confirm address (0x27 or 0x3F) during bring-up.

---

## Boot Sequence (v2.0.0)

```
setup():
  FastLED init → dim white
  LCD init → "LabelStation v2" / "Starting..."
  printer_init() × MAX_PRINTERS
  s_printer_mutex, s_print_queue created
  card_lookup_init()        ← loads extra_cards immediately
  buzzer_init()
  WiFi.begin() + configTime()
  label_renderer_init()
  usb_host_set_printer_callback(on_printer_event)
  usb_host_init()
  Serial1.begin() for RFID
  xTaskCreate(print_task)
  xTaskCreate(lcd_update_task)
  FastLED → green

loop():
  if WiFi connected and DB not yet loaded → card_lookup_refresh()
  poll RFID queue
  poll button
  delay(10)
```

---

## Verification Checklist

1. `platformio run` compiles with no errors
2. Flash → LCD shows `"LabelStation v2"` at boot
3. WiFi connects → serial log shows member fetch success
4. Scan known RFID card → name on LCD, label prints correctly
5. Scan unknown card → error buzz + `ERR:UNKNOWN CARD` on LCD
6. Disconnect printer mid-print → no deadlock, `PRINT FAILED!` on LCD
7. Reconnect printer → next scan prints successfully
8. Hold boot button 1s → test print fires
9. Git tag `v2.0.0` on merge commit

---

## Branch Strategy

1. Merge `fix/design-issues` → `main` first (thread-safety fixes valid regardless of framework) ✅ Done
2. Create `feature/v2-arduino` from updated `main`
3. Implement in order: `platformio.ini` → USB compile check → `main.cpp` → `card_lookup.cpp` → `rfid_reader.cpp` → `buzzer.cpp` → delete `wifi_manager`, `lcd_display`, `status_led`
