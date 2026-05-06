# Label Modes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a 3-position mode switch driving 3 label formats — Mode 1 (NORMAL: existing layout + email + phone + new date format), Mode 2 (SHORT: half-length, no logo, media-aware print quantity), Mode 3 (TODO placeholder, no print).

**Architecture:** New `mode_switch` module polls GPIO 36/37 with debounce; LCD line 0 gains a right-aligned 6-char mode field; renderer API switches to a request struct (`label_render_req_t`) and dispatches to mode-specific sub-renderers; `main.cpp` captures the mode at scan time and short-circuits on Mode 3.

**Tech Stack:** ESP-IDF 5.x, FreeRTOS, PlatformIO, stb_truetype, Brother QL raster protocol, HD44780 LCD (PCF8574 backpack).

**Design doc:** `docs/plans/2026-05-05-label-modes-design.md` (committed `2753bbe`).

**Verification model:** No host-side test framework in this codebase. Each task ends with `pio run` (both `esp32s3` and `test_print` envs) as a compile-time check, plus an on-hardware behavior check where applicable. Manual hardware verification is consolidated at the end of the plan.

---

### Task 0: Worktree setup

**Files:**
- Modify: `/Users/rhooper/Devel/i3/i3labelstation/.gitignore`
- Create: `.worktrees/label-modes/` (the worktree itself)

**Step 1: Add `.worktrees/` to `.gitignore`**

Append to `.gitignore` (after the `# Claude` block):

```
# Worktrees
.worktrees/
```

**Step 2: Commit on main**

```bash
git -C /Users/rhooper/Devel/i3/i3labelstation add .gitignore
git -C /Users/rhooper/Devel/i3/i3labelstation commit -m "$(cat <<'EOF'
Ignore .worktrees/

Per the work-discipline skill, multi-file feature work happens in a
worktree under .worktrees/. Make sure git doesn't try to track those.

Advised-By: Claude <noreply@anthropic.com>
EOF
)"
```

**Step 3: Create the worktree**

```bash
git -C /Users/rhooper/Devel/i3/i3labelstation worktree add .worktrees/label-modes -b feat/label-modes
```

Expected: new branch `feat/label-modes` checked out at `.worktrees/label-modes`.

**Step 4: All subsequent tasks run with `git -C` pointed at the worktree**

Set a working alias mentally:

```
WT=/Users/rhooper/Devel/i3/i3labelstation/.worktrees/label-modes
PIO=/Users/rhooper/Devel/i3/i3labelstation/.venv/bin/platformio
```

Use `git -C "$WT" …` for git ops, edit files at `$WT/...`, and run
`(cd "$WT/esp32/labelscanstation" && "$PIO" run)` for builds.

---

### Task 1: `mode_switch` module

**Files:**
- Create: `$WT/esp32/labelscanstation/src/mode_switch.h`
- Create: `$WT/esp32/labelscanstation/src/mode_switch.cpp`
- Modify: `$WT/esp32/labelscanstation/src/CMakeLists.txt`

**Step 1: Write the header**

`mode_switch.h`:

```c
#pragma once

typedef void (*mode_switch_change_cb_t)(int new_mode);

// Configure GPIO 36 + 37 as inputs with pull-up. Idempotent.
void mode_switch_init(void);

// Sample both pins, return decoded mode 1..3, or 0 if invalid (A=gnd, B=open).
int mode_switch_read(void);

// Last debounced mode (1, 2, or 3). Defaults to 1 until the first stable read.
int mode_switch_current(void);

// Optional: callback invoked from the polling task when current mode changes.
void mode_switch_set_change_cb(mode_switch_change_cb_t cb);

// Spawn the FreeRTOS polling task. Call once after init.
void mode_switch_start_task(void);
```

**Step 2: Write the implementation**

`mode_switch.cpp`:

```c
#include "mode_switch.h"
#include "app_config.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "mode_switch";

#define POLL_INTERVAL_MS 50

static int s_current_mode = 1;
static mode_switch_change_cb_t s_change_cb = nullptr;

void mode_switch_init(void) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << MODE_SWITCH_A_GPIO) | (1ULL << MODE_SWITCH_B_GPIO);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    ESP_LOGI(TAG, "Initialized (A=GPIO%d, B=GPIO%d)", MODE_SWITCH_A_GPIO, MODE_SWITCH_B_GPIO);
}

int mode_switch_read(void) {
    int a = gpio_get_level((gpio_num_t)MODE_SWITCH_A_GPIO);  // 1=open, 0=gnd
    int b = gpio_get_level((gpio_num_t)MODE_SWITCH_B_GPIO);
    if (a == 1 && b == 1) return 1;
    if (a == 1 && b == 0) return 2;
    if (a == 0 && b == 0) return 3;
    return 0;  // invalid (A=0, B=1)
}

int mode_switch_current(void) {
    return s_current_mode;
}

void mode_switch_set_change_cb(mode_switch_change_cb_t cb) {
    s_change_cb = cb;
}

static void mode_switch_task(void *arg) {
    int last = mode_switch_read();
    if (last >= 1 && last <= 3) s_current_mode = last;

    int candidate = last;
    int candidate_count = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        int now = mode_switch_read();
        if (now == 0) {
            candidate_count = 0;
            continue;
        }
        if (now == candidate) {
            if (++candidate_count >= 2 && s_current_mode != candidate) {
                s_current_mode = candidate;
                ESP_LOGI(TAG, "Mode -> %d", s_current_mode);
                if (s_change_cb) s_change_cb(s_current_mode);
            }
        } else {
            candidate = now;
            candidate_count = 1;
        }
    }
}

void mode_switch_start_task(void) {
    xTaskCreate(mode_switch_task, "mode_sw", 2048, nullptr, 4, nullptr);
}
```

**Step 3: Register sources in CMakeLists**

Find the `idf_component_register` block in
`$WT/esp32/labelscanstation/src/CMakeLists.txt` and add `mode_switch.cpp`
to its SRCS list. Read the file first to see the exact format, then add
the source there.

**Step 4: Build to verify it compiles**

```bash
(cd "$WT/esp32/labelscanstation" && "$PIO" run)
```

Expected: both `esp32s3` and `test_print` SUCCESS.

**Step 5: Commit**

```bash
git -C "$WT" add esp32/labelscanstation/src/mode_switch.h \
                 esp32/labelscanstation/src/mode_switch.cpp \
                 esp32/labelscanstation/src/CMakeLists.txt
git -C "$WT" commit -m "$(cat <<'EOF'
Add mode_switch module for GPIO 36/37 polling

Polls every 50 ms with 2-tick debounce, exposes mode_switch_current()
for callers, and fires an optional callback on changes. Encoding per the
design doc: A=B=open -> 1, A=open/B=gnd -> 2, A=B=gnd -> 3, A=gnd/B=open
ignored (mid-detent transient).

Advised-By: Claude <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: Wire mode_switch + LCD line-0 indicator

**Files:**
- Modify: `$WT/esp32/labelscanstation/src/main.cpp` — call `mode_switch_init` + `mode_switch_start_task`; install change callback that nudges the LCD task; rewrite the line-0 formatter to append a right-aligned 6-char mode field when the steady-state message is `SCAN CARD`.

**Step 1: Read main.cpp to find the LCD update task**

Read the LCD task in `main.cpp`. Find where line 0 is set to `"SCAN CARD"`.

**Step 2: Add the mode-aware line-0 helper**

In `main.cpp`, near the LCD task code, add:

```c
static const char *mode_label(int m) {
    switch (m) {
        case 1: return "NORMAL";
        case 2: return " SHORT";
        case 3: return "  TODO";
        default: return "      ";
    }
}

// 16 chars: "SCAN CARD <6-char right-aligned mode>"
//           "0123456789012345"
// "SCAN CARD" is 9 chars; one space; 6-char field.
static void format_ready_line0(int mode, char out[17]) {
    snprintf(out, 17, "SCAN CARD %s", mode_label(mode));
}
```

Replace the existing `"SCAN CARD"` literal where it's set on line 0 with
a call to `format_ready_line0(mode_switch_current(), buf)` followed by
`lcd_set_line(0, buf)`.

**Step 3: Add the change callback**

```c
static void on_mode_change(int new_mode) {
    // Wake the LCD task — implementation depends on how the LCD task is
    // structured. If it's polling a queue or notification, send a notification
    // here. If it's a simple periodic loop, do nothing extra; the next tick
    // picks it up.
    (void)new_mode;
}
```

Read the existing LCD task carefully — if it uses
`xTaskNotifyWait` / `xTaskNotify`, plumb a notify here so the indicator
updates within ~50 ms instead of waiting for the next 1 s tick.

**Step 4: Initialize in setup()**

Add to `app_main()` / `setup()`:

```c
mode_switch_init();
mode_switch_set_change_cb(on_mode_change);
mode_switch_start_task();
```

Place the call after `lcd_display_init()` so the LCD is ready when the
first mode change fires.

**Step 5: Build**

```bash
(cd "$WT/esp32/labelscanstation" && "$PIO" run)
```

Expected: SUCCESS for both envs.

**Step 6: Commit**

```bash
git -C "$WT" add esp32/labelscanstation/src/main.cpp
git -C "$WT" commit -m "$(cat <<'EOF'
Wire mode_switch into main; show mode on LCD line 0

main.cpp calls mode_switch_init/start_task at boot and replaces the bare
'SCAN CARD' literal on LCD line 0 with 'SCAN CARD <NORMAL|SHORT|TODO>'
right-aligned in the 16-char display. Mode-change callback wakes the
LCD task so the indicator updates ~50 ms after the user toggles the
switch.

No print-path behavior change in this commit — Mode 3 still prints
normally; later commits add the short-circuit and the mode-1/2 content
changes.

Advised-By: Claude <noreply@anthropic.com>
EOF
)"
```

**On-hardware check:** flash, watch the LCD, toggle the mode switch.
Expect `SCAN CARD NORMAL`, `SCAN CARD  SHORT`, `SCAN CARD   TODO` to
appear within ~150 ms of each detent.

---

### Task 3: Renderer API change (no behavior change)

**Files:**
- Modify: `$WT/esp32/labelscanstation/src/label_renderer.h`
- Modify: `$WT/esp32/labelscanstation/src/label_renderer.cpp`
- Modify: `$WT/esp32/labelscanstation/src/main.cpp`

**Step 1: Replace the header**

`label_renderer.h`:

```c
#pragma once

#include <cstdint>

typedef struct {
    int mode;                 // 1=normal, 2=short
    const char *name;
    const char *email;        // empty string if not present
    const char *phone;        // empty string if not present
    uint16_t fb_w;
    uint16_t fb_h;
    uint8_t  media_type;      // MEDIA_CONTINUOUS / MEDIA_DIE_CUT (from brother_ql.cpp)
    uint8_t  media_length_mm; // 0 for continuous
} label_render_req_t;

void label_renderer_init(void);
const uint8_t *label_renderer_render(const label_render_req_t *req);
uint16_t label_renderer_stride(void);
```

**Step 2: Update the implementation signature**

In `label_renderer.cpp`, change:

```c
const uint8_t *label_renderer_render(const char *name, uint16_t width, uint16_t height) {
```

to:

```c
const uint8_t *label_renderer_render(const label_render_req_t *req) {
    if (!req || !req->name) return nullptr;
    const char *name = req->name;
    uint16_t width = req->fb_w;
    uint16_t height = req->fb_h;
    // ... rest of the existing function body unchanged for now
```

(Email, phone, mode, media_type, media_length_mm are unread until Tasks 4-5.)

**Step 3: Update the caller in `main.cpp`**

Find the existing call:

```c
fb = label_renderer_render(msg.name, render_w, render_h);
```

Replace with:

```c
label_render_req_t req = {};
req.mode = mode_switch_current();           // captured at print time, fine for now
req.name = msg.name;
req.email = msg.email;                       // see step 4 below
req.phone = msg.phone;                       // see step 4 below
req.fb_w = render_w;
req.fb_h = render_h;
req.media_type = profile ? profile->type : 0x0B;        // default die-cut
req.media_length_mm = profile ? profile->length_mm : 0;
fb = label_renderer_render(&req);
```

**Step 4: Plumb email/phone through the print queue**

Look at the print message struct in `main.cpp` (around `msg.name` /
`enqueue_print`). Extend it with `email[LOOKUP_EMAIL_MAX]` and
`phone[LOOKUP_PHONE_MAX]`. Update `enqueue_print` to take and copy them.
Update the scan handler to pass `result.email` and `result.phone` into
`enqueue_print` (the `lookup_result_t` already has them as of `4b7f763`).

**Step 5: Build**

```bash
(cd "$WT/esp32/labelscanstation" && "$PIO" run)
```

Expected: SUCCESS.

**Step 6: Commit**

```bash
git -C "$WT" add esp32/labelscanstation/src/label_renderer.h \
                 esp32/labelscanstation/src/label_renderer.cpp \
                 esp32/labelscanstation/src/main.cpp
git -C "$WT" commit -m "$(cat <<'EOF'
Switch label_renderer to a request struct

Replace label_renderer_render(name, w, h) with
label_renderer_render(const label_render_req_t *) so we can pass mode,
email, phone, and media metadata in one parameter. Behavior unchanged
this commit — mode/email/phone/media fields are populated but not yet
read by the renderer. Plumb email + phone through the print message
struct so they reach the renderer once it consumes them.

Advised-By: Claude <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Mode 1 — email, phone, new date format

**Files:**
- Modify: `$WT/esp32/labelscanstation/src/label_renderer.cpp`

**Step 1: Change the date format**

Find the `strftime` call (around line 291):

```c
strftime(date_str, sizeof(date_str), "%Y-%m-%d", &timeinfo);
```

Replace with:

```c
strftime(date_str, sizeof(date_str), "%b-%-d-%Y", &timeinfo);
```

Note: `%-d` is a GNU/BSD extension. ESP-IDF's newlib supports it; if it
ever doesn't, fall back to manual day formatting (`%b-%d-%Y` and strip
the leading zero in C).

**Step 2: Add the email line above date**

Reuse `date_scale` for email. Position it one date-line-height above the
date in fb_x (closer to the top of the printed label). Skip if
`req->email[0] == '\0'`.

```c
if (req->email[0]) {
    int date_line_h = (int)((ascent - descent) * date_scale + 0.5f);
    int email_fb_x = date_fb_x + date_line_h + 2;
    render_string_rot(req->email, date_scale, email_fb_x, date_fb_x_was_used_for_y_start);
    // Use the same fb_y (left-justified) as the date line.
}
```

(Re-read the existing date rendering carefully: `date_fb_x` is the
baseline x; `text_y_start` was used for the date's fb_y. Use the same
fb_y for the email so both start at the same horizontal position when
reading the label.)

**Step 3: Add the phone line above the time**

Reuse `time_scale` for phone. Position it one time-line-height above the
time. Skip if `req->phone[0] == '\0'`.

```c
if (req->phone[0]) {
    int time_line_h = (int)((ascent - descent) * time_scale + 0.5f);
    int phone_fb_x = time_fb_x + time_line_h + 2;
    int phone_width = measure_string(req->phone, time_scale);
    int phone_fb_y = height - phone_width - right_margin;
    render_string_rot(req->phone, time_scale, phone_fb_x, phone_fb_y);
}
```

Right-align the phone the same way the time is right-aligned.

**Step 4: Gate Mode-1 additions on `req->mode == 1`**

Wrap the email + phone + new-date logic so they only happen when
`req->mode == 1`. Mode 2 will get its own renderer in Task 5; until then
mode 2 still falls through the (now mode-1-only) path with email/phone
suppressed and old date format — that's intentional, mode 2 lands in
Task 5.

Concretely: keep the date format change unconditional (mode 2 also wants
the new format). Gate ONLY email + phone on `req->mode == 1`.

**Step 5: Build**

```bash
(cd "$WT/esp32/labelscanstation" && "$PIO" run)
```

Expected: SUCCESS.

**Step 6: Commit**

```bash
git -C "$WT" add esp32/labelscanstation/src/label_renderer.cpp
git -C "$WT" commit -m "$(cat <<'EOF'
Mode 1: add email + phone lines, switch date to Mon-D-YYYY

Email renders in the date font just above the date (left side of the
printed label). Phone renders in the time font just above the time
(right side). Both are skipped if the lookup result has them empty.
Date format moves from %Y-%m-%d to %b-%-d-%Y (e.g. May-5-2026); mode 2
will inherit this format too.

Advised-By: Claude <noreply@anthropic.com>
EOF
)"
```

**On-hardware check:** boot in mode 1, scan a known card. Verify the
printed label shows email above date, phone above time, and the date
reads as `May-5-2026` (or whatever today's date is in that format).

---

### Task 5: Mode 2 — short renderer + dispatch

**Files:**
- Modify: `$WT/esp32/labelscanstation/src/label_renderer.cpp`
- Modify: `$WT/esp32/labelscanstation/src/main.cpp` (compute `fb_h` for continuous-mode short labels)
- Modify: `$WT/esp32/labelscanstation/src/brother_ql.h` (export `MEDIA_CONTINUOUS` / `MEDIA_DIE_CUT` constants if not already exported — currently `static constexpr` in `.cpp`).

**Step 1: Export media-type constants**

In `brother_ql.h`, add:

```c
#define MEDIA_TYPE_CONTINUOUS 0x0A
#define MEDIA_TYPE_DIE_CUT    0x0B
```

(Or move the existing `static constexpr` from `brother_ql.cpp` into the
header so the renderer can reference the same names. Pick one
convention — `#define` is fine for a 1-byte constant in this codebase
style.)

**Step 2: Refactor the existing render path into `render_normal`**

Move the body of `label_renderer_render` (everything after the framebuffer
clear) into:

```c
static void render_normal(const label_render_req_t *req,
                          int fb_y_start, int fb_y_end);
```

`fb_y_start` / `fb_y_end` define the span of fb_y this layout occupies
(for normal mode, always `[0, fb_h)`). All `height`-relative computations
(e.g., `height - time_width - right_margin`) become
`(fb_y_end - fb_y_start) - …` or use absolute fb_y inside the span.

Top-level `label_renderer_render` becomes a dispatcher:

```c
const uint8_t *label_renderer_render(const label_render_req_t *req) {
    // ... validate, set s_render_*, clear framebuffer ...
    if (req->mode == 2) {
        if (req->media_type == MEDIA_TYPE_DIE_CUT && req->media_length_mm >= 90) {
            int mid = req->fb_h / 2;
            render_short(req, 0, mid);
            render_short(req, mid, req->fb_h);
            draw_cut_guide(mid);
        } else {
            // Continuous: caller already passed a smaller fb_h (~496).
            // Short die-cut: fb_h is whatever the printer reports.
            render_short(req, 0, req->fb_h);
        }
    } else {
        render_normal(req, 0, req->fb_h);
    }
    return s_framebuffer;
}
```

**Step 3: Implement `render_short`**

```c
static void render_short(const label_render_req_t *req,
                         int fb_y_start, int fb_y_end) {
    int span = fb_y_end - fb_y_start;
    // Name in date font, word-wrapped, top-aligned (high fb_x).
    // Date (left) + Phone above Time (right) on the bottom edge (low fb_x).
    // Same time/date format as Mode 1 (%b-%-d-%Y, h:mm am/pm).
    // No logo.
    // ... see render_normal for the wrap-helper pattern, just use date_scale
    //     for the name.
}
```

The wrap loop is the same shape as the existing name-wrap code in
`render_normal` — factor that into a helper if it's getting duplicated.

**Step 4: Implement `draw_cut_guide`**

A faint dotted line across the fb_x axis at the boundary between the two
short layouts. Every Nth fb_x bit set, for a couple of fb_y positions.
Keep it minimal — it's a tear/cut hint, not a feature.

```c
static void draw_cut_guide(int fb_y) {
    for (int x = 5; x < s_render_width - 5; x += 6) {
        set_pixel(x, fb_y);
        set_pixel(x, fb_y + 1);
    }
}
```

**Step 5: Continuous-mode `fb_h` computation in `main.cpp`**

Where `req.fb_h = render_h` is set, special-case continuous-mode 2:

```c
if (req.mode == 2 && req.media_type == MEDIA_TYPE_CONTINUOUS) {
    req.fb_h = render_h / 2;   // ~45 mm of feed
}
```

Pass that smaller `fb_h` to `brother_ql_print` as the raster row count
(see `main.cpp:188` — the call already takes `render_h`; substitute
`req.fb_h` for it after building `req`).

**Step 6: Build**

```bash
(cd "$WT/esp32/labelscanstation" && "$PIO" run)
```

Expected: SUCCESS.

**Step 7: Commit**

```bash
git -C "$WT" add esp32/labelscanstation/src/label_renderer.cpp \
                 esp32/labelscanstation/src/label_renderer.h \
                 esp32/labelscanstation/src/main.cpp \
                 esp32/labelscanstation/src/brother_ql.h
git -C "$WT" commit -m "$(cat <<'EOF'
Mode 2: short label renderer with media-aware quantity

render_short() draws name (date-font, wrapped, top) + date/phone/time
(no logo) into a configurable fb_y span. Dispatcher in
label_renderer_render() picks one layout for short die-cut and
continuous, two stacked layouts (with a dotted cut guide) for die-cut
>=90mm. main.cpp halves fb_h for continuous-mode mode-2 prints so only
~45 mm of tape feeds.

MEDIA_TYPE_CONTINUOUS / MEDIA_TYPE_DIE_CUT promoted from .cpp-private
constants to brother_ql.h so the renderer can branch on them.

Advised-By: Claude <noreply@anthropic.com>
EOF
)"
```

**On-hardware check:**
- Mode 2, die-cut 29×90mm: one piece, two stacked short layouts, faint cut guide between.
- Mode 2, die-cut 29×42mm (if available): one piece, single short layout.
- Mode 2, continuous 29mm: ~45mm tape feeds, single short layout.

---

### Task 6: Mode 3 print short-circuit

**Files:**
- Modify: `$WT/esp32/labelscanstation/src/main.cpp`

**Step 1: Short-circuit the scan handler**

In the card-scan handling path (where `enqueue_print` is called after a
successful lookup), branch on `mode_switch_current()`:

```c
int mode = mode_switch_current();
if (mode == 3) {
    lcd_override(0, "MODE 3: TODO", 2000);
    buzzer_good();   // re-use the existing good-beep helper
    return;          // skip enqueue
}
// existing enqueue path follows
```

The print message struct already gets the mode in Task 3. The renderer
will never be called in mode 3, so the renderer's `if (req->mode == 2)`
branch sees only modes 1 and 2.

**Step 2: Build**

```bash
(cd "$WT/esp32/labelscanstation" && "$PIO" run)
```

Expected: SUCCESS.

**Step 3: Commit**

```bash
git -C "$WT" add esp32/labelscanstation/src/main.cpp
git -C "$WT" commit -m "$(cat <<'EOF'
Mode 3: short-circuit print path with TODO LCD override

A successful card scan in mode 3 produces an LCD override 'MODE 3: TODO'
and a good beep, but no print is enqueued. Placeholder for a future
label format.

Advised-By: Claude <noreply@anthropic.com>
EOF
)"
```

**On-hardware check:** mode 3, scan a known card. LCD shows
`MODE 3: TODO` for ~2 s, beep fires, printer stays idle.

---

### Task 7: CLAUDE.md update

**Files:**
- Modify: `/Users/rhooper/Devel/i3/i3labelstation/CLAUDE.md` (committed on
  the worktree branch — will land on main when the branch is merged).

**Step 1: Update the "Label Layout" section**

Replace the existing 3-bullet layout description with:

```markdown
### Label Modes

Selected by the GPIO 36/37 mode switch — see `mode_switch.cpp`. Indicator
appears right-aligned on LCD line 0:

| Mode | LCD     | Layout                                                  |
|------|---------|---------------------------------------------------------|
| 1    | NORMAL  | Logo + name + email (above date) + date + phone (above time) + time. Date format `Mon-D-YYYY`. |
| 2    | SHORT   | No logo. Name (date-font, wrapped) + date + time + phone. Print quantity is media-aware (see below). |
| 3    | TODO    | No print — placeholder.                                 |

Mode 2 print quantity:

- Continuous tape: one short layout (~45 mm of feed).
- Die-cut ≥ 90mm: two stacked short layouts on a single piece, with a dotted cut guide.
- Die-cut < 90mm: one short layout filling the piece.
```

**Step 2: Update the "What Works" section**

Add to the "What Works" bullet list:

- 3-position label-mode switch on GPIO 36/37 (NORMAL / SHORT / TODO)

**Step 3: Commit**

```bash
git -C "$WT" add CLAUDE.md
git -C "$WT" commit -m "$(cat <<'EOF'
Document the 3 label modes in CLAUDE.md

Replace the single-layout description with a per-mode table covering
NORMAL / SHORT / TODO and the Mode-2 media-quantity rules.

Advised-By: Claude <noreply@anthropic.com>
EOF
)"
```

---

### Task 8: Merge to main

**Step 1: Final build on the worktree**

```bash
(cd "$WT/esp32/labelscanstation" && "$PIO" run)
```

Expected: SUCCESS for both envs.

**Step 2: Walk through the on-hardware checks consolidated**

(See "Verification" below.)

**Step 3: Merge to main**

```bash
cd /Users/rhooper/Devel/i3/i3labelstation
git checkout main
git merge --no-ff feat/label-modes -m "Merge feat/label-modes: 3-mode label rendering"
```

(Or open a PR if the user prefers — confirm with them at that step.)

**Step 4: Clean up the worktree**

```bash
git worktree remove .worktrees/label-modes
git branch -d feat/label-modes
```

---

## Verification (consolidated, run on hardware)

After all commits land:

1. Boot the device. LCD shows `SCAN CARD <mode>` with the correct mode label.
2. Toggle the mode switch through all 3 detents — indicator updates ≤ 150 ms.
3. Mode 1, scan a known card with email + phone: full label prints with
   logo, name (multi-line), email above date, date as `May-5-2026`, phone
   above time, time as `H:MM am/pm`.
4. Mode 1, scan a known card without phone: phone line absent, no gap.
5. Mode 1, scan a known card without email: email line absent.
6. Mode 2, 29×90mm die-cut: one piece prints with two stacked short
   layouts and a faint dotted cut guide.
7. Mode 2, shorter die-cut (e.g., 29×42mm if available): single short
   layout fills the piece.
8. Mode 2, 29mm continuous: ~45 mm feeds with one short layout. Auto-cut
   fires at the end (model-dependent).
9. Mode 3, scan a known card: LCD `MODE 3: TODO`, beep, printer idle.
10. Switch flipped mid-print: in-flight print finishes in its captured
    mode; LCD indicator updates immediately.

If any hardware check fails, that's a bug in the most recent
implementation commit — fix forward; do not amend earlier commits.
