# Changes: Code Quality & Mode-Specific Labels

## Phase 1: Critical Fixes

### 1A. Consolidate MAX_PRINTERS + GPIO defs
- Moved `MAX_PRINTERS` to `app_config.h`, removed duplicates from `main.cpp` and `usb_host_task.cpp`
- Added GPIO 35 (LED output) and GPIO 39 (input TBD) pin definitions
- Centralized mode switch pins (MODE_PIN1/2) in `app_config.h`, removed local defines from `mode_switch.cpp`

### 1B. Fix WiFi event handler blocking event loop 30s
- Replaced `vTaskDelay(30000)` in disconnect handler with `esp_timer_start_once()` one-shot timer
- Timer callback resets retry count and reconnects without blocking the event loop

### 1C. Fix SNTP never starting if WiFi times out
- Moved `sntp_init_()` from `wifi_init()` success path to `IP_EVENT_STA_GOT_IP` handler
- Added `s_sntp_initialized` guard so SNTP only initializes once
- Changed `s_connected`, `s_ever_connected`, `s_sntp_synced` to `std::atomic<bool>`

### 1D. Fix LCD override race condition
- Added `portMUX_TYPE s_lcd_mux` spinlock
- `lcd_override()` writes protected by `taskENTER_CRITICAL`/`taskEXIT_CRITICAL`
- `lcd_update_task` copies override strings to local buffers under spinlock before I2C writes

### 1E. Fix printer array race condition
- Added `s_printer_mutex` (FreeRTOS mutex) in `main.cpp`
- `on_printer_event()` body wrapped in mutex
- `print_task` holds mutex from `get_active_printer()` through `brother_ql_print()`
- Removed duplicated `s_printer_dev_handles[]`/`s_num_printers` from `usb_host_task.cpp`

### 1F. Fix printer disconnect breaking mode mapping
- Disconnect clears slot in-place instead of shifting array
- Connect finds first empty slot instead of appending
- Removed `s_num_printers` counter; uses `connected`/`model` state per slot

### 1G. Make buzzer non-blocking
- Added `buzzer_melody_t` enum and `buzzer_note_t` note sequence arrays
- Created buzzer task (2KB stack) with queue
- `buzzer_beep_good/bad/sad` enqueue melody and return immediately
- Task drains queue to play latest melody

### 1H. Fix USB transfer freed while pending
- Added `cancel_pending_transfer()`: halt → flush → wait for callback → clear endpoint
- Applied to both `printer_send()` and `printer_read_status()` timeout paths

## Phase 2: Brother QL Correctness

### 2A. Fix false success after failed status reads
- Changed `return true` after 3 failed reads to `return false` with "NO RESPONSE" error
- Changed end-of-loop fallthrough to `return false` with "NO RESPONSE"

### 2B. Replace VLAs with fixed-size arrays
- `uint8_t raster_row[MAX_BYTES_PER_ROW]` (90) and `uint8_t row_buf[3 + MAX_BYTES_PER_ROW]`
- Added bounds check: early return false if `bytes_per_row > MAX_BYTES_PER_ROW`

### 2C. Add bounds checking to s_cmd_buf
- `build_init_cmd` and `build_print_setup` now take `buf_size` parameter
- `configASSERT(pos <= buf_size)` at end of each builder

### 2D. Named protocol constants
- Defined `constexpr` constants for all command bytes, status header, status types, media types, valid flags
- Replaced raw hex literals throughout `brother_ql.cpp`

## Phase 3: Cleanup

### 3A. Remove dead code
- Deleted `button.cpp` and `button.h`
- Removed from `CMakeLists.txt`
- Removed LVGL font configs from `sdkconfig.defaults` (lines 15-18)

### 3B. Fix stale comments and names
- `label_renderer.h:5`: "Initialize LVGL" → "Initialize the label renderer"
- `label_renderer.h:9` + `.cpp`: renamed parameter `card_id` → `name`

### 3C. Add ESP_ERROR_CHECK to unchecked init calls
- `mode_switch.cpp` — `gpio_config()`
- `main.cpp` — button `gpio_config()`
- `buzzer.cpp` — `ledc_timer_config`, `ledc_channel_config`
- `wifi_manager.cpp` — `nvs_flash_erase`, `esp_wifi_*`, `esp_event_*`, `esp_netif_init`
- `rfid_reader.cpp` — `uart_driver_install`, `uart_param_config`, `uart_set_pin`

## New Feature: Mode-Specific Labels

### Mode 1: Name Label (unchanged)
- Scan card → print name label with logo + name + date

### Mode 2: Short-Term Parking Permit
- Scan card → print immediately with 2-day validity
- Label: "Short Term / Parking Permit" + name + From/To dates
- LCD shows "SHORT TERM PKG" when idle

### Mode 3: Long-Term Parking Permit
- Scan card → LED on GPIO 35 flashes at 600ms interval
- LCD shows "LONG TERM PERMIT" + "Nd to MM/DD"
- Short button press cycles days: 3→4→5→6→7→3...
- Long press (>1s) prints the permit
- 30-second auto-cancel timeout
- LCD shows "LONG TERM PKG" when idle

### New files
- `led_output.h` / `led_output.cpp` — GPIO 35 output driver
- Added `label_renderer_render_parking()` for parking permit layout
- Added `led_output.cpp` to `CMakeLists.txt`

## Build Impact
- RAM: 23.7% (was ~23.7%, +80 bytes from new state)
- Flash: 58.4% (was 58.3%, +2KB from parking renderer)
