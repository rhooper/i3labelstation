# Label Scan Station — System Flowchart / FSM

## Boot Sequence

```
POWER ON
  │
  ├─ status_led_init() → dim white
  ├─ lcd_init() → "Starting up..."
  ├─ printer_init() × 3 slots
  ├─ printer_mutex create
  ├─ mode_switch_init() + read
  ├─ led_output_init() → LOW
  ├─ print_queue create (depth 4)
  ├─ card_lookup_init() → load DB
  ├─ buzzer_init() → start buzzer task
  ├─ wifi_init() ─────────────────────────┐
  │   ├─ NVS + netif + event loop         │
  │   ├─ Create retry timer               │
  │   ├─ Register event handlers          │
  │   ├─ esp_wifi_start()                 │
  │   └─ Wait 15s for connection          │
  │       ├─ Connected → GOT_IP handler ──┤
  │       └─ Timeout → retry in bg        │
  ├─ label_renderer_init() → load TTF     │
  ├─ usb_host_init() → start USB tasks    │
  ├─ button GPIO config                   │
  ├─ rfid_init() → start RFID task        │
  ├─ Start print_task                     │
  ├─ Start lcd_update_task                │
  ├─ status_led → green                   │
  └─ Enter MAIN LOOP                     │
                                          │
  ┌───────────────────────────────────────┘
  │ WiFi Event Handler (runs in system task)
  │
  │  STA_START ──→ esp_wifi_connect()
  │
  │  STA_DISCONNECTED
  │   ├─ retries < 10 → esp_wifi_connect()
  │   └─ retries ≥ 10 → start 30s one-shot timer
  │                       └─ timer fires → reset count, reconnect
  │
  │  GOT_IP
  │   ├─ s_connected = true
  │   └─ if !s_sntp_initialized:
  │       ├─ Set TZ (America/Detroit)
  │       ├─ esp_sntp_init()
  │       └─ Start sntp_nightly_task
  │           └─ Loop: pick random 00:00–04:59, sleep, esp_sntp_restart()
```

## Background Tasks

```
┌─────────────────────────────────────────────────────────┐
│ lcd_update_task (500ms loop)                            │
│                                                         │
│  lcd_override active?                                   │
│   ├─ YES → copy override strings (under spinlock)       │
│   │        display ovr[0] on line 1                     │
│   │        ovr[1] non-empty? show it : show clock       │
│   └─ NO  → check get_status_line()                      │
│             ├─ "NETWORKING..." (first boot, no NTP yet)  │
│             ├─ "ERR: NO WIFI"                           │
│             ├─ "ERR: NO NTP"                            │
│             ├─ "ERR: NO PRINTER"                        │
│             └─ null (all OK):                           │
│                 ├─ Easter egg? → "SCAN HAND  MODE"      │
│                 └─ Normal → cycle "SCAN CARD"/"SCAN FOB"│
│                              + mode suffix (NAME/SHORT/ │
│                              LONG) on RHS, every 3s     │
│            Line 2: clock "Apr 12  20:41:45"             │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ print_task (blocks on queue)                            │
│                                                         │
│  Receive print_msg_t from queue                         │
│   ├─ Lock printer_mutex                                 │
│   ├─ get_active_printer() → first connected             │
│   │   └─ none? → "NO PRINTER!" → unlock, continue      │
│   ├─ LCD: "PRINTING..." / name                          │
│   ├─ Render label by type:                              │
│   │   ├─ LABEL_NAME → label_renderer_render(name)       │
│   │   ├─ LABEL_SHORT_PARKING → render_parking(name,d,T) │
│   │   └─ LABEL_LONG_PARKING  → render_parking(name,d,F) │
│   ├─ brother_ql_print(printer, model, framebuffer)      │
│   │   ├─ Send invalidate + init + status request        │
│   │   ├─ Read status → auto-detect media                │
│   │   ├─ Send print setup (mode/media/margins)          │
│   │   ├─ Send 991 raster rows                           │
│   │   ├─ Send print command (0x1A)                      │
│   │   └─ Read completion status (up to 10 attempts)     │
│   │       ├─ 0x01 → success                             │
│   │       ├─ 0x02 → error (format LCD message)          │
│   │       └─ 3 failures / 10 reads → "NO RESPONSE"      │
│   ├─ Unlock printer_mutex                               │
│   └─ Failed? → "PRINT FAILED!" + error on LCD (10s)    │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ buzzer_task (blocks on queue)                           │
│                                                         │
│  Receive melody enum → drain to latest                  │
│   ├─ GOOD: C7 ♩ C7 ♩ (80% gate)                        │
│   ├─ BAD:  C4 ♪ Ab4 ♩                                   │
│   └─ SAD:  C5↓G4↓E4 ♪♪♪ (80% gate)                     │
│  Timer resolution auto-selected per frequency           │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ USB host tasks                                          │
│                                                         │
│  usb_host_lib_task → usb_host_lib_handle_events()       │
│  usb_client_task   → usb_host_client_handle_events()    │
│                                                         │
│  NEW_DEV → open, check VID/PID                          │
│   ├─ Brother QL? → find slot, callback to main          │
│   ├─ Hub? → log, enumerate downstream                   │
│   └─ Other → log, close                                 │
│  DEV_GONE → clear slot, callback to main                │
│   main callback: lock mutex, update s_printers[]        │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ rfid_task (UART polling, 500ms timeout)                 │
│                                                         │
│  Read UART bytes → accumulate in buffer                 │
│  Every 4 bytes → parse as card ID (big-endian)          │
│   └─ Send card_id to rfid_queue                         │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ sntp_nightly_task                                        │
│                                                         │
│  Loop forever:                                          │
│   ├─ Pick random second 00:00:00–04:59:59               │
│   ├─ Check localtime every 60s until target             │
│   ├─ esp_sntp_restart()                                 │
│   └─ Sleep 20h (avoid re-trigger same day)              │
└─────────────────────────────────────────────────────────┘
```

## Main Loop FSM (10ms tick)

```
                    ┌──────────────────────┐
                    │                      │
                    ▼                      │
            ┌──────────────┐              │
            │  MAIN LOOP   │              │
            │  (all modes) │              │
            └──────┬───────┘              │
                   │                      │
    ┌──────────────┼──────────────┐       │
    ▼              ▼              ▼       │
 LT_SELECTING?  Mode changed?  RFID?     │
    │              │              │       │
    │              │              ▼       │
    │              │         Card lookup  │
    │              │          ├─ Unknown → buzzer_bad + "ERR:UNKNOWN CARD"
    │              │          ├─ No printer → buzzer_sad + "NO PRINTER!"
    │              │          └─ Valid ────────────────────┐
    │              │                                       │
    │              ▼                                       │
    │     Cancel LT if active                              │
    │     Show "MODE: xxx" 2s                              │
    │                                                      │
    │                              ┌───────────────────────┘
    │                              │
    │              ┌───────────────┼───────────────┐
    │              ▼               ▼               ▼
    │         Mode 1 (NAME)   Mode 2 (SHORT)  Mode 3 (LONG)
    │              │               │               │
    │         buzzer_good     buzzer_good      buzzer_good
    │         show name       show name        enter LT_SELECTING
    │         enqueue:        "2-DAY PERMIT"   save name/card_id
    │          LABEL_NAME     enqueue:         days=3, timeout=30s
    │                          LABEL_SHORT     update LCD
    │                          days=2          flash LED
    │              │               │               │
    │              └───────┬───────┘               │
    │                      │                       │
    │                      ▼                       │
    │                 ┌──────────┐                  │
    │                 │ print_task│                  │
    │                 │ picks up  │                  │
    │                 │ from queue│                  │
    │                 └──────────┘                  │
    │                                              │
    ▼                                              │
┌──────────────────────────────────────────────────┘
│
│  LT_SELECTING state (Mode 3 only)
│
│  ┌─────────────────────────────────────────┐
│  │                                         │
│  │  LED flashes at 600ms cycle             │
│  │  LCD: "LONG TERM PERMIT"               │
│  │       "Nd to MM/DD"                    │
│  │                                         │
│  │  Timeout (30s no interaction)?          │
│  │   └─ YES → lt_cancel(), "TIMED OUT"    │
│  │                                         │
│  │  Button short press (<1s)?              │
│  │   └─ Cycle days: 3→4→5→6→7→3...        │
│  │      Reset timeout, update LCD          │
│  │                                         │
│  │  Button long press (≥1s)?               │
│  │   └─ enqueue LABEL_LONG_PARKING(days)   │
│  │      lt_cancel() (LED off)              │
│  │      "PRINTING..."                      │
│  │                                         │
│  │  Mode switch changed?                   │
│  │   └─ lt_cancel(), switch mode           │
│  │                                         │
│  └─────────────────────────────────────────┘
│
│  Normal state (Mode 1 or 2, or Mode 3 idle)
│
│  ┌─────────────────────────────────────────┐
│  │  Button long press (≥1s)?               │
│  │   └─ Test print for current mode        │
│  │      Mode 1: LABEL_NAME "TEST"          │
│  │      Mode 2: LABEL_SHORT_PARKING 2d     │
│  │      Mode 3: LABEL_LONG_PARKING 5d      │
│  └─────────────────────────────────────────┘
│
└─→ vTaskDelay(10ms) → back to MAIN LOOP
```

## Deduplication

```
Card scanned → same ID within 10s of last print start?
  ├─ YES → ignore (log only)
  └─ NO  → proceed with enqueue, update last_card_id + timestamp
```

## Printer Slot Management

```
s_printers[0..2] — stable slots (no shifting)

Connect:
  Find first slot where !connected && model==null
   ├─ Found → claim interface, set connected=true
   └─ Full → log warning, close device

Disconnect:
  Find slot by dev_handle match
   └─ Clear in-place: release interface, model=null, connected=false

Active printer selection:
  Return first slot where connected==true (mode-independent)
```

## Concurrency Model

```
┌─────────────┐   ┌────────────┐   ┌─────────────┐
│  main loop  │   │ print_task │   │ USB client  │
│  (app_main) │   │            │   │  callback   │
└──────┬──────┘   └─────┬──────┘   └──────┬──────┘
       │                │                  │
       │ xQueueSend     │ xQueueReceive    │
       │───────────────▶│                  │
       │                │                  │
       │         printer_mutex             │
       │                │◄────────────────▶│
       │                │  (shared lock)   │
       │                │                  │
       │          lcd_mux (spinlock)       │
       │◄──────────────▶│                  │
       │  lcd_override   lcd_update_task   │
       │                                   │
       │         buzzer_queue              │
       │───────────────▶ buzzer_task       │
       │  (fire & forget)                  │
```
