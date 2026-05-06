# Label Modes — Design Document

**Date**: 2026-05-05
**Status**: Approved

## Background

The label scan station prints a single fixed format today: 29×90mm die-cut
with logo, name, date, and time (`label_renderer.cpp`). Operators want
runtime control over what gets printed, selected by the 3-position mode
switch already wired to `MODE_SWITCH_A_GPIO` (36) and `MODE_SWITCH_B_GPIO`
(37) (defines added to `app_config.h:46-50`).

The HelloClub `email` and `mobile` fields were brought into the firmware in
commit `4b7f763` and are available on every successful card lookup as
`lookup_result_t.email` / `.phone`.

## Modes

| Mode | LCD Tag | Behavior |
|------|---------|----------|
| 1 | `NORMAL` | Logo + name + email + date + time + phone, on full 29×90mm |
| 2 | `SHORT`  | No logo. Name (date-font, wrapped) + date + time + phone. Half-length |
| 3 | `TODO`   | Placeholder. Card scans accepted but no print enqueued |

Switch encoding (each pin pulled up; switched contact pulls to ground):

| Mode | A (GPIO 36) | B (GPIO 37) |
|------|-------------|-------------|
| 1    | open        | open        |
| 2    | open        | gnd         |
| 3    | gnd         | open        |

(The original spec said position 3 grounds both pins; on-hardware testing
showed the switch is a standard SP3T where each detent grounds at most one
pin. `(0, 0)` never appears in practice — treat it as invalid.)

## Mode 1 — NORMAL

Identical to the current layout plus:

- **Email** below the existing name area, in the same font as the date
  (63 px), positioned just *above* the date on the printed label.
- **Phone** in the same font as the time (42 px), just *above* the time.
- **Date format** changes from `%Y-%m-%d` to `%b-%-d-%Y` — e.g. `May-5-2026`
  (3-letter month, no zero-pad on day).

If a member has no email or phone in HelloClub, the corresponding line is
omitted (the firmware stores them as empty strings — see `card_lookup.cpp`
where `cJSON_GetStringValue` falling back to `nullptr` becomes `""`).

## Mode 2 — SHORT

Half-length layout. Contents top-to-bottom (when reading the label):

- Name in the date-font (63 px), word-wrapped, top-aligned.
- Date (left), time (right) on the bottom edge.
- Phone above the time, in the time-font (42 px).

Media-driven print quantity:

| Loaded media          | Behavior                                          |
|-----------------------|---------------------------------------------------|
| Continuous tape (29mm)| One print job; ~45mm feeds                        |
| Die-cut ≥ 90mm        | One print job stacking 2 short layouts on the piece, with a faint cut guide between them |
| Die-cut < 90mm        | One print job, single short layout filling the piece |

The renderer chooses based on `media_profile_t.type` (`MEDIA_CONTINUOUS` /
`MEDIA_DIE_CUT`) and `length_mm` queried per print via
`brother_ql_query_media()` (cached in `s_media_profiles[]` in `main.cpp`).

## Mode 3 — TODO

LCD shows `TODO` in the right-aligned mode field. Card scans are
acknowledged (LCD override + good beep) but no print is enqueued. Acts as
a placeholder for a future label format.

## LCD line 0 layout

When the steady-state line-0 is `SCAN CARD`, append a right-aligned 6-char
mode field:

```
SCAN CARD NORMAL    (mode 1)
SCAN CARD  SHORT    (mode 2)
SCAN CARD   TODO    (mode 3)
```

Error / override messages (`NETWORKING…`, `ERR: NO WIFI`, `ERR: NO NTP`,
`ERR: NO PRINTER`, `ERR:UNKNOWN CARD`, success card name overrides) keep
their full-line layout — the mode field only appears alongside the
ready-state `SCAN CARD` text.

## Architecture

### New module: `mode_switch`

Location: `esp32/labelscanstation/src/mode_switch.{h,cpp}`

Public API:

```c
typedef void (*mode_change_cb_t)(int new_mode);

void mode_switch_init(void);
int  mode_switch_read(void);            // raw sample: 1, 2, 3, or 0 if invalid
int  mode_switch_current(void);         // last debounced mode
void mode_switch_set_change_cb(mode_change_cb_t cb);  // optional
void mode_switch_start_task(void);
```

Implementation: a small FreeRTOS task polls every 50 ms. A new reading must
match the previous reading before it's promoted to `current_mode` (2-tick
debounce ≈ 100 ms). On change, the registered callback fires (used by
`main.cpp` to wake the LCD update task immediately).

Polling chosen over GPIO interrupts because (a) switch transitions are
infrequent and (b) the rest of the codebase is task-driven, so interrupts
would add complexity for no real responsiveness gain.

### Renderer API change

```c
typedef struct {
    int mode;                 // 1 or 2 (mode 3 never reaches the renderer)
    const char *name;
    const char *email;        // empty string if not present
    const char *phone;        // empty string if not present
    uint16_t fb_w;
    uint16_t fb_h;
    uint8_t  media_type;      // MEDIA_CONTINUOUS / MEDIA_DIE_CUT
    uint8_t  media_length_mm; // 0 for continuous
} label_render_req_t;

const uint8_t *label_renderer_render(const label_render_req_t *req);
```

The current `label_renderer_render(name, w, h)` signature retires (only
caller is `main.cpp`).

Internal split:
- `render_normal(req, fb_y_start, fb_y_end)` — current layout + email + phone.
- `render_short(req, fb_y_start, fb_y_end)` — short layout.

Top-level `label_renderer_render` clears the framebuffer and calls the
right sub-renderer. For mode 2 die-cut ≥90mm, it calls `render_short`
twice with `[0, fb_h/2)` and `[fb_h/2, fb_h)`, then draws a dotted cut
guide near `fb_y = fb_h/2`.

### Print dispatch

In `main.cpp` scan-handling path:

1. `mode = mode_switch_current()`
2. If `mode == 3`: LCD override `MODE 3: TODO` for ~2 s, good beep, return —
   skip enqueue.
3. Otherwise build a `label_render_req_t` from the lookup result and the
   cached `s_media_profiles[slot]`, and enqueue.

The mode is captured at scan time, not at print time — a switch flip
during a print does not retroactively change that print.

## Files modified

- `esp32/labelscanstation/src/mode_switch.h`         — NEW
- `esp32/labelscanstation/src/mode_switch.cpp`       — NEW
- `esp32/labelscanstation/src/CMakeLists.txt`        — register sources
- `esp32/labelscanstation/src/label_renderer.h`      — new struct + sig
- `esp32/labelscanstation/src/label_renderer.cpp`    — split + new content
- `esp32/labelscanstation/src/main.cpp`              — init + LCD + dispatch
- `CLAUDE.md`                                        — modes section + layout
- `docs/plans/2026-05-05-label-modes-design.md`      — this doc

## Build / commit sequence

Implementation will run inside a worktree:

```
git worktree add .worktrees/label-modes -b feat/label-modes
```

Verify `.worktrees/` is in `.gitignore` first; add + commit if not.

Commits, each ending `Advised-By: Claude <noreply@anthropic.com>`:

1. mode_switch module + CMakeLists registration
2. main.cpp wiring + LCD line-0 indicator
3. Renderer API change (no behavior change)
4. Mode 1 content (email + phone + new date format)
5. Mode 2 content (short layout, 2-up dispatch, continuous handling)
6. Mode 3 print short-circuit
7. CLAUDE.md update

After each commit: `pio run` for both `esp32s3` and `test_print` envs.

## Verification

End-to-end checks performed against the physical hardware:

1. With no scan: toggle the switch through all 3 positions; LCD line 0
   updates ≤ 150 ms after each detent.
2. Mode 1 print: known card scanned. Inspect the printed label for:
   logo, multi-line name, email line above date, date as `May-5-2026`,
   phone above time, time as `H:MM am/pm`.
3. Mode 1 with a member who has no phone: phone line is omitted, layout
   reflows without a gap.
4. Mode 2 with 29×90mm die-cut: a single piece prints with two stacked
   short layouts, no logo, faint cut guide between halves.
5. Mode 2 with 29×42mm (or other shorter die-cut): a single piece with
   one short layout filling the available height.
6. Mode 2 with 29mm continuous tape: ~45 mm of tape feeds with a single
   short layout; tape is cut at the end of the print job (auto-cut on the
   model in use).
7. Mode 3: scan acknowledged via LCD override + beep, printer remains idle.
8. Switch flipped mid-print: the in-flight print finishes in its captured
   mode; the LCD indicator updates immediately.
